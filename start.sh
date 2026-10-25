#!/usr/bin/env bash
# ============================================================================
# 一键启动：库存运维中枢（FlashSale 自研引擎 + AI 运维 Agent）
#
# 用法（在 WSL / Linux 里执行，引擎依赖 sys/epoll.h 只能跑在 Linux）：
#   ./start.sh           真实模式（需要 DEEPSEEK_API_KEY + embedding 端点 + 引擎）
#   ./start.sh --demo    演示模式（离线：假模型 + 哈希伪向量，零配置）
#
# 这一条命令会按顺序完成：建虚拟环境 → 装依赖 → 编译引擎 → 起引擎 → 建索引 → 起 Web。
# 启动后浏览器打开 http://127.0.0.1:8000
# ============================================================================

set -euo pipefail

cd "$(dirname "$0")"
ROOT="$(pwd)"
AGENT_DIR="$ROOT/agent"
VENV="$AGENT_DIR/.venv"
ENGINE_BIN="$ROOT/mini_redis/mini_redis"
ENGINE_PID=""

# ── 解析参数：--demo 走离线演示 ──
DEMO=0
for a in "$@"; do case "$a" in --demo) DEMO=1;; esac; done
if [ "$DEMO" = "1" ]; then export DRY_RUN=1 RAG_OFFLINE=1; fi

# ── 1) 虚拟环境 + 依赖 ──
PY="$VENV/bin/python"
if [ ! -x "$PY" ]; then
  echo "▶ 创建虚拟环境：$VENV"
  python3 -m venv "$VENV"
fi
echo "▶ 安装依赖（按 requirements.txt）"
"$PY" -m pip install -q -r "$ROOT/requirements.txt"

# ── 2) 编译引擎（仅在二进制缺失或源码更新时）──
if [ ! -x "$ENGINE_BIN" ] || \
   [ -n "$(find "$ROOT/mini_redis/src" -name '*.cpp' -newer "$ENGINE_BIN" 2>/dev/null)" ]; then
  echo "▶ 编译 mini_redis 引擎（C++11）"
  ( cd "$ROOT/mini_redis" && g++ -std=c++11 -O2 -pthread -I include src/*.cpp -o mini_redis )
fi

# ── 3) 启动引擎（后台；若 6379 已在监听则跳过）──
if ! "$PY" -c "import socket;socket.create_connection(('127.0.0.1',6379),0.3)" 2>/dev/null; then
  echo "▶ 启动引擎（后台）"
  "$ENGINE_BIN" 6379 > "$ROOT/mini_redis/mini_redis.log" 2>&1 &
  ENGINE_PID=$!
  sleep 0.6
fi

# 退出时顺手关掉后台引擎
trap 'kill "$ENGINE_PID" 2>/dev/null || true' EXIT

# ── 4) 构建 RAG 索引 ──
echo "▶ 构建 RAG 索引"
( cd "$AGENT_DIR" && "$PY" main.py --build-index )

# ── 5) 启动 Web（前台，Ctrl+C 退出）──
echo "▶ 启动库存运维中枢  →  http://127.0.0.1:8000"
( cd "$AGENT_DIR" && DRY_RUN="${DRY_RUN:-0}" "$PY" web.py )
