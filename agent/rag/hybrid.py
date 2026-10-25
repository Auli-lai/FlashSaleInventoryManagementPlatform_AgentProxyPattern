"""
混合检索 —— 向量召回 + BM25 召回 → RRF 融合（MVP 不做 rerank）。

RRF（Reciprocal Rank Fusion）只用排名不用分数，绕开两路分数量纲不可比的问题：
    RRF(d) = Σ_r  1 / (k + rank_r(d))
两个文档被两路同时召回时分数叠加，正是我们要的"两路都认为相关"。
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Sequence

from bm25 import BM25Index
from embedder import Embedder
from rerank import NoOpReranker
from vector_store import VectorStore


@dataclass
class Hit:
    chunk: Any
    score: float
    source: str
    debug: dict[str, Any] = field(default_factory=dict)

    @property
    def text(self) -> str:
        return self.chunk.text


def rrf_fuse(rankings: Sequence[Sequence[str]], k: int = 60) -> dict[str, float]:
    scores: dict[str, float] = {}
    for ranking in rankings:
        for rank, cid in enumerate(ranking, start=1):
            scores[cid] = scores.get(cid, 0.0) + 1.0 / (k + rank)
    return scores


class HybridRetriever:
    def __init__(self, store: VectorStore, embedder: Embedder, dim: int) -> None:
        self.store = store
        self.embedder = embedder
        self.dim = dim
        self._by_id: dict[str, Any] = {c.chunk_id: c for c in store.chunks}
        self.sparse = BM25Index(
            doc_ids=[c.chunk_id for c in store.chunks],
            doc_texts=[c.text for c in store.chunks],
        )

    def dense_only(self, query: str, top_k: int = 20):
        vec = self.embedder.embed_query(query)
        # VectorStore.search 返回 (整数行号, 分数)，行号即 store.chunks 的下标，
        # 不是 chunk_id 字符串（BM25 那边才是字符串 id）。
        return [Hit(self.store.chunks[cid], s, "dense", {"dense_rank": i})
                for i, (cid, s) in enumerate(self.store.search(vec, top_k), start=1)]

    def sparse_only(self, query: str, top_k: int = 20):
        return [Hit(self._by_id[cid], s, "sparse", {"sparse_rank": i})
                for i, (cid, s) in enumerate(self.sparse.search(query, top_k), start=1)
                if cid in self._by_id]

    def hybrid(self, query: str, top_k: int = 20, k: int = 60):
        dense = self.dense_only(query, top_k)
        sparse = self.sparse_only(query, top_k)
        dense_ids = [h.chunk.chunk_id for h in dense]
        sparse_ids = [h.chunk.chunk_id for h in sparse]
        fused = rrf_fuse([dense_ids, sparse_ids], k=k)
        ranked = sorted(fused.items(), key=lambda kv: kv[1], reverse=True)[:top_k]
        return [Hit(self._by_id[cid], score, "hybrid", {
            "dense_rank": dense_ids.index(cid) + 1 if cid in dense_ids else None,
            "sparse_rank": sparse_ids.index(cid) + 1 if cid in sparse_ids else None,
        }) for cid, score in ranked if cid in self._by_id]

    def retrieve(self, query: str, top_k: int = 5):
        fused = self.hybrid(query, top_k=top_k * 4, k=60)
        reranker = NoOpReranker()
        reranked = reranker.rerank(query, [h.chunk for h in fused], top_k)
        pre_rank = {h.chunk.chunk_id: i for i, h in enumerate(fused, start=1)}
        return [Hit(item.chunk, item.score, "rerank", {**h.debug, "pre_rerank_rank": pre_rank.get(item.chunk.chunk_id)})
                if (h := next((x for x in fused if x.chunk.chunk_id == item.chunk.chunk_id), None))
                else Hit(item.chunk, item.score, "rerank", {})
                for item in reranked]


def format_hits(hits: Sequence[Hit], max_chars: int = 800) -> str:
    if not hits:
        return "（没有检索到相关资料）"
    blocks = []
    for i, hit in enumerate(hits, start=1):
        text = hit.text
        if len(text) > max_chars:
            text = text[:max_chars] + "…（已截断）"
        meta = hit.chunk.metadata
        origin = meta.get("topic") or meta.get("category") or meta.get("source")
        blocks.append(f"【资料 {i}】{hit.chunk.chunk_id}（{origin}）\n{text}")
    return "\n\n".join(blocks)
