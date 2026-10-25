"""
向量索引 —— 优先用 FAISS（快），没有就退回 numpy 暴力搜索（小语料够用）。

设计：所有向量都是 L2 归一化后的，所以内积 == 余弦相似度，用 IndexFlatIP。
"""

from __future__ import annotations

from typing import Sequence

import numpy as np


class VectorStore:
    """一批 chunk 的向量索引。支持 save/load 到磁盘（numpy 格式）。"""

    def __init__(self, chunks: list, dim: int) -> None:
        self.chunks = chunks
        self.dim = dim
        self._matrix: np.ndarray | None = None
        self._index = None  # faiss index，可选
        self._try_faiss()

    def _try_faiss(self) -> None:
        try:
            import faiss  # type: ignore
            self._faiss = faiss
        except ImportError:  # pragma: no cover
            self._faiss = None

    def set_vectors(self, matrix: np.ndarray) -> None:
        matrix = np.asarray(matrix, dtype="float32")
        if matrix.shape[0] == 0:
            self._matrix = matrix
            return
        self._matrix = matrix
        if self._faiss is not None and matrix.shape[0] > 0:
            idx = self._faiss.IndexFlatIP(self.dim)
            idx.add(matrix)
            self._index = idx

    def search(self, query_vec: np.ndarray, top_k: int = 20) -> list[tuple[int, float]]:
        if self._matrix is None or self._matrix.shape[0] == 0:
            return []
        q = np.asarray(query_vec, dtype="float32").reshape(1, -1)
        if self._index is not None:
            k = min(top_k, self._matrix.shape[0])
            scores, ids = self._index.search(q, k)
            return [(int(i), float(s)) for i, s in zip(ids[0], scores[0]) if i != -1]
        # numpy 暴力：内积
        sims = self._matrix @ q.reshape(-1)
        order = np.argsort(-sims)[:top_k]
        return [(int(i), float(sims[i])) for i in order]

    def save(self, path: str) -> None:
        np.save(path, self._matrix)

    @classmethod
    def load(cls, path: str, chunks: list, dim: int) -> "VectorStore":
        store = cls(chunks, dim)
        matrix = np.load(path)
        store.set_vectors(matrix)
        return store
