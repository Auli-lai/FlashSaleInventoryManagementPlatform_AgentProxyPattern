"""
库存运维 Agent —— 命令行入口。

===========================================================================
用法速查
===========================================================================

  # 1) 离线演示（不需要 Key / 引擎 / 网络，先看循环长什么样）
  python main.py --dry-run --question "库存还有多少"

  # 2) 真实诊断（需要 DEEPSEEK_API_KEY，引擎在跑且 ENGINE_HOST/PORT 可达）
  python main.py --question "stock:1 会不会超卖"

  # 3) 交互问答（多轮，支持追问）
  python main.py                 # 真实模式交互
  python main.py --dry-run       # 离线模式交互

  # 4) 其它
  python main.py --list-tools    # 列出 6 个工具
  python main.py --build-index   # 构建 RAG 索引（默认离线；真实用 RAG_OFFLINE=0）
  python main.py --question "..." --json   # 输出结构化 JSON
  python main.py --question "..." --trace  # 附带每一步工具调用轨迹

===========================================================================
模式说明
===========================================================================
  --dry-run  用 ScriptedChatModel（假模型）+ DemoToolRunner（假数据），
             把"思考→调工具→回灌→出结论"整条链路跑出来，零成本验证代码没断。
              注意：假模型是固定剧本，不认你的问题，只为演示循环形状。
  真实模式   用 DeepSeek(deepseek-flash) + 真实 RESP 客户端 + 真实 RAG 检索。
"""

from __future__ import annotations

import argparse
import json
import os
import sys

# 保证 agent/ 在 sys.path（无论从哪里调用本脚本）
_AGENT_DIR = os.path.dirname(os.path.abspath(__file__))
if _AGENT_DIR not in sys.path:
    sys.path.insert(0, _AGENT_DIR)

from agent import InventoryAgent, Session  # noqa: E402
from tools import TOOLS, ToolRunner  # noqa: E402
from offline_demo import DemoToolRunner, default_demo_script  # noqa: E402
from llm_client import ScriptedChatModel  # noqa: E402


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------

def _print_tool_list() -> None:
    print("已注册工具（共 %d 个）：\n" % len(TOOLS))
    for t in TOOLS:
        required = t.parameters.get("required", [])
        props = t.parameters.get("properties", {})
        optional = [k for k in props if k not in required]
        sig = ", ".join(list(required) + [f"{o}=?" for o in optional])
        print(f"  · {t.name}({sig})")
        # 把 description 的前两行作为一句话简介
        first_line = t.description.strip().splitlines()[0] if t.description.strip() else ""
        print(f"      {first_line[:88]}")


def _run_once(agent: InventoryAgent, question: str, session: Session | None,
              show_trace: bool, as_json: bool) -> None:
    diag = agent.run(question, session=session)
    if as_json:
        print(json.dumps(diag.to_dict(), ensure_ascii=False, indent=2))
        return

    if show_trace and diag.steps:
        print("\n" + diag.trace())
        print("=" * 60)
    print("\n── 结论 ──")
    print(diag.answer)
    meta = (
        f"\n[诊断信息] 轮数={diag.rounds} 工具调用={diag.tool_calls} "
        f"停止原因={diag.stopped_because} "
        f"输入tokens={diag.input_tokens} 输出tokens={diag.output_tokens}"
    )
    if diag.forced_conclusion:
        meta += "（已强制收尾）"
    if diag.error:
        meta += f" 错误={diag.error}"
    print(meta)


def _interactive(agent: InventoryAgent) -> None:
    session = Session()
    print("库存运维 Agent 交互模式（输入 exit / quit 退出，空白跳过）\n")
    while True:
        try:
            q = input("你 > ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\n再见。")
            break
        if not q:
            continue
        if q.lower() in ("exit", "quit", "q"):
            print("再见。")
            break
        try:
            diag = agent.run(q, session=session)
        except Exception as exc:  # noqa: BLE001
            print(f"[运行出错] {type(exc).__name__}: {exc}")
            continue
        print("\n── 结论 ──")
        print(diag.answer)
        print(f"[轮数={diag.rounds} 工具调用={diag.tool_calls} "
              f"停止={diag.stopped_because}]\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="库存引擎运维 Agent（DeepSeek + 工具调用 + RAG）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--question", "-q", help="要问的问题（不传则进入交互模式）")
    ap.add_argument("--dry-run", action="store_true",
                    help="用假模型 + 假数据离线跑，不需要 Key/引擎/网络")
    ap.add_argument("--json", action="store_true", help="输出结构化 JSON")
    ap.add_argument("--trace", action="store_true", help="打印每一步工具调用轨迹")
    ap.add_argument("--list-tools", action="store_true", help="列出所有工具后退出")
    ap.add_argument("--build-index", action="store_true",
                    help="构建 RAG 索引后退出（默认离线；真实索引用 RAG_OFFLINE=0）")
    ap.add_argument("--web", action="store_true",
                    help="启动 Web 界面（FastAPI + SSE，默认 http://127.0.0.1:8000）")
    args = ap.parse_args(argv)

    # 列出工具
    if args.list_tools:
        _print_tool_list()
        return 0

    # 构建索引
    if args.build_index:
        from rag import build_index
        offline = os.environ.get("RAG_OFFLINE", "1").lower() in ("1", "true", "yes")
        build_index.main(offline=offline)
        return 0

    # 启动 Web
    if args.web:
        from web import run_server
        run_server()
        return 0

    # 选择模型与工具执行器
    if args.dry_run:
        agent = InventoryAgent(
            model=ScriptedChatModel(default_demo_script()),
            runner=DemoToolRunner(),
        )
    else:
        agent = InventoryAgent()  # 默认 DeepSeek + 真实 ToolRunner（懒连接）

    session = Session()
    if args.question:
        _run_once(agent, args.question, session, args.trace, args.json)
        return 0

    # 交互模式
    _interactive(agent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
