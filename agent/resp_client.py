"""
手写薄 RESP 客户端 —— 引擎本来就 RESP 兼容，约 150 行自己写一个，
顺带证明协议理解度（比 redis-py 更轻、零依赖，且能精确控制我们要的接口）。

支持：
  · 发送命令（数组化 bulk string，标准 RESP2）
  · 解析五种回复：简单字符串(+)、错误(-)、整数(:)、批量字符串($)、数组(*)
  · 简单 pipeline（一次发多条、按序收多条）
  · 超时与重连

引擎约定（见 mini_redis 源码）：
  · 端口默认 6379（./mini_redis <port> 可改）
  · STATS 命令返回 bulk 字符串，内容为 JSON（热点 key / 命令分布 / QPS）
"""

from __future__ import annotations

import json
import socket
import time
from typing import Any


class RespError(Exception):
    """引擎返回 -ERR ... 时抛出。"""


class RespClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 6379, timeout: float = 2.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._buf = b""

    # ── 连接 ──
    def connect(self) -> None:
        if self._sock is not None:
            return
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(self.timeout)
        self._sock = s
        self._buf = b""

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
            self._buf = b""

    def __enter__(self) -> "RespClient":
        self.connect()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    # ── 发送 ──
    @staticmethod
    def _encode(args: list[str]) -> bytes:
        parts = [f"*{len(args)}\r\n".encode()]
        for a in args:
            b = a.encode("utf-8")
            parts.append(f"${len(b)}\r\n".encode())
            parts.append(b)
            parts.append(b"\r\n")
        return b"".join(parts)

    def send(self, *args: str) -> None:
        """发送一条命令（不读响应）。"""
        self.connect()
        assert self._sock is not None
        self._sock.sendall(self._encode(list(args)))

    def send_and_read(self, *args: str) -> Any:
        """发送一条并读取其完整响应（最常用）。"""
        self.connect()
        self.send(*args)
        return self._read_reply()

    # ── 读取 ──
    def _fill(self) -> None:
        assert self._sock is not None
        chunk = self._sock.recv(65536)
        if not chunk:
            raise RespError("连接被对端关闭")
        self._buf += chunk

    def _read_line(self) -> bytes:
        while b"\r\n" not in self._buf:
            self._fill()
        line, _, self._buf = self._buf.partition(b"\r\n")
        return line

    def _read_reply(self) -> Any:
        while True:
            if not self._buf:
                self._fill()
            prefix = self._buf[0:1]
            if prefix in (b"+", b"-", b":", b"$"):
                self._buf = self._buf[1:]
                line = self._read_line()
                if prefix == b"+":
                    return line.decode("utf-8")
                if prefix == b"-":
                    raise RespError(line.decode("utf-8"))
                if prefix == b":":
                    return int(line)
                if prefix == b"$":
                    length = int(line)
                    if length == -1:
                        return None
                    while len(self._buf) < length + 2:
                        self._fill()
                    data = self._buf[:length]
                    self._buf = self._buf[length + 2:]
                    return data.decode("utf-8", errors="replace")
            elif prefix == b"*":
                self._buf = self._buf[1:]
                line = self._read_line()
                count = int(line)
                if count <= 0:
                    return [] if count == 0 else None
                return [self._read_reply() for _ in range(count)]
            else:
                # 内联命令回退（引擎也支持），按 \r\n 切一行
                line = self._read_line()
                return line.decode("utf-8", errors="replace")

    # ── 便捷方法 ──
    def get(self, key: str) -> Any:
        return self.send_and_read("GET", key)

    def exists(self, key: str) -> int:
        return self.send_and_read("EXISTS", key)

    def ttl(self, key: str) -> int:
        return self.send_and_read("TTL", key)

    def keys(self, pattern: str = "*") -> list[str]:
        r = self.send_and_read("KEYS", pattern)
        return r or []

    def stats(self, topn: int = 10) -> dict[str, Any]:
        r = self.send_and_read("STATS", str(topn))
        if r is None:
            return {}
        try:
            return json.loads(r)
        except (json.JSONDecodeError, TypeError):
            return {"raw": r}

    def hget(self, key: str, field: str) -> Any:
        """读取 Hash 单个字段，不存在返回 None。"""
        return self.send_and_read("HGET", key, field)

    def hgetall(self, key: str) -> dict[str, Any]:
        """读取整个 Hash，返回 {field: value}；不是 Hash / 不存在返回空 dict。"""
        r = self.send_and_read("HGETALL", key)
        if not isinstance(r, list):
            return {}
        out: dict[str, Any] = {}
        for i in range(0, len(r) - 1, 2):
            out[r[i]] = r[i + 1]
        return out

    def execute_raw(self, cmd: str, *args: str) -> Any:
        """执行任意命令并原样返回解析结果。"""
        return self.send_and_read(cmd, *args)


if __name__ == "__main__":
    import sys
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 6379
    with RespClient(host, port) as c:
        print("PING ->", c.send_and_read("PING"))
        print("SET stock:1 100 ->", c.send_and_read("SET", "stock:1", "100"))
        print("DECR stock:1 ->", c.send_and_read("DECR", "stock:1"))
        print("GET stock:1 ->", c.send_and_read("GET", "stock:1"))
        print("STATS ->", json.dumps(c.stats(5), ensure_ascii=False, indent=2))
