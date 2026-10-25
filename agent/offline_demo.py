"""
离线演示替身 —— 让 `python main.py --dry-run` 在没有引擎、没有 API Key、
没有网络的情况下，把"模型思考 → 调工具 → 回灌 → 出结论"整条链路跑出来。

包含两个东西：
  1. DemoToolRunner：6 个工具的**假实现**（返回固定的演示数据）
  2. default_demo_script：一个假模型（ScriptedChatModel）的编剧脚本

真实 Agent 不会用到本文件；它只服务于"先看清循环长什么样"的学习目的，
以及在 CI / 没网环境里验证链路没断。
"""

from __future__ import annotations

from llm_client import ScriptedChatModel, ToolCall, Turn
from tools import ToolRunner


class DemoToolRunner(ToolRunner):
    """用假数据代替真实引擎与检索层。"""

    def __init__(self) -> None:
        super().__init__(client=None)

    def _get_key(self, key: str) -> dict:
        return {"key": key, "exists": True, "value": "50", "ttl_seconds": -1,
                "value_is_integer": True}

    def _hot_keys(self, top_n: int = 10) -> dict:
        return {"top_n": top_n, "hot_keys": [
            {"key": "stock:1", "hits": 1820},
            {"key": "stock:2", "hits": 1340},
            {"key": "seckill:token:abc", "hits": 980},
        ], "note": "（演示数据）来自引擎 STATS 的实时访问计数。"}

    def _qps_report(self, **_kwargs: object) -> dict:
        return {"uptime_sec": 3600, "total_commands": 142000, "connections": 3,
                "qps_estimate": 39.4, "qps_window_secs": 6,
                "cmd_counts": {"GET": 60000, "INCR": 40000, "DECR": 40000, "SET": 2000},
                "note": "（演示数据）"}

    def _stock_query(self, key: str) -> dict:
        return {"key": key, "exists": True, "remaining_stock": 50, "is_integer": True,
                "sold_out": False}

    def _simulate_decr(self, key: str) -> dict:
        return {"key": key, "mode": "SIMULATION（仅演示，已还原，未真正减少库存）",
                "before": "50", "after_decr": 49, "after_restore": 50, "final": "50",
                "note": "（演示数据）标准秒杀扣减流程演示。"}

    def _knowledge_qa(self, query: str, top_k: int = 5) -> dict:
        return {"hits": [
            {"chunk_id": "manual_DECR", "text":
             "DECR key：将 key 存储的整数值减 1。若 key 不存在则先初始化为 0 再减，"
             "返回减后的值。若返回负数说明『扣减超过实际库存』，应做余额校验回滚。",
             "source": "命令手册"},
        ], "note": "（演示数据）来自本地知识库。"}


def default_demo_script() -> list[Turn]:
    """假模型的剧本：先查库存，再给结论。"""
    return [
        Turn(
            text="我先查一下这个商品的库存余量。",
            tool_calls=[ToolCall(id="c1", name="stock_query", arguments={"key": "stock:1"})],
        ),
        Turn(
            text=(
                "## 结论\n商品 stock:1 当前剩余库存 50 件，未售罄，可正常售卖。\n\n"
                "## 依据\nstock_query 工具返回 remaining_stock=50，sold_out=false。"
                "该值是一个整数计数，表示还有 50 件可用。\n\n"
                "## 建议\n若这是秒杀商品，建议配合 hot_keys 观察是否有热点集中；"
                "若担心超卖，可调用 simulate_decr 演示标准扣减-还原流程（不会真减库存）。"
            ),
            stop_reason="stop",
        ),
    ]


if __name__ == "__main__":
    runner = DemoToolRunner()
    for t in runner.tools:
        print(f"  {t.name}")
    print("DemoToolRunner 就绪（返回固定演示数据）。")
