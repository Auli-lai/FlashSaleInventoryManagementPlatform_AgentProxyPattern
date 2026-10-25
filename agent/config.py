"""
Agent 配置 —— 所有外部依赖的坐标集中在这里（不散落到各个文件）。

===========================================================================
关键变更（相对 net-pluse）：LLM 从阿里云百炼换成 DeepSeek
===========================================================================

本项目是库存引擎（自研 Redis 兼容）的运维 Agent。按计划文档第九节的复用
映射，原本要复用 net-pluse 的 llm_client（百炼 API）。但用户决定不再使用
百炼，改用 DeepSeek。因此：

    · LLM 走 DeepSeek 的 OpenAI 兼容端点（https://api.deepseek.com）
    · 模型用 deepseek-flash（便宜、快，支持 Function Calling）
    · Key 从环境变量 DEEPSEEK_API_KEY 读，绝不硬编码

【DeepSeek 没有 embedding 接口】这一点要特别说明：DeepSeek 只提供
chat/completion，不提供向量化。所以 RAG 的向量化不能再用 DeepSeek，而是
走一个**可配置的 OpenAI 兼容 embedding 端点**（见下方 EMBEDDING_*）。
你可以指向：
    · 本地 Ollama：      http://localhost:11434/v1  （推荐，零费用、可离线）
    · OpenAI：           https://api.openai.com/v1
    · 或其它兼容服务
如果不想配 embedding，置 RAG_OFFLINE=1 即可用哈希伪向量把链路跑通
（仅验证代码路径，不能用于评测检索质量）。
"""

from __future__ import annotations

import os

_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# ---------------------------------------------------------------------------
# .env 支持（极简加载器，不引第三方依赖）
# ---------------------------------------------------------------------------

def _load_dotenv(path: str) -> None:
    """KEY=VALUE 一行一条；值两边引号去掉；不覆盖已存在的环境变量。"""
    if not os.path.isfile(path):
        return
    try:
        with open(path, encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                key = key.strip()
                value = value.strip().strip('"').strip("'")
                if key and key not in os.environ:
                    os.environ[key] = value
    except OSError:
        pass


for _candidate in (
    os.path.join(_PROJECT_ROOT, ".env"),          # 项目根目录（FlashSale 顶层）
    os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"),  # agent/
    os.path.join(os.getcwd(), ".env"),           # 当前工作目录
):
    _load_dotenv(_candidate)


# ---------------------------------------------------------------------------
# 1. LLM：DeepSeek（OpenAI 兼容协议）
# ---------------------------------------------------------------------------

DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY")

def require_deepseek_key() -> str:
    if not DEEPSEEK_API_KEY:
        raise RuntimeError(
            "没有找到环境变量 DEEPSEEK_API_KEY。\n"
            "运行前先导出（Linux/WSL）：\n"
            "    export DEEPSEEK_API_KEY='sk-你的deepseek key'\n"
            "Windows PowerShell：\n"
            "    $env:DEEPSEEK_API_KEY='sk-你的deepseek key'\n"
            "DeepSeek 官网 -> API Keys 里创建。\n"
            "（只想验证链路能不能跑通、不想配 key，加 --dry-run 参数）"
        )
    return DEEPSEEK_API_KEY


DEEPSEEK_BASE_URL = os.environ.get("DEEPSEEK_BASE_URL", "https://api.deepseek.com")
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-flash")
MAX_TOKENS = int(os.environ.get("DEEPSEEK_MAX_TOKENS", "2048"))


# ---------------------------------------------------------------------------
# 2. 库存引擎（RESP 客户端连接目标）
# ---------------------------------------------------------------------------

ENGINE_HOST = os.environ.get("ENGINE_HOST", "127.0.0.1")
ENGINE_PORT = int(os.environ.get("ENGINE_PORT", "6379"))
ENGINE_TIMEOUT = float(os.environ.get("ENGINE_TIMEOUT", "2.0"))


# ---------------------------------------------------------------------------
# 3. 向量化：OpenAI 兼容端点（DeepSeek 不提供，需另行配置）
# ---------------------------------------------------------------------------

EMBEDDING_BASE_URL = os.environ.get(
    "EMBEDDING_BASE_URL", "http://localhost:11434/v1"   # 默认指向本地 Ollama
)
EMBEDDING_API_KEY = os.environ.get("EMBEDDING_API_KEY", "ollama")  # Ollama 不需要真 key
EMBEDDING_MODEL = os.environ.get("EMBEDDING_MODEL", "nomic-embed-text")
EMBEDDING_DIM = int(os.environ.get("EMBEDDING_DIM", "768"))

# OpenAI 兼容 embedding 单次请求最多 100 条文本，超了要分批。
EMBEDDING_BATCH = int(os.environ.get("EMBEDDING_BATCH", "32"))

# 离线冒烟模式：不调 embedding API，用确定性哈希假装向量。
# 仅用于验证"索引→召回→融合"链路通不通，没有语义能力，不可用于评测。
OFFLINE_EMBEDDING = os.environ.get("RAG_OFFLINE", "").lower() in ("1", "true", "yes")


# ---------------------------------------------------------------------------
# 4. 检索参数
# ---------------------------------------------------------------------------

DENSE_TOP_K = int(os.environ.get("RAG_DENSE_TOP_K", "20"))
SPARSE_TOP_K = int(os.environ.get("RAG_SPARSE_TOP_K", "20"))
RRF_K = int(os.environ.get("RAG_RRF_K", "60"))
FINAL_TOP_K = int(os.environ.get("RAG_FINAL_TOP_K", "5"))

INDEX_DIR = os.environ.get(
    "RAG_INDEX_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "rag", "index_store"),
)


if __name__ == "__main__":
    print("── 当前配置 ──")
    print(f"  DEEPSEEK_MODEL     {DEEPSEEK_MODEL}")
    print(f"  DEEPSEEK_BASE_URL  {DEEPSEEK_BASE_URL}")
    print(f"  ENGINE_HOST:PORT   {ENGINE_HOST}:{ENGINE_PORT}")
    print(f"  EMBEDDING_MODEL    {EMBEDDING_MODEL}")
    print(f"  EMBEDDING_BASE_URL {EMBEDDING_BASE_URL}")
    print(f"  OFFLINE_EMBEDDING  {OFFLINE_EMBEDDING}")
    if DEEPSEEK_API_KEY:
        print(f"  DEEPSEEK_API_KEY   …{DEEPSEEK_API_KEY[-4:]}  （已读到，长度 {len(DEEPSEEK_API_KEY)}）")
    else:
        print("  DEEPSEEK_API_KEY   (未读到 —— 真实模式需要，--dry-run 不需要)")
