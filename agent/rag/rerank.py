"""重排器。MVP 阶段不引入 rerank（语料 < 1000 条，Top10 人工可评估）。
提供 NoOpReranker 占位，并保持接口一致，方便日后接 rerank 模型。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Sequence


@dataclass
class RankedChunk:
    chunk: object
    score: float


class Reranker:
    def rerank(self, query: str, chunks: Sequence[object], top_k: int = 5) -> list[RankedChunk]:
        raise NotImplementedError


class NoOpReranker(Reranker):
    def rerank(self, query: str, chunks: Sequence[object], top_k: int = 5) -> list[RankedChunk]:
        items = [RankedChunk(c, getattr(c, "metadata", {}).get("_score", 0.0)) for c in chunks]
        return items[:top_k]
