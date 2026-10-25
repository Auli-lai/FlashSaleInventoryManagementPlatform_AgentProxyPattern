"""
向量化 —— OpenAI 兼容端点（默认指向本地 Ollama，零费用、可离线）。

===========================================================================
为什么不用 DeepSeek 做 embedding？
===========================================================================

DeepSeek 只提供 chat/completion，**没有 embedding 接口**。所以本项目的
向量化走一个**可配置的 OpenAI 兼容端点**（见 config.EMBEDDING_*）：

    · 本地 Ollama：      EMBEDDING_BASE_URL=http://localhost:11434/v1
                         EMBEDDING_MODEL=nomic-embed-text
    · OpenAI：           EMBEDDING_BASE_URL=https://api.openai.com/v1
                         EMBEDDING_MODEL=text-embedding-3-small
    · 其它兼容服务：     按对应地址 / 模型名填即可

默认配置指向 Ollama，因为零费用且可离线，最适合本地演示。
不配 embedding 时置 RAG_OFFLINE=1 用哈希伪向量把链路跑通（不可用于评测）。
"""

from __future__ import annotations

import hashlib
import os
from abc import ABC, abstractmethod
from typing import Sequence

import numpy as np

from config import (
    EMBEDDING_BASE_URL,
    EMBEDDING_BATCH,
    EMBEDDING_DIM,
    EMBEDDING_MODEL,
    OFFLINE_EMBEDDING,
    require_deepseek_key,
)


class Embedder(ABC):
    dim: int

    @abstractmethod
    def embed_documents(self, texts: Sequence[str]) -> np.ndarray: ...

    def embed_query(self, text: str) -> np.ndarray:
        return self.embed_documents([text])[0]


def _l2_normalize(matrix: np.ndarray) -> np.ndarray:
    norms = np.linalg.norm(matrix, axis=1, keepdims=True)
    norms[norms == 0] = 1.0
    return matrix / norms


class OpenAICompatibleEmbedder(Embedder):
    """通过任意 OpenAI 兼容端点调用向量化模型（Ollama / OpenAI / 其它）。"""

    def __init__(self, model: str = EMBEDDING_MODEL, dim: int = EMBEDDING_DIM,
                 base_url: str = EMBEDDING_BASE_URL, api_key: str | None = None,
                 batch_size: int = EMBEDDING_BATCH) -> None:
        from openai import OpenAI
        self.model = model
        self.dim = dim
        self.batch_size = batch_size
        key = api_key or os.environ.get("EMBEDDING_API_KEY") or "ollama"
        self._client = OpenAI(api_key=key, base_url=base_url)

    def embed_documents(self, texts: Sequence[str]) -> np.ndarray:
        texts = list(texts)
        if not texts:
            return np.zeros((0, self.dim), dtype="float32")
        result: list[np.ndarray] = [np.zeros(self.dim, dtype="float32")] * len(texts)
        for start in range(0, len(texts), self.batch_size):
            batch = texts[start:start + self.batch_size]
            resp = self._client.embeddings.create(model=self.model, input=batch)
            items = sorted(resp.data, key=lambda d: d.index)
            for j, item in enumerate(items):
                result[start + j] = np.asarray(item.embedding, dtype="float32")
        return _l2_normalize(np.vstack(result))


class OfflineHashEmbedder(Embedder):
    """确定性哈希伪向量。**不是真 embedding，仅供验证链路，不可用于评测。**"""

    def __init__(self, dim: int = 256) -> None:
        self.dim = dim
        print("[warn] 离线模式：使用哈希伪向量，只有字面匹配能力，不可用于评测")

    def embed_documents(self, texts: Sequence[str]) -> np.ndarray:
        from bm25 import tokenize
        matrix = np.zeros((len(texts), self.dim), dtype="float32")
        for row, text in enumerate(texts):
            for token in tokenize(text):
                h = int(hashlib.md5(token.encode("utf-8")).hexdigest(), 16)
                matrix[row, h % self.dim] += 1.0
        return _l2_normalize(matrix)


def build_embedder(offline: bool = OFFLINE_EMBEDDING, **kwargs) -> Embedder:
    if offline:
        return OfflineHashEmbedder()
    return OpenAICompatibleEmbedder(**kwargs)
