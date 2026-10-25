"""
知识库切分 —— 按"一条知识"切，不按字符盲切。

Chunk 既是向量输入、也是 BM25 输入、也直接喂给 LLM，所以文本要同时
"语义密"和"字面全"（技术词如 DECR / TTL / -1 必须原样保留）。
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from typing import Any


@dataclass
class Chunk:
    chunk_id: str
    text: str
    metadata: dict[str, Any] = field(default_factory=dict)


def chunk_knowledge(knowledge: dict[str, dict[str, str]]) -> list[Chunk]:
    """knowledge = {大类: {主题: 正文}}。每个 (大类, 主题) 一个 chunk。"""
    chunks: list[Chunk] = []
    for category, topics in knowledge.items():
        for topic, text in topics.items():
            body = text.strip()
            full = f"库存运维知识 · {category} · {topic}\n{body}"
            digest = hashlib.md5(f"kb|{category}|{topic}".encode()).hexdigest()[:12]
            chunks.append(Chunk(
                chunk_id=f"kb_{digest}",
                text=full,
                metadata={"source": "knowledge", "category": category, "topic": topic},
            ))
    return chunks
