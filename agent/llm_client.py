"""
LLM 客户端 —— 带 Function Calling 的 DeepSeek（OpenAI 兼容）封装。

===========================================================================
为什么本模块要自己定义一个"中立消息格式"
===========================================================================

整个项目有两条协议在跑（DeepSeek 是 OpenAI 兼容；万一日后换 Anthropic
又得写另一套）。Agent 循环不应该关心"工具结果是放在 tool 消息里还是
单独一条"这种协议细节 —— 那是协议的活，不是编排的活。

所以循环内部统一用这套中立格式：

    {"role": "user",      "content": "库存还有多少"}
    {"role": "assistant", "content": "我先查一下", "tool_calls": [ToolCall, ...]}
    {"role": "tool",      "tool_call_id": "call_abc", "content": "{...}",
                          "is_error": False}

到发请求那一刻，由本模块翻译成目标协议的形状：

    to_openai_messages(...)  ->  DeepSeek / OpenAI 的 messages 参数

将来要换到 Anthropic 端点，只要再加一个 `to_anthropic_messages()` ——
**Agent 循环一行都不用改**。

===========================================================================
DeepSeek 的 Function Calling 要点
===========================================================================

DeepSeek 兼容 OpenAI 的工具调用协议：
  · 请求里 tools 是 [{"type":"function","function":{name,description,parameters}}]
  · 回复里 assistant message 带 tool_calls：
        [{"id":"call_xxx","type":"function",
          "function":{"name":..., "arguments":"{...json 字符串...}"}}]
  · 工具结果回灌为 {"role":"tool","tool_call_id":...,"content":...}
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Any, Sequence

from config import DEEPSEEK_BASE_URL, DEEPSEEK_MODEL, MAX_TOKENS, require_deepseek_key


class LLMError(RuntimeError):
    """模型调用失败。Agent 循环据此决定是重试、降级还是中止。"""


# ---------------------------------------------------------------------------
# 中立数据结构
# ---------------------------------------------------------------------------

@dataclass
class ToolCall:
    """模型要求调用的一次工具。arguments 是已经解析好的 dict。"""
    id: str
    name: str
    arguments: dict[str, Any] = field(default_factory=dict)
    raw_arguments: str = ""
    parse_error: str | None = None


@dataclass
class Turn:
    """模型的一次回复。"""
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    stop_reason: str = ""
    input_tokens: int = 0
    output_tokens: int = 0

    @property
    def wants_tools(self) -> bool:
        return bool(self.tool_calls)


class ChatModel:
    """循环只依赖这个接口。测试里换成假的就能完全离线跑。"""
    def chat(self, messages: list[dict[str, Any]], tools: Sequence[dict[str, Any]]) -> Turn:
        ...


# ---------------------------------------------------------------------------
# 消息构造 helper —— 中立格式在产生处就统一
# ---------------------------------------------------------------------------

def user_message(text: str) -> dict[str, Any]:
    return {"role": "user", "content": text}


def assistant_message(turn: Turn) -> dict[str, Any]:
    return {
        "role": "assistant",
        "content": turn.text,
        "tool_calls": list(turn.tool_calls),
    }


def tool_message(call: ToolCall, result: Any, is_error: bool = False) -> dict[str, Any]:
    """工具结果消息。result 会被序列化成 JSON 字符串。"""
    if isinstance(result, str):
        content = result
    else:
        content = json.dumps(result, ensure_ascii=False)
    return {
        "role": "tool",
        "tool_call_id": call.id,
        "name": call.name,
        "content": content,
        "is_error": is_error,
    }


# ---------------------------------------------------------------------------
# 中立格式 -> OpenAI / DeepSeek 协议
# ---------------------------------------------------------------------------

def to_openai_messages(messages: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """把中立消息列表翻译成 OpenAI / DeepSeek 的 messages 参数。

    关键点（OpenAI 协议）：
      · assistant 消息若带 tool_calls，每个 tool_call 的 arguments 必须是
        **JSON 字符串**（不是 dict）。
      · 工具结果用独立的 {"role":"tool","tool_call_id":...} 消息，
        且 tool_call_id 必须与 assistant 的 tool_calls[].id 对应。
    """
    out: list[dict[str, Any]] = []
    for msg in messages:
        role = msg.get("role")

        if role == "tool":
            out.append({
                "role": "tool",
                "tool_call_id": msg.get("tool_call_id") or "",
                "content": msg.get("content", ""),
            })
            continue

        if role == "assistant":
            content = msg.get("content") or ""
            tool_calls = msg.get("tool_calls") or []
            if tool_calls:
                oc = [{
                    "id": tc.id,
                    "type": "function",
                    "function": {
                        "name": tc.name,
                        "arguments": tc.raw_arguments or json.dumps(tc.arguments, ensure_ascii=False),
                    },
                } for tc in tool_calls]
                # OpenAI 要求：带 tool_calls 的 assistant 消息 content 可为空串
                out.append({"role": "assistant", "content": content, "tool_calls": oc})
            else:
                out.append({"role": "assistant", "content": content})
            continue

        # user
        out.append({"role": "user", "content": msg.get("content") or ""})
    return out


def to_openai_tools(tools: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    """把中立工具定义翻译成 OpenAI / DeepSeek 的 tools 参数。

    中立 Tool.to_anthropic() 给的是 {"name","description","input_schema"}，
    这里统一包成 OpenAI 的 {"type":"function","function":{...}} 形状。
    注意：调用方传入的 tools 既可以是中性 dict（含 "parameters"），
    也可以是已带 function 包裹的 dict，这里做兼容。
    """
    result = []
    for t in tools:
        if "function" in t:
            result.append(t)
            continue
        params = t.get("parameters") or t.get("input_schema") or {"type": "object", "properties": {}}
        result.append({
            "type": "function",
            "function": {
                "name": t["name"],
                "description": t.get("description", ""),
                "parameters": params,
            },
        })
    return result


# ---------------------------------------------------------------------------
# 真实实现：DeepSeek（OpenAI 兼容）
# ---------------------------------------------------------------------------

class DeepSeekChatModel:
    """通过 DeepSeek 的 OpenAI 兼容端点调用 deepseek-flash，带工具调用。"""

    def __init__(
        self,
        model: str = DEEPSEEK_MODEL,
        base_url: str = DEEPSEEK_BASE_URL,
        max_tokens: int = MAX_TOKENS,
        temperature: float | None = None,
    ) -> None:
        try:
            from openai import OpenAI
        except ImportError as exc:  # pragma: no cover
            raise LLMError(
                f"没有安装 openai SDK，无法调用 DeepSeek：{exc}\n"
                f"    pip install openai"
            ) from exc

        self.model = model
        self.max_tokens = max_tokens
        self.temperature = temperature
        self._client = OpenAI(api_key=require_deepseek_key(), base_url=base_url)
        self.total_input_tokens = 0
        self.total_output_tokens = 0

    def chat(self, messages: list[dict[str, Any]], tools: Sequence[dict[str, Any]]) -> Turn:
        kwargs: dict[str, Any] = {
            "model": self.model,
            "messages": to_openai_messages(messages),
            "max_tokens": self.max_tokens,
        }
        if tools:
            kwargs["tools"] = to_openai_tools(tools)
            kwargs["tool_choice"] = "auto"
        if self.temperature is not None:
            kwargs["temperature"] = self.temperature

        try:
            resp = self._client.chat.completions.create(**kwargs)
        except Exception as exc:  # openai 的异常类型很多，这里统一转成 LLMError
            raise LLMError(f"DeepSeek 调用失败：{type(exc).__name__}: {exc}") from exc

        return self._parse_response(resp)

    def _parse_response(self, resp: Any) -> Turn:
        usage = getattr(resp, "usage", None)
        turn = Turn(
            stop_reason=str(getattr(resp, "stop_reason", "") or "stop"),
            input_tokens=int(getattr(usage, "prompt_tokens", 0) or 0),
            output_tokens=int(getattr(usage, "completion_tokens", 0) or 0),
        )
        self.total_input_tokens += turn.input_tokens
        self.total_output_tokens += turn.output_tokens

        msg = resp.choices[0].message
        if getattr(msg, "content", None):
            turn.text = msg.content.strip()

        for tc in getattr(msg, "tool_calls", None) or []:
            fn = tc.function
            name = fn.name
            raw = fn.arguments or "{}"
            call = ToolCall(id=str(tc.id), name=name, raw_arguments=raw)
            try:
                call.arguments = json.loads(raw) if raw.strip() else {}
            except json.JSONDecodeError:
                call.arguments = {}
                call.parse_error = f"参数不是合法 JSON：{raw[:200]}"
            turn.tool_calls.append(call)

        return turn

    # ── 流式版本：逐 token 产出，供前端 SSE 实时渲染 ──
    def stream_chat(self, messages: list[dict[str, Any]], tools: Sequence[dict[str, Any]]):
        """生成器，逐块产出流式事件：
            {"kind": "text", "text": <增量文本>}
            {"kind": "turn", "turn": <完整 Turn，含已拼好的 tool_calls>}
            {"kind": "error", "message": <错误>}
        """
        kwargs: dict[str, Any] = {
            "model": self.model,
            "messages": to_openai_messages(messages),
            "max_tokens": self.max_tokens,
            "stream": True,
            "stream_options": {"include_usage": True},
        }
        if tools:
            kwargs["tools"] = to_openai_tools(tools)
            kwargs["tool_choice"] = "auto"

        try:
            stream = self._client.chat.completions.create(**kwargs)
        except Exception as exc:  # openai 异常类型多，统一转 LLMError 文案
            yield {"kind": "error", "message": f"DeepSeek 调用失败：{type(exc).__name__}: {exc}"}
            return

        content_buf: list[str] = []
        tool_acc: dict[int, dict[str, str]] = {}
        finish_reason: str | None = None
        usage = None

        for chunk in stream:
            if getattr(chunk, "usage", None):
                usage = chunk.usage
            if not getattr(chunk, "choices", None):
                continue
            delta = chunk.choices[0].delta
            ctext = getattr(delta, "content", None)
            if ctext:
                content_buf.append(ctext)
                yield {"kind": "text", "text": ctext}
            for tc in getattr(delta, "tool_calls", None) or []:
                idx = getattr(tc, "index", 0) or 0
                slot = tool_acc.setdefault(idx, {"id": "", "name": "", "arguments": ""})
                if getattr(tc, "id", None):
                    slot["id"] = tc.id
                fn = getattr(tc, "function", None)
                if fn is not None:
                    if getattr(fn, "name", None):
                        slot["name"] = fn.name
                    if getattr(fn, "arguments", None):
                        slot["arguments"] += fn.arguments
            fr = getattr(chunk.choices[0], "finish_reason", None)
            if fr:
                finish_reason = fr

        # 拼出完整 Turn（工具调用的增量参数需要合并）
        text = "".join(content_buf)
        calls: list[ToolCall] = []
        for idx in sorted(tool_acc):
            slot = tool_acc[idx]
            raw = slot["arguments"] or "{}"
            args: dict[str, Any] = {}
            parse_error: str | None = None
            try:
                args = json.loads(raw) if raw.strip() else {}
            except json.JSONDecodeError:
                parse_error = f"参数不是合法 JSON：{raw[:200]}"
            calls.append(ToolCall(
                id=slot["id"] or f"call_{idx}", name=slot["name"],
                raw_arguments=raw, arguments=args, parse_error=parse_error,
            ))
        turn = Turn(text=text.strip(), tool_calls=calls, stop_reason=str(finish_reason or "stop"))
        if usage is not None:
            turn.input_tokens = int(getattr(usage, "prompt_tokens", 0) or 0)
            turn.output_tokens = int(getattr(usage, "completion_tokens", 0) or 0)
            self.total_input_tokens += turn.input_tokens
            self.total_output_tokens += turn.output_tokens
        yield {"kind": "turn", "turn": turn}

    def usage_note(self) -> str:
        price_in, price_out = 0.0, 0.0  # deepseek-flash 价格以官网为准
        cost = (self.total_input_tokens * price_in + self.total_output_tokens * price_out)
        return (
            f"累计 {self.total_input_tokens} 输入 + {self.total_output_tokens} 输出 tokens"
            + (f"，≈ ¥{cost:.4f}" if (price_in or price_out) else "")
        )


# ---------------------------------------------------------------------------
# 假模型：不花钱、不需要 Key、不需要网络，就能把整条 Agent 链路跑起来
# ---------------------------------------------------------------------------

class ScriptedChatModel:
    """按剧本返回预设回复的假模型。**只用于演示和测试，不要用于真实诊断。**

    剧本是一串 Turn；每被调用一次就弹出下一个。弹完了就一直返回最后一轮
    的内容（配合 max_rounds 收尾）。
    """

    def __init__(self, script: Sequence[Turn] | None = None) -> None:
        self._script = list(script or [])
        self._index = 0
        self.calls: list[list[dict[str, Any]]] = []

    def chat(self, messages: list[dict[str, Any]], tools: Sequence[dict[str, Any]]) -> Turn:
        self.calls.append([dict(m) for m in messages])
        if not self._script:
            return Turn(text="（假模型没有剧本）", stop_reason="stop")
        if self._index < len(self._script):
            turn = self._script[self._index]
            self._index += 1
        else:
            turn = self._script[-1]
        return turn

    def stream_chat(self, messages: list[dict[str, Any]], tools: Sequence[dict[str, Any]]):
        """演示用的流式版本：把剧本文本切成小块逐块吐出，最后给出完整 Turn。"""
        self.calls.append([dict(m) for m in messages])
        if not self._script:
            yield {"kind": "text", "text": "（假模型没有剧本）"}
            yield {"kind": "turn", "turn": Turn(text="（假模型没有剧本）", stop_reason="stop")}
            return
        turn = self._script[min(self._index, len(self._script) - 1)]
        self._index += 1
        text = turn.text or ""
        for chunk in _chunk_text(text, size=16):
            yield {"kind": "text", "text": chunk}
        yield {"kind": "turn", "turn": turn}


def _chunk_text(text: str, size: int = 16) -> list[str]:
    """把一段文本切成小块，用于模拟打字机式流式输出。"""
    if not text:
        return []
    return [text[i:i + size] for i in range(0, len(text), size)]


if __name__ == "__main__":
    # 翻译层自检：一个 assistant 带两个 tool_calls + 两条 tool 结果
    msgs = [
        user_message("库存还有多少"),
        assistant_message(Turn(
            text="我先查一下",
            tool_calls=[ToolCall(id="c1", name="get_key", arguments={"key": "stock:1"}),
                        ToolCall(id="c2", name="get_qps_report", arguments={})],
        )),
        tool_message(ToolCall(id="c1", name="get_key"), {"value": 42}),
        tool_message(ToolCall(id="c2", name="get_qps_report"), {"qps": 12.3}),
    ]
    rendered = to_openai_messages(msgs)
    print(f"中立消息 {len(msgs)} 条 -> OpenAI 消息 {len(rendered)} 条")
    for m in rendered:
        kind = m["role"]
        if kind == "assistant" and "tool_calls" in m:
            names = [tc["function"]["name"] for tc in m["tool_calls"]]
            print(f"  assistant tool_calls={names}")
        else:
            print(f"  {kind}: {str(m.get('content',''))[:40]}")
