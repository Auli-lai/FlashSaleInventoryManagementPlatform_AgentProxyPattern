"""
评测集 —— 20 条谓词（10 知识类 + 10 工具类）。

设计目标：在**没有 DeepSeek Key、没有引擎**的情况下也能跑出确定性结果，
验证"检索层能召回正确资料"和"工具层能产出预期字段"这两条链路没断。
真正的端到端（DeepSeek 选工具 + 真实引擎）评测请见下方 run_eval.py 的 --live 模式。

每条谓词是一个 dict：
  id          唯一编号
  category    "knowledge" | "tool"
  question    用户问法（贴近真实口语）
  --- 知识类 ---
  expected_topics   期望命中的知识库 topic（命中其一即算对）
  --- 工具类 ---
  expected_tool     期望调用的工具名
  args             传给该工具的参数（演示数据下有效）
  expect_keys      返回 dict 必须包含的字段（缺了就判 fail）
"""

from __future__ import annotations

PREDICATES: list[dict] = [
    # ───────────────────────── 知识类（10）─────────────────────────
    {
        "id": "K01", "category": "knowledge",
        "question": "DECR 返回负数是什么意思",
        "expected_topics": ["DECR", "decr_negative"],
    },
    {
        "id": "K02", "category": "knowledge",
        "question": "TTL 返回 -1 表示什么",
        "expected_topics": ["TTL", "ttl_minus_one"],
    },
    {
        "id": "K03", "category": "knowledge",
        "question": "TTL 返回 -2 说明什么",
        "expected_topics": ["TTL", "ttl_minus_two"],
    },
    {
        "id": "K04", "category": "knowledge",
        "question": "QPS 突然下降可能是什么原因",
        "expected_topics": ["qps_sudden_drop"],
    },
    {
        "id": "K05", "category": "knowledge",
        "question": "INCRBY 数值溢出会发生什么",
        "expected_topics": ["INCRBY", "incr_overflow"],
    },
    {
        "id": "K06", "category": "knowledge",
        "question": "怎么防止秒杀超卖",
        "expected_topics": ["oversell", "DECR"],
    },
    {
        "id": "K07", "category": "knowledge",
        "question": "热点 key 太集中怎么办",
        "expected_topics": ["hot_key_concentration"],
    },
    {
        "id": "K08", "category": "knowledge",
        "question": "AOF 积压怎么处理",
        "expected_topics": ["aof_backlog"],
    },
    {
        "id": "K09", "category": "knowledge",
        "question": "为什么 KEYS 命令会让引擎变慢",
        "expected_topics": ["KEYS", "keys_blocking"],
    },
    {
        "id": "K10", "category": "knowledge",
        "question": "EXPIRE 怎么给 key 设置过期",
        "expected_topics": ["EXPIRE"],
    },

    # ───────────────────────── 工具类（10）─────────────────────────
    {
        "id": "T01", "category": "tool",
        "question": "查一下 stock:1 这个 key 的详情",
        "expected_tool": "get_key", "args": {"key": "stock:1"},
        "expect_keys": ["key", "exists", "value", "ttl_seconds"],
    },
    {
        "id": "T02", "category": "tool",
        "question": "哪些 key 访问次数最多",
        "expected_tool": "hot_keys", "args": {},
        "expect_keys": ["top_n", "hot_keys"],
    },
    {
        "id": "T03", "category": "tool",
        "question": "当前引擎 QPS 多少",
        "expected_tool": "qps_report", "args": {},
        "expect_keys": ["qps_estimate", "connections", "cmd_counts"],
    },
    {
        "id": "T04", "category": "tool",
        "question": "stock:1 还剩多少库存，会不会超卖",
        "expected_tool": "stock_query", "args": {"key": "stock:1"},
        "expect_keys": ["remaining_stock", "sold_out", "is_integer"],
    },
    {
        "id": "T05", "category": "tool",
        "question": "帮我演示一下扣减库存的逻辑",
        "expected_tool": "simulate_decr", "args": {"key": "stock:1"},
        "expect_keys": ["mode", "before", "after_decr", "after_restore", "final"],
    },
    {
        "id": "T06", "category": "tool",
        "question": "DECR 返回负数是什么意思",
        "expected_tool": "knowledge_qa", "args": {"query": "DECR 返回负数", "top_k": 5},
        "expect_keys": ["hits"],
    },
    {
        "id": "T07", "category": "tool",
        "question": "查一下那个库存 key 的 TTL",
        "expected_tool": "get_key", "args": {"key": "库存"},
        "expect_keys": ["key", "ttl_seconds"],
    },
    {
        "id": "T08", "category": "tool",
        "question": "给我 Top5 的热点 key",
        "expected_tool": "hot_keys", "args": {"top_n": 5},
        "expect_keys": ["top_n", "hot_keys"],
    },
    {
        "id": "T09", "category": "tool",
        "question": "库存变成负数了怎么回事",
        "expected_tool": "knowledge_qa", "args": {"query": "库存变成负数", "top_k": 5},
        "expect_keys": ["hits"],
    },
    {
        "id": "T10", "category": "tool",
        "question": "商品 1 的库存余量",
        "expected_tool": "stock_query", "args": {"key": "商品"},
        "expect_keys": ["remaining_stock"],
    },
]
