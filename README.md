# 秒杀库存管理中台 · FlashSaleInventoryManagementPlatform

手写 Redis 兼容存储引擎 + Qt5 桌面客户端 + AI 运维 Agent 的秒杀库存管理全栈项目。

- **存储引擎**：C++11 自研 `mini_redis`，RESP2 协议、epoll 事件驱动、5 种数据结构、SkipList 实现 ZSet、AOF 持久化。
- **防超卖核心**：原子 `DECR` + 回滚，单机实测 INCR/DECR **10w+ QPS**。
- **桌面端**：Qt5 客户端（Key 浏览器、秒杀下单、批量压测、实时库存监控、命令控制台）。
- **AI 运维层**：Python 在外围包一层自然语言运维 Agent（ReAct 循环 + 工具调用 + RAG 双路召回），默认只读，**不改动引擎核心逻辑**（仅新增一个只读 `STATS` 命令）。配套 FastAPI + SSE 三栏可视化运维台。

---

## 系统架构

```
┌──────────────────────────────────────────────────────────────┐
│                     Qt5 桌面客户端 (MiniRedisDesktop)          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐     │
│  │ Key 浏览器│ │ 业务操作  │ │ 业务监控  │ │ 命令控制台    │     │
│  └──────────┘ └──────────┘ └──────────┘ └──────────────┘     │
│                  RedisClient（RESP 协议 + FIFO 回调队列）       │
└───────────────────────────┬──────────────────────────────────┘
                            │  TCP/RESP  (127.0.0.1:6379)
┌───────────────────────────┴──────────────────────────────────┐
│               mini_redis（自研兼容 Redis 存储引擎）             │
│  ┌──────────────────────────────────────────────────────┐    │
│  │  String │ Hash │ List │ Set │ ZSet(SkipList 跳表)    │    │
│  └──────────────────────────────────────────────────────┘    │
│  ┌──────────┐ ┌──────────┐ ┌────────────────────────────┐    │
│  │ RESP 协议 │ │ AOF 日志  │ │ epoll 事件循环 · 边缘触发   │    │
│  │ 编解码器  │ │ 后台线程  │ │ 连接超时 · 惰性+定期过期    │    │
│  │          │ │ 批量落盘  │ │                            │    │
│  └──────────┘ └──────────┘ └────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
                            ▲
                            │  同一个引擎，两个独立 TCP 客户端，数据实时共享
┌───────────────────────────┴──────────────────────────────────┐
│  库存运维 Agent（Python，agent/ 目录）                          │
│  CLI(main.py) / Web(web.py, FastAPI + SSE) → InventoryAgent   │
│   → 手写 RESP2 客户端直连引擎，只读查询；RAG 检索本地知识库     │
└──────────────────────────────────────────────────────────────┘
```

---

## 核心设计：Decr-and-Check 防超卖

传统方案用 MySQL `UPDATE stock = stock - 1 WHERE stock > 0`，高并发下容易出现**行锁瓶颈**或**超卖**。

本系统使用 **Redis 原子 DECR + 回滚机制**：

```
下单请求
  │
  ▼
DECR flash:1:product:{id}:stock   ← 原子操作，返回剩余值
  │
  ├─ 返回值 ≥ 0 →  下单成功 → LPUSH 订单队列 → INCR 统计
  │
  └─ 返回值 < 0 →  库存不足 → INCR 回滚 → 返回失败
```

**零超卖保证**：`DECR` 是原子的，返回负数的请求都会 `INCR` 回滚，库存永远不会 `< 0`。秒杀库存计数器必须用 `SET` 预先初始化（否则 `DECR` 对不存在的 key 返回 `-1` 会误判）。

---

## 库存数据模型

库存全部存在 `mini_redis` 里，按 key 命名空间组织（种子数据见 `src/business_data.cpp`，幂等：仅当 `product:1001` 不存在时写入）：

| Key | 类型 | 含义 | 关键字段 |
|-----|------|------|----------|
| `product:{id}` | Hash | 商品档案 | `name / price / stock / sold / category / brand`（1001~1008，8 件覆盖 5 品类）|
| `flash:sale:1` | Hash | 秒杀活动配置 | `title / product_ids / discount_pct / start_time / end_time / limit_per_user / status` |
| `flash:1:product:{id}` | Hash | 单品秒杀信息 | `flash_price / flash_stock / sold` |
| `flash:1:product:{id}:stock` | **String** | **秒杀库存计数器** | 整数，靠 `DECR` 原子扣减防超卖（高 QPS 热路径）|
| `stats:daily:*` | String | 运营统计计数 | `orders / revenue / visitors` |
| `orders:pending` | List | 订单队列 | 下单成功时 `LPUSH` |

> 引擎底层是 `std::map<string, RedisObject*>`，`RedisObject` 用 `type` 标签挂 5 种结构；**统计锁 `stats_mtx` 与数据主锁 `mtx` 隔离**，统计写入绝不会拖慢或破坏 `INCR`/`DECR` 扣减正确性（设计要点，也是 10w+ → 100w+ 演进中热路径上要盯的点）。

---

## 库存运维 Agent（AI 层）

在引擎之上加一层**自然语言运维 Agent**：用中文问「库存还有多少 / QPS 多少 / DECR 返回负数是什么意思」，它会**自己决定调哪个工具、查实时数据或检索知识库、再给结论**。默认只读，绝不碰删除 / 真实扣减 / 改生产配置等危险命令。

### 架构

```
┌──────────────────────────────────────────────────────────────┐
│  CLI（main.py） / Web（web.py, FastAPI + SSE 三栏可视化）       │
├──────────────────────────────────────────────────────────────┤
│  InventoryAgent（ReAct 循环：思考→调工具→回灌→出结论）          │
│   · 容错：参数校验 / 重复调用拦截 / 轮次上限 / 上下文裁剪 /      │
│           强制收尾 / LLM 出错降级                              │
├──────────────────────────────────────────────────────────────┤
│  6 个工具（tools.py）                                          │
│   get_key · hot_keys · qps_report · stock_query ·              │
│   simulate_decr（模拟扣减，显式标注并还原）· knowledge_qa      │
├──────────────┬───────────────────────────────────────────────┤
│ DeepSeek      │ RAG 检索层（rag/）                             │
│ deepseek-flash│  向量召回 + BM25 召回 → RRF 融合(k=60)         │
│ OpenAI 兼容   │  rerank：NoOp 占位（MVP 不接真模型）           │
├──────────────┴───────────────────────────────────────────────┤
│  手写 RESP2 客户端 ──▶ mini_redis 引擎（STATS 只读命令）        │
└──────────────────────────────────────────────────────────────┘
```

**关键决策**：LLM 用 **DeepSeek**（`deepseek-flash`，OpenAI 兼容协议，Key 读 `DEEPSEEK_API_KEY`）。注意 **DeepSeek 不提供 embedding 接口**，所以 RAG 向量化走可配置的 OpenAI 兼容端点（默认本地 Ollama `nomic-embed-text`），离线可用哈希伪向量跑通链路。

### 6 个工具（tools.py）

| 工具 | 作用 | 说明 |
|------|------|------|
| `get_key` | 查单个 key 详情（值 / TTL / 是否存在）| 通用；支持 key 名模糊匹配 |
| `hot_keys` | 访问最多的 TopN 热点 key | 来自 `STATS`，看全局排行 |
| `qps_report` | 引擎负载概览（QPS / 连接数 / 命令分布）| 来自 `STATS` |
| `stock_query` | 秒杀库存余量 + 超卖判断 | 专指库存语义，判断是否 ≤0 / 负数 |
| `simulate_decr` | 演示一次「扣减-还原」 | **只读验证**，DECR 后必 INCR 还原，**不影响生产库存** |
| `knowledge_qa` | 在本地知识库做混合检索 | 查命令手册 / 排障经验，不连引擎 |

工具层有三条铁律：`run()` 永远返回 dict 永不抛异常；key 名支持模糊匹配（`KEYS` + 子串 + `difflib` 相似度兜底）；description 写清「什么时候调」和「与相似工具怎么区分」。

### RAG：双路召回 + RRF

- **切分**：按知识条目切（`chunking.py`），入参 `{大类: {主题: 正文}}`，每个 `(大类, 主题)` 一篇 chunk——**不是固长、也不是递归字符切**。理由：chunk 同时喂给向量、BM25 和 LLM，必须「语义密 + 字面全」，`DECR/TTL/-1` 等技术词原样保留。语料 51 条（30 命令手册 + 20 排障经验，`corpus.py`）。
- **Embedding**：`OpenAICompatibleEmbedder`，默认 **Ollama `nomic-embed-text` 768 维、L2 归一、分批 32**；可选 OpenAI `text-embedding-3-small`；离线用 `OfflineHashEmbedder`（256 维哈希伪向量，仅验证链路，不可用于评测）。
- **双路召回**：`HybridRetriever` = 向量召回（`VectorStore`）+ BM25 召回（`BM25Index`）→ **RRF 融合**（`k=60`，只用排名不用分数，绕开两路分数量纲不可比）。
- **Rerank**：`NoOpReranker` 占位，MVP 阶段不接真 rerank 模型（语料 < 1000 条，Top10 人工可评估），接口已留好待日后替换。

### Agent 设计：ReAct 循环与容错

核心是一个 **ReAct 循环**（`agent.py` 的 `InventoryAgent`），同步 `run()` 与流式 `run_stream()` 共用全部容错。节点流：

1. **System Prompt**：角色设定为「资深库存运维工程师，面向可能非技术的运营」，定死纪律——先观察再推理再调工具；**只依据工具返回数据下结论，绝不编造库存/QPS 数字**；默认只读。
2. **Session**：极简多轮上下文，只存消息列表，让「那个 key 呢？」这类追问能被理解。
3. **主循环**（最多 8 轮）：每轮先 `trim` 超长上下文（>24000 字符只改旧工具结果内容、不删消息），再调模型。
4. **模型决策**：要调工具 → `_execute` 容错执行（参数校验 / 重复调用拦截 `max_identical_calls=2` / 真正运行 / 结果回灌），循环回到模型；不要工具 → 流式出结论 → `finish`。
5. **两个出口**：达轮次/工具上限 → **强制结论**（nudge 模型「不要再调工具，直接基于已有数据给结论」，用空 tools 收尾）；LLM 调用异常 → **降级**（把已采集的工具原始数据返回，标注未分析，而不是直接崩）。

> 真实工程里 30 行 ReAct 骨架能跑通演示，但上不了生产——上面这些容错（参数永远合法 / 工具永远成功 / 不会打转 / 上下文不爆 / 总会停下来）才是这个文件的真实工作量。

### 可视化运维台（web.py）

FastAPI + SSE，浏览器打开 `http://127.0.0.1:8000`，三栏布局：**商品信息**（类别 / 库存 / 状态徽章，来自 `/api/products`）· **执行轨迹**（AI 思考与工具调用实时滚动）· **对话历史**（结论留存，刷新可恢复）。顶栏含引擎状态点、配置透明化（真实 / 演示回退）、**暗色/亮色切换**（CSS 变量 + localStorage）。

相对原型的六项升级：① 运行时长客户端每秒实时反推且为整数秒；② 商品信息面板；③ API Key 透明化（无 Key 自动回退演示模式，顶部 banner 提示）；④ AI 思考全流程 SSE 流式输出；⑤ 历史对话留存（`/api/history` + 服务端 `SESSION`/`HISTORY`）；⑥ 暗色/亮色主题切换。

### 引擎侧新增：STATS 命令

为 Agent 提供数据，引擎新增一个**只读**命令（不影响 INCR/DECR 主路径）：

```
STATS [topn]    返回 JSON（RESP bulk 包裹）：
  uptime_sec / total_commands / connections /
  qps_estimate（最近 5 秒滑动窗口）/ qps_window_secs /
  cmd_counts（命令类型分布）/ hot_keys（访问 TopN）
```

设计要点：统计走**独立的 `stats_mtx`**，与存储主锁 `mtx` 完全隔离，`record_command` 用保护包裹，绝不会拖慢或破坏扣减正确性。

---

## 性能（redis-benchmark 实测，单机）

```
场景：8 件秒杀商品，库存各 50，模拟 1000 并发下单
  成功: 400 单（库存耗尽，DECR ≥ 0）
  失败(库存不足): 600 单（DECR < 0 → INCR 回滚）
  超卖: 0 单（DECR 原子性保证）

redis-benchmark -t incr   →  INCR-QPS  ≈ 192,455
redis-benchmark -t decr   →  DECR-QPS  ≈ 193,124
```

> 满足绝大多数电商秒杀场景；当前目标是从 10w+ 进一步演进到 100w+ QPS（分片 / 批处理 / 无锁结构等方向探索中）。

---

## 功能矩阵

### 业务层（Qt 客户端）

| 功能 | 说明 |
|------|------|
| 秒杀下单 | 选择活动/商品，输入用户ID，原子 DECR 扣减库存 |
| 批量压测 | 自定义并发数（1~100000），一键模拟高并发秒杀 |
| 库存管理 | 调整库存/已售，秒杀商品自动同步 flash 计数器 |
| CSV 导入 | 批量导入商品数据，自动识别表头/分隔符/品类 |
| 手动添加 | 添加商品时选择普通/秒杀类型，自动创建 flash 数据 |

### 监控层（Qt 客户端）

| 功能 | 说明 |
|------|------|
| KPI 卡片 | 今日订单 / 营收 / 访客 / 待处理 / 秒杀商品数 |
| 库存仪表 | 8 件商品实时库存进度条（绿/橙/红三级预警）|
| 销售排行 | ZSet 排名 TOP 10 |
| 自动刷新 | 轮询，首次缓存元数据后续增量更新 |

### 存储引擎层

| 功能 | 说明 |
|------|------|
| 5 种数据结构 | String / List / Hash / Set / ZSet（SkipList 跳表实现）|
| 原子操作 | INCR / DECR / INCRBY / DECRBY 线程安全 |
| AOF 持久化 | 写命令 RESP 编码追加，独立后台线程批量落盘，启动重放恢复 |
| 键过期 | 惰性删除 + 定期扫描，TTL 支持 |
| RESP 协议 | 完整兼容 Redis 序列化协议，任意 Redis 客户端可连接 |

---

## 快速开始

### 环境

- **引擎**：Linux / Windows WSL2（依赖 `sys/epoll.h`，只能在 Linux/WSL 构建运行）。
- **桌面端**：需 Qt 5.12+（Widgets + Network）+ CMake 3.10+；Windows 下用 WSLg 显示。
- **Agent**：Python 3.10+，见 `agent/requirements.txt`（numpy / openai / jieba / fastapi / uvicorn；FAISS 可选，不装自动回退 numpy 暴力检索）。

### 编译引擎（仅 Linux/WSL）

```bash
# 方式 A：直接用 mini_redis 子目录（无需 Qt5）
cd mini_redis && g++ -std=c++11 -O2 -pthread -I include src/*.cpp -o mini_redis

# 方式 B：从根目录 CMake，关掉 Qt 桌面端
cmake -DBUILD_DESKTOP=OFF -S . -B build && cmake --build build

# 启动引擎（后台）
./mini_redis 6379 > mini_redis.log 2>&1 &
```

### 桌面端（Qt）

```bash
mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd bin/ && ./MiniRedisDesktop
```

启动后点击工具栏 **▶ Start Server** → **Connect**，自动初始化 8 件示例秒杀商品。

### Agent + 可视化运维台（一键 / 手动）

引擎依赖 Linux，以下步骤在 **WSL / Linux** 执行：

```bash
# 一条命令：建 venv → 装依赖 → 编译引擎 → 起引擎 → 建索引 → 起 Web
./start.sh            # 真实模式（需 DEEPSEEK_API_KEY + embedding 端点 + 引擎）
./start.sh --demo     # 演示模式（离线：假模型 + 哈希伪向量，零配置）
```

启动后浏览器打开 **http://127.0.0.1:8000**。

<details>
<summary>手动分步（等价于 start.sh 内部步骤）</summary>

```bash
# 1) Python 环境（在 agent/ 目录下）
cd agent
python3 -m venv .venv
.venv/bin/pip install -r ../requirements.txt

# 2) 起引擎（见上方「编译引擎」），确保 127.0.0.1:6379 在跑

# 3) 建 RAG 索引（演示用离线哈希即可；真实检索置 RAG_OFFLINE=0）
RAG_OFFLINE=1 .venv/bin/python main.py --build-index

# 4) 启动 Web（必须在 agent/ 目录下，模块导入依赖该目录）
.venv/bin/python -m uvicorn web:app --port 8000
#   演示模式：先 export DRY_RUN=1 再起 uvicorn
```

CLI 用法：

```bash
python main.py --question "库存还有多少"          # 单次真实诊断
python main.py --dry-run --question "DECR 返回负数是什么"   # 离线演示
python main.py                                   # 交互问答（支持追问）
python main.py --list-tools                      # 列出 6 个工具
python main.py --question "..." --trace          # 附带工具调用轨迹
python main.py --question "..." --json           # 结构化 JSON 输出
```

</details>

> ⚠️ **启动 Web 的两个常见坑**
> 1. FastAPI 服务是 **`agent/web.py`**，命令是 `uvicorn web:app`（模块名 `web` + 对象名 `app`），不是 `uvicorn app:app`。
> 2. 必须在 **`agent/` 目录**下执行（模块导入依赖该目录）；跑 Web 前需装好依赖、建好索引；要实时数据还需引擎在 6379 运行。
> 3. 未配 `DEEPSEEK_API_KEY` 时 Web 会自动回退演示模式（顶部 banner 提示），不影响看界面和链路形状。

### 配置（.env）

复制 `.env.example` 为 `.env` 后填写（`.env` 已被 `agent/config.py` 自动加载，勿提交进 git）：

```ini
# LLM：DeepSeek（OpenAI 兼容）
DEEPSEEK_API_KEY=           # 真实模式必填
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-flash

# 库存引擎连接目标
ENGINE_HOST=127.0.0.1
ENGINE_PORT=6379
ENGINE_TIMEOUT=2.0

# 向量化：OpenAI 兼容端点（DeepSeek 无 embedding）
EMBEDDING_BASE_URL=http://localhost:11434/v1   # 默认本地 Ollama
EMBEDDING_MODEL=nomic-embed-text              # ollama pull nomic-embed-text
EMBEDDING_DIM=768

# 离线冒烟：置 1 用哈希伪向量（不调 embedding API）
RAG_OFFLINE=1
```

---

## 项目结构

```
FlashSaleInventoryManagementPlatform/
├── CMakeLists.txt              # 根构建（Qt 桌面端可选，mini_redis 始终构建）
├── README.md
├── requirements.txt            # Agent Python 依赖
├── start.sh                    # 一键启动（真实 / --demo）
├── .env.example                # Agent 配置样例
├── test_products.csv           # CSV 导入测试数据
├── include/  src/              # Qt5 桌面客户端（GUI + RESP 客户端 + 种子数据）
├── mini_redis/                 # 自研 Redis 兼容存储引擎（C++11）
│   ├── include/  src/
│   │   ├── db.cpp              # 存储引擎 + AOF 持久化 + SkipList + 过期
│   │   ├── command.cpp         # 命令表（含 STATS）+ RESP 响应构造
│   │   ├── server.cpp          # epoll 事件循环
│   │   ├── epoll.cpp / resp_parser.cpp / timer.cpp / log.cpp
└── agent/                      # 库存运维 Agent（Python）
    ├── config.py               # 所有外部依赖坐标
    ├── llm_client.py           # DeepSeek 封装 + 流式 + 假模型
    ├── tools.py                # 6 个工具定义 + 执行层（模糊匹配 key）
    ├── resp_client.py          # 手写 RESP2 客户端
    ├── agent.py                # ReAct 循环 + 容错 + Session 多轮
    ├── web.py                  # FastAPI + SSE 三栏可视化
    ├── main.py                 # CLI 入口
    ├── offline_demo.py         # 假模型 + 假数据
    ├── rag/                    # BM25 / 向量索引 / 混合检索 / 语料 / 索引构建
    └── eval/                   # 评测集 + 运行器
```

---

## 技术亮点

| 层面 | 技术方案 | 关键词 |
|------|----------|--------|
| 数据结构 | SkipList 跳表实现 ZSet，O(log n) 插入/查找 | 自研数据结构 |
| 并发控制 | `std::mutex` 保护 store 全局锁；统计走独立 `stats_mtx` | 互斥锁，临界区隔离 |
| 防超卖 | DECR 返回值判断 + INCR 回滚，零超卖 | 原子操作，乐观锁 |
| 网络模型 | epoll ET + EPOLLONESHOT + 非阻塞 IO + 发送缓冲 | 事件驱动，高并发 |
| 协议 | 手写 RESP 编解码，状态机解析，支持流水线 | 协议设计，序列化 |
| 持久化 | AOF 追加日志 + 独立后台线程批量落盘 + 启动重放 | 数据可靠性 |
| Agent | ReAct 循环 + 工具调用 + 参数校验/重复拦截/强制收尾 | LLM Agent，容错 |
| RAG | 向量 + BM25 双路召回 → RRF 融合 | 混合检索，无重排占位 |
| 可视化 | FastAPI + SSE 流式 + 暗/亮主题 + 历史留存 | 实时，可观测 |

---

## 许可证

MIT License
