"""
RAG 包入口 —— 对外只暴露 build_retriever()，屏蔽内部 BM25 / FAISS /
Embedder / HybridRetriever 的装配细节。

tools.py 里 `from rag import build_retriever` 就是走这里。它负责：
    1. 找到 INDEX_DIR 下 build_index.py 落盘的 dense.npy + chunks.json + meta.json
    2. 重建 VectorStore（向量索引）和对应维度的 Embedder
    3. 用二者装配一个 HybridRetriever 返回（对外只有 .retrieve(query, top_k)）

【维度一致性】关键陷阱：离线（哈希伪向量，dim=256）建的索引，查询时也必须
用离线 embedder，否则向量维度对不上会直接炸。meta.json 里记录了建索引时的
offline 标志与 dim，这里优先按它决定用哪个 embedder；没 meta 时才回退到
config 的 RAG_OFFLINE。
"""

from __future__ import annotations

import json
import os
import sys
from typing import Any

# 路径自举：rag 包内部都是"平级裸导入"（from chunking import ... / from config
# import ...）。无论本文件是被 `from rag import build_retriever`（包上下文）还是被
# build_index.py 当脚本间接加载，都先确保 rag/ 与 agent/ 在 sys.path 上，
# 否则裸导入会找不到兄弟模块。
_HERE = os.path.dirname(os.path.abspath(__file__))
_PARENT = os.path.dirname(_HERE)
for _p in (_HERE, _PARENT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from config import INDEX_DIR, OFFLINE_EMBEDDING
from chunking import Chunk
from vector_store import VectorStore
from embedder import build_embedder
from hybrid import HybridRetriever


def _load_meta() -> dict[str, Any] | None:
    path = os.path.join(INDEX_DIR, "meta.json")
    if not os.path.isfile(path):
        return None
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def build_retriever(offline: bool | None = None) -> HybridRetriever:
    """重建检索器。offline 若不传，按 meta.json（优先）或 config 推断。

    抛 RuntimeError 当索引文件不存在（提示先跑 build_index.py）。
    """
    dense_path = os.path.join(INDEX_DIR, "dense.npy")
    chunks_path = os.path.join(INDEX_DIR, "chunks.json")
    if not (os.path.isfile(dense_path) and os.path.isfile(chunks_path)):
        raise RuntimeError(
            "检索索引还没建。请先在 agent/ 目录下运行：\n"
            "    python rag/build_index.py --offline   # 不想配 embedding，先验证链路\n"
            "    python rag/build_index.py             # 用 config 里的 embedding 端点建真实索引\n"
            f"（索引目录 = {INDEX_DIR}）"
        )

    import numpy as np

    meta = _load_meta() or {}
    matrix = np.load(dense_path)
    dim = int(meta.get("dim", matrix.shape[1]))

    # 维度决策：显式传 > meta.json 记录 > config 标志
    if offline is None:
        offline = bool(meta.get("offline", OFFLINE_EMBEDDING))

    with open(chunks_path, encoding="utf-8") as f:
        raw = json.load(f)
    chunks = [Chunk(c["chunk_id"], c["text"], c.get("metadata", {})) for c in raw]

    store = VectorStore.load(dense_path, chunks, dim)
    embedder = build_embedder(offline=offline)
    return HybridRetriever(store, embedder, embedder.dim)


__all__ = ["build_retriever", "HybridRetriever", "Chunk"]
