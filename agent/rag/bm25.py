"""
BM25 稀疏检索 —— 自己实现，不依赖 rank_bm25。

本文件改编自 net-pluse 的 bm25.py。库存场景里很多查询是强字面的
（"DECR 返回负数"、"TTL 返回 -1"），向量语义检索容易漏，BM25 恰好补足。

分词：优先用 jieba（中文分词更准），若环境没装 jieba 则退化为
「CJK 单字 + 拉丁/数字整词」的兜底方案，保证检索链路不依赖额外安装。
"""

from __future__ import annotations

import math
import re
from collections import Counter
from typing import Sequence

try:
    import jieba
    _HAS_JIEBA = True
except ImportError:  # pragma: no cover
    _HAS_JIEBA = False

# 技术词必须整块保留（DECR / stock:1 / -1 / 50ms 这类）
_TOKEN_PATTERNS = [
    r"\d{1,3}(?:\.\d{1,3}){3}",                       # IPv4
    r"[a-zA-Z]+[a-zA-Z0-9_]*\d+",                     # eth0 / stock:1 / wlan0
    r"-?\d+(?:\.\d+)?(?:ms|pps|MB/s|Mbps|dBm|%)?",     # 负数 / 50ms / 3.2%
    r"\d+\.\d+",                                       # 浮点
]
_PLACEHOLDER = "zqzqzq{}zqzqzq"
_TECH_TOKEN_RE = re.compile("|".join(_TOKEN_PATTERNS))

_STOPWORDS = set(
    "的 了 在 是 和 与 及 或 有 为 对 从 到 被 把 就 都 也 还 而 但 这 那 "
    "一个 一种 我们 你们 他们 它 会 能 要 可以 应该 可能 什么 怎么 如何 "
    "哪些 哪个 时候 情况 问题 上面 下面 里面 表示 说明 如果 那么 因为 所以".split()
)


def tokenize(text: str) -> list[str]:
    placeholders: dict[str, str] = {}

    def _stash(match: re.Match) -> str:
        token = match.group(0)
        key = _PLACEHOLDER.format(len(placeholders))
        placeholders[key] = token
        return key

    masked = _TECH_TOKEN_RE.sub(_stash, text)
    tokens: list[str] = []
    if _HAS_JIEBA:
        pieces = list(jieba.cut(masked))
    else:
        # 兜底：CJK 单字 + 非 CJK 整词
        pieces = re.findall(r"[一-鿿]|[a-zA-Z0-9_]+|[^\s一-鿿a-zA-Z0-9_]", masked)

    for piece in pieces:
        piece = piece.strip()
        if not piece:
            continue
        if piece in placeholders:
            tokens.append(placeholders[piece].lower())
            continue
        if piece in _STOPWORDS:
            continue
        if not re.search(r"[\w一-鿿]", piece):
            continue
        tokens.append(piece.lower())
    return tokens


class BM25Index:
    K1 = 1.5
    B = 0.75

    def __init__(self, doc_ids: Sequence[str], doc_texts: Sequence[str]) -> None:
        if len(doc_ids) != len(doc_texts):
            raise ValueError("doc_ids 和 doc_texts 长度必须一致")
        self.doc_ids = list(doc_ids)
        self.n_docs = len(self.doc_ids)
        self._doc_tf: list[Counter[str]] = []
        self._inverted: dict[str, set[int]] = {}
        self._doc_len: list[int] = []
        for i, text in enumerate(doc_texts):
            tf = Counter(tokenize(text))
            self._doc_tf.append(tf)
            self._doc_len.append(len(tf))
            for term in tf:
                self._inverted.setdefault(term, set()).add(i)
        self._avgdl = (sum(self._doc_len) / self.n_docs) if self.n_docs else 0.0
        self._idf: dict[str, float] = {}
        for term, doc_set in self._inverted.items():
            df = len(doc_set)
            self._idf[term] = math.log(1.0 + (self.n_docs - df + 0.5) / (df + 0.5))

    def search(self, query: str, top_k: int = 20) -> list[tuple[str, float]]:
        query_terms = tokenize(query)
        if not query_terms:
            return []
        scores: dict[int, float] = {}
        for term in query_terms:
            doc_set = self._inverted.get(term)
            if not doc_set:
                continue
            idf = self._idf[term]
            for doc_idx in doc_set:
                tf = self._doc_tf[doc_idx][term]
                dl = self._doc_len[doc_idx]
                denom = tf + self.K1 * (1 - self.B + self.B * dl / self._avgdl)
                scores[doc_idx] = scores.get(doc_idx, 0.0) + idf * tf * (self.K1 + 1) / denom
        ranked = sorted(scores.items(), key=lambda kv: kv[1], reverse=True)[:top_k]
        return [(self.doc_ids[i], s) for i, s in ranked]

    def __len__(self) -> int:
        return self.n_docs


if __name__ == "__main__":
    idx = BM25Index(
        ["c1", "c2", "c3"],
        [
            "DECR key 将整数值减 1，返回减后的值。若返回负数说明库存不足。",
            "TTL key 返回剩余秒数，-1 表示永不过期，-2 表示 key 不存在。",
            "INCR key 将整数值加 1，常用于秒杀库存计数。",
        ],
    )
    print("查询「DECR 返回负数」 ->", idx.search("DECR 返回负数", top_k=3))
    print("查询「TTL -1 什么意思」->", idx.search("TTL -1 什么意思", top_k=3))
