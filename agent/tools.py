"""
库存引擎运维 Agent 工具层 —— 把引擎能力包装成 LLM 可调用的 tool。

===========================================================================
工具设计原则（面试会被问，也是 Agent 好不好用的分水岭）
===========================================================================

1. 一个 tool 对应一个【动作】，不是一个【数据切片】。
   例：热点排行 / QPS 报告都来自 STATS 一条命令，但拆成两个工具——
   因为模型问"QPS"时不应该被迫接收一堆热点 key 噪声。

2. description 里必须写【什么时候调】，不只是【能干什么】。
   只写"返回 QPS"模型不知道该不该调；写清"当用户问当前 QPS / 吞吐 /
   引擎忙不忙时调"它才会在正确的时候调。

3. description 里必须写【和相似工具怎么区分】。
   get_key（查单个 key）和 stock_query（查库存余量）数据同源，语义不同：
   前者通用，后者特指秒杀库存语义。不写清楚模型会瞎挑。

4. 错误也要返回 dict，绝不抛异常。模型看到 {"error": ...} 会自己重试或换思路。

5. key 名支持模糊匹配：模型常常只记得"库存那个 key"而拼不对全名。
   工具层在引擎侧做 KEYS 通配 + 相似度兜底，避免模型因 key 不存在而乱编。
"""

from __future__ import annotations

import difflib
import os
import sys
from dataclasses import dataclass, field
from typing import Any, Callable

from resp_client import RespClient, RespError

_AGENT_DIR = os.path.dirname(os.path.abspath(__file__))


@dataclass
class Tool:
    """一个可被模型调用的工具。parameters 用 JSON Schema（两套协议公共子集）。"""
    name: str
    description: str
    parameters: dict[str, Any]
    method: str
    extra: dict[str, Any] = field(default_factory=dict)

    def to_anthropic(self) -> dict[str, Any]:
        return {"name": self.name, "description": self.description,
                "input_schema": self.parameters, **self.extra}

    def to_openai(self) -> dict[str, Any]:
        return {"type": "function", "function": {
            "name": self.name, "description": self.description,
            "parameters": self.parameters}}


# ---------------------------------------------------------------------------
# 工具定义表
# ---------------------------------------------------------------------------

TOOLS: list[Tool] = [
    Tool(
        name="get_key",
        method="_get_key",
        description=(
            "查询某个 key 的详情：是否存在、当前值、剩余 TTL（秒，<0 表示永不过期）。\n"
            "\n"
            "【什么时候调】用户问『某个具体 key 现在是什么』『这个值对不对』"
            "『这个 key 还剩多久过期』时调用。\n"
            "\n"
            "【和 stock_query 怎么区分】get_key 对任意 key 通用（用户给的 key 名可能不精确，"
            "本工具会做模糊匹配）；stock_query 专指秒杀库存余量，会顺带给『会不会超卖』的判断。"
            "二者都能查库存 key，但 stock_query 多了业务语义。\n"
            "\n"
            "【key 名不确定怎么办】参数不要求精确：传 'stock' 也会被模糊匹配到 'stock:1' 之类的 key，"
            "匹配到多个时会把候选列出来让你确认。"
        ),
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string",
                        "description": "目标 key 名，支持模糊匹配。如 stock:1、库存、product_100。"}
            },
            "required": ["key"],
        },
    ),
    Tool(
        name="hot_keys",
        method="_hot_keys",
        description=(
            "返回访问次数最多的 Top N 热点 key 及其访问计数（来自引擎 STATS）。\n"
            "\n"
            "【什么时候调】用户问『哪些 key 最热』『热点 key 是什么』『访问量最高的是哪个』"
            "『有没有热点集中』时调用。\n"
            "\n"
            "【和 get_key 怎么区分】hot_keys 看全局排行（不需要指定 key）；get_key 看单个指定 key。"
        ),
        parameters={
            "type": "object",
            "properties": {
                "top_n": {"type": "integer", "description": "返回前几名，默认 10，最大 50。"}
            },
            "required": [],
        },
    ),
    Tool(
        name="qps_report",
        method="_qps_report",
        description=(
            "返回引擎运行概览：运行时间、累计命令数、当前连接数、估算 QPS、以及命令类型分布"
            "（GET/INCR/DECR 等各执行了多少次）。\n"
            "\n"
            "【什么时候调】用户问『现在 QPS 多少』『引擎忙不忙』『吞吐多少』"
            "『总共处理了多少请求』『INCR 和 DECR 哪个多』时调用。\n"
            "\n"
            "【和 hot_keys 怎么区分】qps_report 看引擎整体负载与命令分布；hot_keys 看具体哪些 key 被访问得多。"
        ),
        parameters={"type": "object", "properties": {}, "required": []},
    ),
    Tool(
        name="stock_query",
        method="_stock_query",
        description=(
            "查询秒杀商品的剩余库存（即某个库存 key 的当前整数值）。\n"
            "\n"
            "【什么时候调】用户明确在问『某商品还剩多少库存』『会不会超卖』『库存够不够』时调用。\n"
            "\n"
            "【和 get_key 怎么区分】stock_query 专指库存语义，会额外判断『当前值是否 <= 0（已售罄）』"
            "以及『是否出现过负数（扣减异常）』；get_key 只是通用地返回 key 的值。\n"
            "\n"
            "【key 名不确定怎么办】同 get_key，支持模糊匹配。"
        ),
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string",
                        "description": "库存 key 名（支持模糊匹配），如 stock:1 或 库存。"}
            },
            "required": ["key"],
        },
    ),
    Tool(
        name="simulate_decr",
        method="_simulate_decr",
        description=(
            "【模拟】验证一次『扣减库存』的逻辑是否正确：执行 DECR 后再执行 INCR 还原，"
            "返回扣减前 / 扣减后 / 还原后的三个数值。\n"
            "\n"
            "【重要】这是只读验证，不会真正减少库存——DECR 之后一定跟着 INCR 还原。"
            "用于在不动生产数据的前提下演示『先扣减、再判断返回值、不足则回滚』的标准秒杀流程。\n"
            "\n"
            "【什么时候调】用户问『扣减是怎么做的』『DECR 后怎么判断够不够』『负数怎么办』"
            "『能不能演示一下扣减』时调用。绝不要用它做真实扣减（那是生产路径，本 Agent 默认只读）。\n"
            "\n"
            "【和 stock_query 怎么区分】stock_query 只看当前值；simulate_decr 演示一次完整扣减-还原过程。"
        ),
        parameters={
            "type": "object",
            "properties": {
                "key": {"type": "string",
                        "description": "要模拟扣减的库存 key（支持模糊匹配）。"}
            },
            "required": ["key"],
        },
    ),
    Tool(
        name="knowledge_qa",
        method="_knowledge_qa",
        description=(
            "在【运维知识库】（命令使用手册 + 排障经验）里做混合检索，返回最相关的资料片段。\n"
            "\n"
            "【什么时候调】两类场景：\n"
            "  1. 命令怎么用 ——『DECR 返回负数是什么意思』『EXPIRE 怎么用』『TTL 返回 -1 是什么』\n"
            "  2. 排障经验 ——『QPS 突然掉了可能为什么』『热点 key 太多怎么办』『AOF 积压怎么处理』\n"
            "\n"
            "【和引擎工具怎么区分】引擎工具（get_key/stock_query/qps_report/hot_keys）查的是"
            "【实时数据】；这个工具查的是【通用知识和经验】，数据来自本地知识库，不连引擎。\n"
            "\n"
            "【query 怎么写】带上限定词更好，如『DECR 返回 -1 表示什么』远好于『DECR』。"
        ),
        parameters={
            "type": "object",
            "properties": {
                "query": {"type": "string",
                          "description": "检索问题，用自然语言。带命令名 / 现象词效果更好。"},
                "top_k": {"type": "integer", "description": "返回资料条数，默认 5。"}
            },
            "required": ["query"],
        },
    ),
]

TOOL_NAMES: list[str] = [t.name for t in TOOLS]


# ---------------------------------------------------------------------------
# 工具执行层
# ---------------------------------------------------------------------------

class ToolRunner:
    """持有引擎连接与检索层，把 (name, arguments) 分发到具体实现。

    两条铁律（同 net-pluse）：
      1. run() 永远返回 dict，永远不抛异常。
      2. 所有外部连接都是懒加载。
    """

    def __init__(self, client: RespClient | None = None) -> None:
        self._client = client
        self._rag: Any | None = None
        self._by_name = {t.name: t for t in TOOLS}

    # ---------------- 注册表 ----------------
    @property
    def tools(self) -> list[Tool]:
        return TOOLS

    def anthropic_tools(self) -> list[dict[str, Any]]:
        return [t.to_anthropic() for t in TOOLS]

    def openai_tools(self) -> list[dict[str, Any]]:
        return [t.to_openai() for t in TOOLS]

    # ---------------- 懒加载外部依赖 ----------------
    @property
    def client(self) -> RespClient:
        if self._client is None:
            from config import ENGINE_HOST, ENGINE_PORT, ENGINE_TIMEOUT
            self._client = RespClient(ENGINE_HOST, ENGINE_PORT, ENGINE_TIMEOUT)
        return self._client

    @property
    def rag(self) -> Any:
        if self._rag is None:
            if _AGENT_DIR not in sys.path:
                sys.path.insert(0, _AGENT_DIR)
            from rag import build_retriever
            self._rag = build_retriever()
        return self._rag

    # ---------------- 分发 ----------------
    def run(self, name: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        arguments = arguments or {}
        tool = self._by_name.get(name)
        if tool is None:
            return {"error": f"没有名为 {name!r} 的工具。", "available_tools": TOOL_NAMES}
        try:
            return getattr(self, tool.method)(**arguments)
        except RespError as exc:
            return {"error": f"引擎返回错误：{exc}"}
        except TypeError as exc:
            return {"error": f"参数不匹配：{exc}",
                    "correct_parameters": sorted(tool.parameters.get("properties", {})),
                    "required": tool.parameters.get("required", [])}
        except Exception as exc:  # 兜底
            return {"error": f"{type(exc).__name__}: {exc}"}

    # ---------------- key 模糊匹配 ----------------
    def _resolve_key(self, key: str) -> tuple[str | None, list[str]]:
        """用引擎 KEYS 做模糊匹配，返回 (精确或最佳匹配 key, 候选列表)。"""
        try:
            all_keys = self.client.keys("*")
        except RespError as exc:
            return None, [f"引擎不可用：{exc}"]
        if not all_keys:
            return None, []
        # 1) 精确（忽略大小写）命中
        for k in all_keys:
            if k.lower() == key.lower():
                return k, [k]
        # 2) 子串命中
        subs = [k for k in all_keys if key.lower() in k.lower()]
        if len(subs) == 1:
            return subs[0], subs
        # 3) 模糊相似度
        close = difflib.get_close_matches(key, all_keys, n=5, cutoff=0.3)
        if len(close) == 1:
            return close[0], close
        # 多个候选时返回 None 让模型/用户确认
        return None, (subs or close or all_keys[:10])

    # ---------------- 各工具实现 ----------------
    def _get_key(self, key: str) -> dict[str, Any]:
        resolved, candidates = self._resolve_key(key)
        if resolved is None:
            return {
                "error": "没有唯一匹配到该 key",
                "you_typed": key,
                "candidates": candidates[:10],
                "how_to_fix": "请从候选里挑一个精确 key 名再查，或先调 hot_keys / KEYS 看看有哪些 key。",
            }
        exists = self.client.exists(resolved)
        ttl = self.client.ttl(resolved)
        value = self.client.get(resolved) if exists else None
        return {
            "key": resolved,
            "exists": bool(exists),
            "value": value,
            "ttl_seconds": ttl,  # -1=永不过期, -2=不存在
            "value_is_integer": (value is not None and value.lstrip("-").isdigit()),
        }

    def _hot_keys(self, top_n: int = 10) -> dict[str, Any]:
        top_n = max(1, min(int(top_n or 10), 50))
        stats = self.client.stats(top_n)
        hot = stats.get("hot_keys", [])
        return {
            "top_n": top_n,
            "hot_keys": hot,
            "note": "来自引擎 STATS 的实时访问计数；计数越大说明越热。",
        }

    def _qps_report(self, **_kwargs: Any) -> dict[str, Any]:
        stats = self.client.stats(0)
        return {
            "uptime_sec": stats.get("uptime_sec"),
            "total_commands": stats.get("total_commands"),
            "connections": stats.get("connections"),
            "qps_estimate": stats.get("qps_estimate"),
            "qps_window_secs": stats.get("qps_window_secs"),
            "cmd_counts": stats.get("cmd_counts", {}),
            "note": "qps_estimate = 最近若干秒命令数 / 秒数，是估算值。",
        }

    def _stock_query(self, key: str) -> dict[str, Any]:
        resolved, candidates = self._resolve_key(key)
        if resolved is None:
            return {
                "error": "没有唯一匹配到库存 key",
                "you_typed": key,
                "candidates": candidates[:10],
                "how_to_fix": "请挑一个精确 key 名再查。",
            }
        value = self.client.get(resolved)
        result: dict[str, Any] = {
            "key": resolved,
            "exists": value is not None,
            "remaining_stock": None,
            "is_integer": False,
        }
        if value is not None and value.lstrip("-").isdigit():
            n = int(value)
            result["remaining_stock"] = n
            result["is_integer"] = True
            result["sold_out"] = (n <= 0)
            if n < 0:
                result["warning"] = (
                    "剩余库存为负数，说明出现过『扣减超过实际库存』的异常"
                    "（DECR 没做余额校验就减了）。这是排障里要重点关注的现象。"
                )
        else:
            result["warning"] = "该 key 的值不是整数，可能不是库存计数。"
        return result

    def _simulate_decr(self, key: str) -> dict[str, Any]:
        resolved, candidates = self._resolve_key(key)
        if resolved is None:
            return {
                "error": "没有唯一匹配到库存 key",
                "you_typed": key,
                "candidates": candidates[:10],
                "how_to_fix": "请挑一个精确 key 名再演示。",
            }
        try:
            before = self.client.get(resolved)
            decr = self.client.send_and_read("DECR", resolved)   # 模拟扣减
            after = self.client.get(resolved)
            restore = self.client.send_and_read("INCR", resolved)  # 还原
            final = self.client.get(resolved)
        except RespError as exc:
            return {"error": f"模拟扣减失败（已尝试还原）：{exc}"}
        return {
            "key": resolved,
            "mode": "SIMULATION（仅演示，已还原，未真正减少库存）",
            "before": before,
            "after_decr": decr,
            "after_restore": restore,
            "final": final,
            "note": (
                "标准秒杀扣减流程：DECR 后立刻读返回值判断。若返回值 < 0 说明库存不足"
                "（应回滚或拒绝）；>=0 才放行订单。本工具用 INCR 把数据还原，"
                "不影响生产库存。若期间引擎有真实流量，final 可能不等于 before。"
            ),
        }

    def _knowledge_qa(self, query: str, top_k: int = 5) -> dict[str, Any]:
        try:
            hits = self.rag.retrieve(query, top_k=int(top_k or 5))
        except RuntimeError as exc:
            return {"error": f"知识库检索不可用：{exc}"}
        except ImportError as exc:
            return {"error": f"检索层不可用：{exc}（可能缺少 faiss 或索引未建）"}
        if not hits:
            return {"hits": [], "note": "没有检索到相关资料。"}
        return {
            "hits": [{
                "chunk_id": h.chunk.chunk_id,
                "text": h.text[:1200],
                "source": h.chunk.metadata.get("topic") or h.chunk.metadata.get("source"),
            } for h in hits],
            "note": "资料来自本地知识库（命令手册 + 排障经验）的混合检索结果。",
        }


if __name__ == "__main__":
    runner = ToolRunner()
    print(f"已注册 {len(runner.tools)} 个工具：\n")
    for t in runner.tools:
        required = t.parameters.get("required", [])
        optional = [k for k in t.parameters.get("properties", {}) if k not in required]
        sig = ", ".join(list(required) + [f"{o}=?" for o in optional])
        print(f"  {t.name}({sig})  [{len(t.description)} 字]")
