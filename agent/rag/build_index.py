"""
构建检索索引 —— 读语料 → 切 chunk → 向量化 → 落盘 FAISS/numpy + BM25 可重建。

用法：
    python build_index.py            # 用 config 里的 embedding 端点
    python build_index.py --offline  # 用哈希伪向量（不花额度，仅验证链路）
    python build_index.py --sample   # 同 offline
"""

from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))                      # rag/
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))      # agent/

from chunking import Chunk, chunk_knowledge
from config import INDEX_DIR, OFFLINE_EMBEDDING, EMBEDDING_MODEL, EMBEDDING_BASE_URL
from corpus import build_knowledge_base
from embedder import build_embedder
from vector_store import VectorStore


def build_chunks() -> list[Chunk]:
    kb = build_knowledge_base()
    return chunk_knowledge(kb)


def main(offline: bool) -> None:
    chunks = build_chunks()
    print(f"语料切出 {len(chunks)} 个 chunk")

    emb = build_embedder(offline=offline)
    print(f"向量化中（dim={emb.dim}）…")
    import numpy as np
    matrix = emb.embed_documents([c.text for c in chunks])

    os.makedirs(INDEX_DIR, exist_ok=True)
    VectorStore(chunks, emb.dim).set_vectors(matrix)
    np.save(os.path.join(INDEX_DIR, "dense.npy"), matrix)

    with open(os.path.join(INDEX_DIR, "chunks.json"), "w", encoding="utf-8") as f:
        json.dump([{"chunk_id": c.chunk_id, "text": c.text, "metadata": c.metadata}
                   for c in chunks], f, ensure_ascii=False, indent=2)

    # 元信息：让 build_retriever 能在查询时重建正确维度 / 离线标志的 embedder，
    # 否则离线建索引、在线（或反过来）检索会因向量维度不一致而报错。
    meta = {
        "dim": int(matrix.shape[1]),
        "offline": offline,
        "embedding_model": EMBEDDING_MODEL,
        "embedding_base_url": EMBEDDING_BASE_URL,
        "count": len(chunks),
    }
    with open(os.path.join(INDEX_DIR, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2)

    print(f"索引已落盘：{INDEX_DIR}（{len(chunks)} 条，dim={meta['dim']}，offline={offline}）")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", action="store_true", help="用哈希伪向量")
    ap.add_argument("--sample", action="store_true", help="同 --offline")
    args = ap.parse_args()
    offline = args.offline or args.sample or OFFLINE_EMBEDDING
    main(offline)
