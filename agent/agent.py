"""
库存引擎运维 Agent —— ReAct 循环（本项目最核心的产出物）。

===========================================================================
先说结论：核心循环只有 30 行，剩下都是边界处理
===========================================================================

网上讲 ReAct 的文章会给你这个骨架：

    while True:
        turn = model.chat(messages, tools)
        if not turn.tool_calls:
            return turn.text
        for call in turn.tool_calls:
            result = runner.run(call.name, call.arguments)
        messages.append(assistant_message(turn))
        messages.append(tool_message(call, result))

**这个骨架能跑通演示，但上不了生产。** 因为它假设了：
    · 模型给的参数永远合法      -> 现实中它会漏必填项、传错类型
    · 工具永远调用成功          -> 现实中引擎会挂、key 名会拼错
    · 模型不会原地打转          -> 现实中它会反复调同一个工具
    · 上下文永远不会超长        -> 现实中 JSON 几个回合就撑爆
    · 模型总会停下来            -> 现实中它能转到你账户没钱

本文件大部分代码就是处理这五件事 —— 也是简历上"工具调用失败回灌、
重复调用检测、轮次上限等容错机制"那句话对应的真实工作量。

复用说明：本文件改编自 net-pluse 的 agent.py（同一套 ReAct 骨架），
但换成了库存运维场景、DeepSeek 模型、OpenAI 工具协议，并去掉了长期记忆
（MVP 阶段用不到，多轮上下文由 Session 提供）。这正是计划文档里
"共用 Agent 骨架但场景 / 工具 / 数据源完全不同"的体现。
"""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Sequence

from llm_client import (
    ChatModel,
    DeepSeekChatModel,
    LLMError,
    Turn,
    assistant_message,
    tool_message,
    user_message,
    _chunk_text,
)
from tools import TOOLS, ToolRunner


SYSTEM_PROMPT = """你是一名资深库存系统运维工程师，负责一个自研的 Redis 兼容库存引擎（用于秒杀场景）。
你通过调用工具来查询引擎状态、排查异常、回答运维问题。你面向的是运营 / 运维人员（可能非技术背景）。

【工作方式】
先观察、再推理、然后调用工具验证，而不是凭空下结论。
每次调用工具前，用一句话说明你为什么要调它。
拿到结果后判断：信息够了吗？不够就继续调；够了就给出结论。
不要在同一轮里重复调用参数完全相同的工具。

【必须遵守的规则】
1. 只依据工具返回的数据下结论。数据里没有的信息，直接说"现有数据不足以判断"，
   绝不要用你的一般知识编造具体数值（尤其是库存数字、QPS 数字）。
2. 严格区分【观测到的现象】和【推断的原因】。工具返回的是现象，原因需要证据支撑。
3. 如果不同工具返回的数据相互矛盾，明确指出矛盾，不要强行圆成一个结论。
4. 工具返回结果里若有 "error" 字段，说明这次调用失败了：判断是参数写错了还是引擎真的不可用，然后修正或换思路。
5. 默认只读。唯一的"写"是 simulate_decr（模拟扣减），它会明确标注 SIMULATION 并还原，不要把它当成真实扣减。
   绝不执行删除 key、修改生产配置、真实扣减等危险操作。

【输出格式】
用 Markdown，包含三部分：
  ## 结论 —— 直接回答用户的问题
  ## 依据 —— 列出支撑结论的具体数据（带上数值，必须来自工具返回）
  ## 建议 —— 可执行的下一步操作（如"建议扩容""建议检查热点 key"）
控制在 500 字以内，不要复述工具返回的原始 JSON。"""


@dataclass
class AgentLimits:
    max_rounds: int = 8
    max_tool_calls: int = 16
    max_identical_calls: int = 2
    max_context_chars: int = 24000
    keep_recent_tool_results: int = 4


@dataclass
class ToolExecution:
    call: Any
    result: dict[str, Any]
    is_error: bool = False
    elapsed_ms: float = 0.0
    intervention: str | None = None

    def brief(self, max_chars: int = 160) -> str:
        payload = json.dumps(self.result, ensure_ascii=False)
        if len(payload) > max_chars:
            payload = payload[:max_chars] + "…"
        return payload


@dataclass
class Step:
    round: int
    thought: str
    executions: list[ToolExecution] = field(default_factory=list)


@dataclass
class Diagnosis:
    question: str
    answer: str
    steps: list[Step] = field(default_factory=list)
    rounds: int = 0
    stopped_because: str = "end_turn"
    tool_calls: int = 0
    input_tokens: int = 0
    output_tokens: int = 0
    error: str | None = None
    forced_conclusion: bool = False
    answer_is_model_output: bool = False

    def to_dict(self) -> dict[str, Any]:
        return {
            "question": self.question,
            "answer": self.answer,
            "rounds": self.rounds,
            "tool_calls": self.tool_calls,
            "stopped_because": self.stopped_because,
            "forced_conclusion": self.forced_conclusion,
            "answer_is_model_output": self.answer_is_model_output,
            "error": self.error,
            "usage": {"input_tokens": self.input_tokens, "output_tokens": self.output_tokens},
            "steps": [{
                "round": s.round, "thought": s.thought,
                "tools": [{
                    "name": e.call.name, "arguments": e.call.arguments,
                    "is_error": e.is_error, "elapsed_ms": round(e.elapsed_ms, 1),
                    "intervention": e.intervention, "result": e.result,
                } for e in s.executions],
            } for s in self.steps],
        }

    def trace(self) -> str:
        lines: list[str] = []
        for step in self.steps:
            lines.append(f"── 第 {step.round} 轮 " + "─" * 46)
            if step.thought:
                lines.append(f"💭 {step.thought}")
            for e in step.executions:
                mark = "❌" if e.is_error else "🔧"
                if e.intervention:
                    mark = "⛔"
                args = json.dumps(e.call.arguments, ensure_ascii=False)
                lines.append(f"  {mark} {e.call.name}({args})  [{e.elapsed_ms:.0f}ms]")
                if e.intervention:
                    lines.append(f"     ⚠️  {e.intervention}")
                lines.append(f"     → {e.brief()}")
            lines.append("")
        return "\n".join(lines)


EventHook = Callable[[str, dict[str, Any]], None]


class Session:
    """极简多轮会话：保存历史消息，让"那个 key 呢"这类追问能被理解。

    只存消息，不做向量化 / 长期记忆（那是进阶项）。MVP 够用。
    """

    def __init__(self) -> None:
        self.messages: list[dict[str, Any]] = []

    def add_round(self, question: str, steps: list[Step], answer: str) -> None:
        self.messages.append(user_message(question))
        tools_used: list[str] = []
        for s in steps:
            for e in s.executions:
                tools_used.append(e.call.name)
        summary = answer
        if tools_used:
            summary += f"\n（本轮回合用到的工具：{', '.join(tools_used)}）"
        self.messages.append(assistant_message(Turn(text=summary)))

    def context(self, limit: int = 20) -> list[dict[str, Any]]:
        return self.messages[-limit:]


# 局部导入避免循环依赖
from llm_client import ToolCall  # noqa: E402  （agent.py 与 llm_client 同包，安全）


class InventoryAgent:
    def __init__(
        self,
        model: ChatModel | None = None,
        runner: ToolRunner | None = None,
        limits: AgentLimits | None = None,
        system_prompt: str = SYSTEM_PROMPT,
        on_event: EventHook | None = None,
    ) -> None:
        self._model = model
        self._runner = runner or ToolRunner()
        self.limits = limits or AgentLimits()
        self.system_prompt = system_prompt
        self.on_event = on_event

    @property
    def model(self) -> ChatModel:
        if self._model is None:
            self._model = DeepSeekChatModel()
        return self._model

    @property
    def runner(self) -> ToolRunner:
        return self._runner

    def _emit(self, kind: str, **payload: Any) -> None:
        if self.on_event:
            self.on_event(kind, payload)

    def run(self, question: str, session: Session | None = None) -> Diagnosis:
        lim = self.limits
        self._emit("start", question=question)

        messages: list[dict[str, Any]] = [{"role": "system", "content": self.system_prompt}]
        if session is not None:
            messages.extend(session.context())
        messages.append(user_message(question))

        steps: list[Step] = []
        seen: dict[str, int] = {}
        total_calls = 0
        stopped = "end_turn"
        answer = ""
        err: str | None = None
        forced = False
        real_answer = False

        for round_no in range(1, lim.max_rounds + 1):
            self._trim_context(messages, lim)
            try:
                turn = self.model.chat(messages, self._runner.openai_tools())
            except LLMError as exc:
                stopped = "llm_error"
                err = str(exc)
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=str(exc))
                break
            except Exception as exc:  # noqa: BLE001
                stopped = "llm_error"
                err = f"{type(exc).__name__}: {exc}"
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=err)
                break

            if not turn.wants_tools:
                if turn.text.strip():
                    answer = turn.text
                    real_answer = True
                else:
                    answer = "（模型没有给出结论）"
                    real_answer = False
                stopped = turn.stop_reason or "end_turn"
                self._emit("final", round=round_no, text=answer)
                break

            self._emit("thought", round=round_no, text=turn.text)
            messages.append(assistant_message(turn))
            step = Step(round=round_no, thought=turn.text)
            steps.append(step)
            self._emit("assistant", round=round_no, tool_calls=len(turn.tool_calls))

            for call in turn.tool_calls:
                total_calls += 1
                execution = self._execute(call, seen, lim)
                step.executions.append(execution)
                messages.append(tool_message(call, execution.result, is_error=execution.is_error))
                self._emit(
                    "tool_result", round=round_no, name=call.name,
                    arguments=call.arguments, raw=execution.brief(2000),
                    is_error=execution.is_error, intervention=execution.intervention,
                    elapsed_ms=execution.elapsed_ms,
                )
                if total_calls >= lim.max_tool_calls:
                    stopped = "max_tool_calls"
                    break

            if stopped == "max_tool_calls":
                answer, real_answer = self._force_conclusion(messages, lim)
                forced = True
                break
        else:
            stopped = "max_rounds"

        if stopped == "max_rounds":
            answer, real_answer = self._force_conclusion(messages, lim)
            forced = True

        diagnosis = Diagnosis(
            question=question, answer=answer, steps=steps, rounds=len(steps),
            stopped_because=stopped, tool_calls=total_calls, error=err,
            forced_conclusion=forced, answer_is_model_output=real_answer,
        )
        self._collect_usage(diagnosis)
        if session is not None and real_answer:
            session.add_round(question, steps, answer)
        self._emit("finish", stopped_because=stopped, rounds=len(steps))
        return diagnosis

    # ── 流式版本：逐 token / 逐事件吐给前端，配合 SSE 实时渲染 ──
    def run_stream(self, question: str, session: Session | None = None) -> Diagnosis:
        """与 run() 等价，但通过 on_event 实时流式输出：
          · thought_chunk  —— 模型思考文本（逐块，实时进"执行轨迹"）
          · answer_start / answer_chunk / answer_end —— 最终结论（逐块进"对话"）
          · tool_result    —— 每次工具调用结果
          · error / finish —— 异常与收尾
        复用 run() 的全部容错（轮次上限、重复拦截、参数校验、强制结论）。
        """
        lim = self.limits
        self._emit("start", question=question)

        messages: list[dict[str, Any]] = [{"role": "system", "content": self.system_prompt}]
        if session is not None:
            messages.extend(session.context())
        messages.append(user_message(question))

        steps: list[Step] = []
        seen: dict[str, int] = {}
        total_calls = 0
        stopped = "end_turn"
        answer = ""
        err: str | None = None
        forced = False
        real_answer = False
        answer_buf: list[str] = []  # 收集最终结论文本，供历史记录使用

        def emit_answer(text: str) -> None:
            self._emit("answer_start")
            for ch in _chunk_text(text, size=28):
                self._emit("answer_chunk", text=ch)
            self._emit("answer_end", text=text)

        for round_no in range(1, lim.max_rounds + 1):
            self._trim_context(messages, lim)
            self._emit("round_start", round=round_no)
            try:
                stream = self.model.stream_chat(messages, self._runner.openai_tools())
            except LLMError as exc:
                stopped = "llm_error"
                err = str(exc)
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=err)
                break
            except Exception as exc:  # noqa: BLE001
                stopped = "llm_error"
                err = f"{type(exc).__name__}: {exc}"
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=err)
                break

            turn_text = ""
            turn: Turn | None = None
            llm_err: str | None = None
            try:
                for ev in stream:
                    kind = ev.get("kind")
                    if kind == "text":
                        turn_text += ev["text"]
                        self._emit("thought_chunk", round=round_no, text=ev["text"])
                    elif kind == "error":
                        llm_err = ev["message"]
                    elif kind == "turn":
                        turn = ev["turn"]
            except LLMError as exc:
                llm_err = str(exc)
            except Exception as exc:  # noqa: BLE001
                llm_err = f"{type(exc).__name__}: {exc}"

            if llm_err is not None:
                stopped = "llm_error"
                err = llm_err
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=llm_err)
                break
            if turn is None:
                stopped = "llm_error"
                err = "模型没有返回任何内容"
                answer = self._degraded_answer(steps, err)
                self._emit("error", message=err)
                break

            # 模型决定不再调用工具 —— 出结论
            if not turn.wants_tools:
                if turn.text.strip():
                    answer = turn.text
                    real_answer = True
                else:
                    answer = "（模型没有给出结论）"
                    real_answer = False
                stopped = turn.stop_reason or "end_turn"
                answer_buf.append(answer)
                emit_answer(answer)
                self._emit("final", round=round_no, text=answer)
                break

            # 还有工具要调：把本轮思考落进轨迹，并真正执行
            messages.append(assistant_message(turn))
            step = Step(round=round_no, thought=turn_text)
            steps.append(step)
            self._emit("assistant", round=round_no, tool_calls=len(turn.tool_calls))
            for call in turn.tool_calls:
                total_calls += 1
                execution = self._execute(call, seen, lim)
                step.executions.append(execution)
                messages.append(tool_message(call, execution.result, is_error=execution.is_error))
                self._emit(
                    "tool_result", round=round_no, name=call.name,
                    arguments=call.arguments, raw=execution.brief(2000),
                    is_error=execution.is_error, intervention=execution.intervention,
                    elapsed_ms=execution.elapsed_ms,
                )
                if total_calls >= lim.max_tool_calls:
                    stopped = "max_tool_calls"
                    break

            if stopped == "max_tool_calls":
                ans, real = self._force_conclusion_stream(messages, lim)
                answer, real_answer = ans, real
                answer_buf.append(answer)
                forced = True
                break
        else:
            stopped = "max_rounds"

        if stopped == "max_rounds":
            ans, real = self._force_conclusion_stream(messages, lim)
            answer, real_answer = ans, real
            answer_buf.append(answer)
            forced = True

        diagnosis = Diagnosis(
            question=question, answer=answer, steps=steps, rounds=len(steps),
            stopped_because=stopped, tool_calls=total_calls, error=err,
            forced_conclusion=forced, answer_is_model_output=real_answer,
        )
        self._collect_usage(diagnosis)
        if session is not None and real_answer:
            session.add_round(question, steps, answer)
        self._emit("finish", stopped_because=stopped, rounds=len(steps))
        return diagnosis

    def _force_conclusion_stream(self, messages: list[dict[str, Any]], lim: AgentLimits):
        """达到上限后强制出结论，并流式输出。复用 _force_conclusion 的 nudges。"""
        self._emit("forcing_conclusion")
        nudge = user_message(
            "已达到本轮诊断的工具调用上限。请**不要再调用任何工具**，"
            "直接基于上面已经获得的数据给出结论。如果现有数据不足以判断，就明确说明还缺什么信息。"
        )
        messages.append(nudge)
        try:
            stream = self.model.stream_chat(messages, [])
        except LLMError as exc:
            return f"（达到工具调用上限，且生成结论时失败：{exc}）", False
        except Exception as exc:  # noqa: BLE001
            return f"（达到工具调用上限，且生成结论时失败：{type(exc).__name__}: {exc}）", False

        buf: list[str] = []
        turn: Turn | None = None
        self._emit("answer_start")
        try:
            for ev in stream:
                if ev.get("kind") == "text":
                    buf.append(ev["text"])
                    self._emit("answer_chunk", text=ev["text"])
                elif ev.get("kind") == "turn":
                    turn = ev["turn"]
        except LLMError as exc:
            self._emit("answer_end", text="")
            return f"（达到工具调用上限，且生成结论时失败：{exc}）", False
        if not buf and turn is not None and turn.text.strip():
            buf.append(turn.text)
            self._emit("answer_chunk", text=turn.text)
        text = "".join(buf)
        self._emit("answer_end", text=text)
        if text.strip():
            return text, True
        return "（达到工具调用上限，模型未给出结论）", False

    # ── 单次工具调用：所有容错都在这 ──
    def _execute(self, call: ToolCall, seen: dict[str, int], lim: AgentLimits) -> ToolExecution:
        problem = self._validate_arguments(call)
        if problem:
            return ToolExecution(call=call, result={
                "error": "invalid_arguments", "message": problem,
                "how_to_fix": "请按工具 schema 重新给出参数后再次调用。",
            }, is_error=True, intervention=f"参数非法：{problem}")

        fingerprint = self._fingerprint(call)
        count = seen.get(fingerprint, 0) + 1
        seen[fingerprint] = count
        if count > lim.max_identical_calls:
            return ToolExecution(call=call, result={
                "error": "duplicate_call_blocked",
                "message": f"你已经用完全相同的参数调用过 {call.name} {count-1} 次了，结果不会变。",
                "how_to_fix": "不要再重复调用这个工具，请改用其它工具或直接给结论。",
            }, is_error=True, intervention=f"重复调用被拦截（第 {count} 次）")

        start = time.perf_counter()
        result = self._runner.run(call.name, call.arguments)
        elapsed = (time.perf_counter() - start) * 1000
        is_error = "error" in result

        if count > 1 and not is_error:
            result = dict(result)
            result["_note"] = (
                f"这是第 {count} 次调用 {call.name}，参数与之前完全相同。"
                f"如果这些数据你已经用过了，请换一个工具或直接给结论。"
            )
        return ToolExecution(call=call, result=result, is_error=is_error, elapsed_ms=elapsed)

    @staticmethod
    def _fingerprint(call: ToolCall) -> str:
        try:
            args = json.dumps(call.arguments, sort_keys=True, ensure_ascii=False)
        except (TypeError, ValueError):
            args = repr(call.arguments)
        return f"{call.name}|{args}"

    def _validate_arguments(self, call: ToolCall) -> str | None:
        if call.parse_error:
            return call.parse_error
        spec = next((t for t in TOOLS if t.name == call.name), None)
        if spec is None:
            return f"没有名为 {call.name!r} 的工具。可用工具：{[t.name for t in TOOLS]}"
        props = spec.parameters.get("properties", {})
        required = spec.parameters.get("required", [])
        args = call.arguments or {}

        problems: list[str] = []
        missing = [r for r in required if r not in args]
        if missing:
            problems.append(f"缺少必填参数 {missing}")
        unknown = [k for k in args if k not in props]
        if unknown:
            problems.append(f"出现了未定义的参数 {unknown}。这个工具接受的参数是 {sorted(props)}")
        for key, value in list(args.items()):
            declared = props.get(key, {}).get("type")
            if declared == "integer" and isinstance(value, str):
                if value.lstrip("-").isdigit():
                    args[key] = int(value)
                else:
                    problems.append(f"参数 {key} 应该是整数，收到的是 {value!r}")
            elif declared == "string" and not isinstance(value, str):
                problems.append(f"参数 {key} 应该是字符串，收到的是 {type(value).__name__}")
        return "；".join(problems) if problems else None

    # ── 上下文管理 ──
    def _estimate_chars(self, messages: Sequence[dict[str, Any]]) -> int:
        total = 0
        for m in messages:
            total += len(str(m.get("content") or ""))
            for c in m.get("tool_calls") or []:
                total += len(json.dumps(c.arguments, ensure_ascii=False)) + len(c.name)
        return total

    def _trim_context(self, messages: list[dict[str, Any]], lim: AgentLimits) -> None:
        """上下文超预算时裁剪旧的工具结果（只替换内容，绝不删消息）。"""
        size = self._estimate_chars(messages)
        if size <= lim.max_context_chars:
            return
        tool_indexes = [i for i, m in enumerate(messages) if m.get("role") == "tool"]
        protect = set(tool_indexes[-lim.keep_recent_tool_results:])
        for i in tool_indexes:
            if i in protect:
                continue
            content = messages[i].get("content") or ""
            if content.startswith("[已裁剪"):
                continue
            name = messages[i].get("name") or "工具"
            messages[i]["content"] = (
                f"[已裁剪：{name} 的返回结果，原文 {len(content)} 字符。"
                f"如需重新获取请再次调用该工具。]"
            )
            if self._estimate_chars(messages) <= lim.max_context_chars:
                break

    def _force_conclusion(self, messages: list[dict[str, Any]], lim: AgentLimits) -> tuple[str, bool]:
        self._emit("forcing_conclusion")
        nudge = user_message(
            "已达到本轮诊断的工具调用上限。请**不要再调用任何工具**，"
            "直接基于上面已经获得的数据给出结论。如果现有数据不足以判断，就明确说明还缺什么信息。"
        )
        messages.append(nudge)
        try:
            turn = self.model.chat(messages, [])  # 空 tools = 不允许调用
        except LLMError as exc:
            return f"（达到工具调用上限，且生成结论时失败：{exc}）", False
        except Exception as exc:  # noqa: BLE001
            return f"（达到工具调用上限，且生成结论时失败：{type(exc).__name__}: {exc}）", False
        if turn.text.strip():
            return turn.text, True
        return "（达到工具调用上限，模型未给出结论）", False

    def _degraded_answer(self, steps: list[Step], err: str) -> str:
        collected = [e for s in steps for e in s.executions if not e.is_error]
        if not collected:
            return f"诊断未能完成：{err}"
        lines = ["（模型服务不可用，以下为已经采集到的原始数据，未做分析）", "", f"原因：{err}", ""]
        for e in collected:
            lines.append(f"· {e.call.name}：{e.brief(400)}")
        return "\n".join(lines)

    def _collect_usage(self, diagnosis: Diagnosis) -> None:
        model = self._model
        if hasattr(model, "total_input_tokens"):
            diagnosis.input_tokens = int(getattr(model, "total_input_tokens", 0))
            diagnosis.output_tokens = int(getattr(model, "total_output_tokens", 0))


def diagnose(question: str, **kwargs: Any) -> Diagnosis:
    """一行调用：`diagnose("库存还有多少")`。"""
    return InventoryAgent(**kwargs).run(question)


if __name__ == "__main__":
    from llm_client import ScriptedChatModel
    from offline_demo import DemoToolRunner, default_demo_script

    agent = InventoryAgent(model=ScriptedChatModel(default_demo_script()), runner=DemoToolRunner())
    result = agent.run("库存还有多少")
    print(result.trace())
    print("=" * 60)
    print(result.answer)
