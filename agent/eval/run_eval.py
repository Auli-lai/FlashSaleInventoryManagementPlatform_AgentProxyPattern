"""
评测运行器 —— 跑通 20 条谓词，输出 pass/fail 与汇总。

===========================================================================
用法
===========================================================================
  # 离线（默认）：知识类用真实 RAG 检索器（哈希伪向量），工具类用演示执行器
  python eval/run_eval.py

  # 真实端到端：需要 DEEPSEEK_API_KEY + 引擎在跑，且已建真实索引（RAG_OFFLINE=0）
  python eval/run_eval.py --live

===========================================================================
判定逻辑
===========================================================================
  知识类：retrieve(query) 的 top5 里，至少有一个 chunk 的 topic 在
          expected_topics 中 → pass。
  工具类：用 DemoToolRunner 调用 expected_tool，返回 dict 必须包含 expect_keys
          且不能有 "error" 字段 → pass。
===========================================================================
注意：离线模式下 RAG 用的是哈希伪向量（只有字面重叠能力），知识类判定的
"命中"依赖查询与正文共享技术词（DECR / TTL / -1 等）。建真实索引后召回更准。
"""

from __future__ import annotations

import argparse
import os
import sys

_AGENT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _AGENT_DIR not in sys.path:
    sys.path.insert(0, _AGENT_DIR)

from eval.predicates import PREDICATES  # noqa: E402


def _eval_knowledge(case: dict, retriever) -> tuple[bool, str]:
    hits = retriever.retrieve(case["question"], top_k=5)
    got_topics = [h.chunk.metadata.get("topic") or h.chunk.metadata.get("source") for h in hits]
    matched = [t for t in case["expected_topics"] if t in got_topics]
    ok = bool(matched)
    detail = (f"top5 topics={got_topics}; 命中={matched}"
              if ok else f"top5 topics={got_topics}; 期望任一在 {case['expected_topics']}")
    return ok, detail


def _eval_tool(case: dict) -> tuple[bool, str]:
    from offline_demo import DemoToolRunner
    runner = DemoToolRunner()
    result = runner.run(case["expected_tool"], case.get("args", {}))
    if "error" in result:
        return False, f"工具返回 error：{result['error']}"
    missing = [k for k in case["expect_keys"] if k not in result]
    if missing:
        return False, f"缺少字段 {missing}（实际 keys={sorted(result)}）"
    return True, f"字段齐全：{case['expect_keys']}"


def main() -> int:
    ap = argparse.ArgumentParser(description="库存运维 Agent 评测")
    ap.add_argument("--live", action="store_true", help="真实端到端（需 Key+引擎+真实索引）")
    args = ap.parse_args()

    retriever = None
    if not args.live:
        from rag import build_retriever
        try:
            retriever = build_retriever()
        except RuntimeError as exc:
            print(f"[跳过知识类] {exc}")
            retriever = None

    results: list[tuple[dict, bool, str]] = []
    for case in PREDICATES:
        if case["category"] == "knowledge":
            if retriever is None:
                results.append((case, False, "RAG 索引不可用（先跑 python main.py --build-index）"))
                continue
            ok, detail = _eval_knowledge(case, retriever)
        else:
            ok, detail = _eval_tool(case)
        results.append((case, ok, detail))

    # 输出报告
    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print(f"\n{'ID':<5}{'类别':<10}{'问题':<34}{'结果':<6}说明")
    print("-" * 100)
    for case, ok, detail in results:
        mark = "PASS" if ok else "FAIL"
        q = case["question"][:32]
        print(f"{case['id']:<5}{case['category']:<10}{q:<34}{mark:<6}{detail[:40]}")

    print("-" * 100)
    print(f"通过 {passed}/{total}  （知识类需在离线索引之上召回，离线哈希仅字面匹配）")
    if not args.live:
        print("提示：python eval/run_eval.py --live 可在真实环境下做端到端评测。")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())
