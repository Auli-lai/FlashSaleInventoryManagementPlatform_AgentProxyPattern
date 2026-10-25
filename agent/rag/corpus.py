"""
运维知识库语料 —— 命令使用手册 + 排障经验。

结构约定（和 chunk_knowledge 对齐）：
    build_knowledge_base() -> {大类: {主题: 正文}}

chunk_knowledge 会把每个 (大类, 主题) 切成一个 chunk，正文同时用于
向量化、BM25 和直接喂给 LLM，所以技术词（DECR / TTL / -1 / -2 等）必须
原样保留，不能改写。

本文件是 RAG 的"源头真值"。要补充/修正知识，改这里，再重跑 build_index.py。
"""

from __future__ import annotations

import json
import os

# ===========================================================================
# 一、命令使用手册（30+ 条常用命令的语义与返回值约定）
# ===========================================================================

COMMANDS: dict[str, str] = {
    "GET": (
        "GET key —— 取字符串值。\n"
        "· 语法：GET key\n"
        "· 返回值：key 存在且为字符串则返回字符串；不存在返回 nil（RESP 的 $-1 空 bulk）；"
        "若 key 不是字符串类型，返回 WRONGTYPE 错误。\n"
        "· 秒杀场景：常用 GET stock:1 看库存整数值。值形如 \"100\" 才是整数库存。"
    ),
    "SET": (
        "SET key value [EX seconds] —— 写入字符串值，可选过期秒数。\n"
        "· 语法：SET key value EX 60\n"
        "· 返回值：成功返回 OK。EX 不传则永不过期（TTL 显示 -1）。\n"
        "· 注意：SET 会覆盖旧值（含旧 TTL）。秒杀初始化库存常用 "
        "SET stock:1 1000 EX 3600。"
    ),
    "INCR": (
        "INCR key —— 整数值原子 +1。\n"
        "· 语法：INCR key\n"
        "· 返回值：递增后的整数。key 不存在时当作 0 后 +1，返回 1。\n"
        "· 语义：若原值不是整数或超出 64 位有符号范围，返回错误。\n"
        "· 本引擎实现注意：INCRBY 的返回路径若用 (int)num 做截断，"
        "超过 int 范围会溢出/截断，属已知风险点。"
    ),
    "DECR": (
        "DECR key —— 整数值原子 -1。\n"
        "· 语法：DECR key\n"
        "· 返回值：递减后的整数。key 不存在时当作 0 后 -1，返回 -1。\n"
        "· 关键语义：DECR 可能把库存减成负数！标准秒杀流程必须『DECR 后读返回值判断』："
        "返回值 < 0 说明库存不足（应拒绝订单 / 回滚），>= 0 才放行。\n"
        "· 排障：若 GET 看到库存是负数，说明历史 DECR 没做余额校验。"
    ),
    "INCRBY": (
        "INCRBY key delta —— 整数值原子 +delta。\n"
        "· 语法：INCRBY key 100\n"
        "· 返回值：增加后的整数。本引擎用它承载 INCR（delta=1）。\n"
        "· 风险：返回路径若用 int 截断，delta 过大或基数过大时会溢出。"
    ),
    "DECRBY": (
        "DECRBY key delta —— 整数值原子 -delta。\n"
        "· 语法：DECRBY key 100\n"
        "· 返回值：减少后的整数。DECR 是其 delta=1 的特例。\n"
        "· 同样可能减成负数，需配合返回值校验防超卖。"
    ),
    "EXISTS": (
        "EXISTS key —— 判断 key 是否存在。\n"
        "· 语法：EXISTS key\n"
        "· 返回值：存在返回 1，不存在返回 0（整数）。\n"
        "· 秒杀前置校验常用：EXISTS stock:1 为假说明活动未初始化。"
    ),
    "DEL": (
        "DEL key [key ...] —— 删除一个或多个 key。\n"
        "· 语法：DEL stock:1\n"
        "· 返回值：被删除的 key 个数。\n"
        "· 危险命令，本 Agent 默认不直接调用。"
    ),
    "TTL": (
        "TTL key —— 查询剩余过期秒数。\n"
        "· 语法：TTL key\n"
        "· 返回值三种情况：\n"
        "    -2 = key 不存在；\n"
        "    -1 = key 存在但永不过期（没有设 EX）；\n"
        "    >=0 = 剩余存活秒数。\n"
        "· 排障高频问题：看到 -2 说明这个 key 已经没了（可能过期或被 DEL）；"
        "看到 -1 说明当初 SET 没带 EX。"
    ),
    "EXPIRE": (
        "EXPIRE key seconds —— 给 key 设置过期秒数。\n"
        "· 语法：EXPIRE stock:1 3600\n"
        "· 返回值：1 设置成功，0 表示 key 不存在。\n"
        "· 注意：重新 SET 同 key 会清掉 TTL（回到 -1）。"
    ),
    "KEYS": (
        "KEYS pattern —— 按通配符列出所有匹配的 key。\n"
        "· 语法：KEYS stock:*  或  KEYS *\n"
        "· 返回值：匹配的 key 数组。\n"
        "· 性能警告：KEYS 在主线程全量扫描，数据量大时会长时间阻塞主路径、"
        "导致 QPS 骤降甚至超时。生产不要用，本 Agent 仅做模糊匹配辅助。"
    ),
    "DBSIZE": (
        "DBSIZE —— 返回当前数据库中 key 的总数。\n"
        "· 语法：DBSIZE\n"
        "· 返回值：整数。秒杀场景可快速看库存 key 规模。"
    ),
    "FLUSHDB": (
        "FLUSHDB —— 清空当前库所有 key。\n"
        "· 语法：FLUSHDB\n"
        "· 返回值：OK。\n"
        "· 极度危险，会清空全部库存，本 Agent 绝不直接调用。"
    ),
    "INFO": (
        "INFO [section] —— 返回服务器运行信息（Redis 兼容）。\n"
        "· 语法：INFO  /  INFO stats\n"
        "· 返回值：多行文本。本引擎重点关注 memory / stats / replication 段。\n"
        "· 与本项目新增的 STATS 区别：INFO 是通用指标，STATS 是本项目定制的"
        "实时运维视图（QPS 估算、热点 key、命令分布）。"
    ),
    "STATS": (
        "STATS [topn] —— 本项目自研的实时运维统计命令（默认 topn=10）。\n"
        "· 语法：STATS 10\n"
        "· 返回值：一段 JSON（RESP bulk 包裹），包含：\n"
        "    uptime_sec 运行时长、total_commands 累计命令数、connections 当前连接数、\n"
        "    qps_estimate 估算 QPS（最近 5 秒滑动窗口）、qps_window_secs 窗口秒数、\n"
        "    cmd_counts 各命令类型计数、hot_keys 访问 TopN。\n"
        "· 设计要点：统计走独立 stats_mtx，与存储主锁 mtx 完全隔离，"
        "不影响 INCR/DECR 主路径性能。只读，安全。"
    ),
    "HSET": (
        "HSET key field value —— 写入哈希字段。\n"
        "· 语法：HSET item:1 name iphone price 5999（多 field-value 对）\n"
        "· 返回值：新增的 field 数（已存在的字段更新不计数）。\n"
        "· 秒杀场景常用于存商品属性（价格、名称），与库存计数分离。"
    ),
    "HGET": (
        "HGET key field —— 取哈希单个字段值。\n"
        "· 语法：HGET item:1 price\n"
        "· 返回值：字段值字符串；字段或 key 不存在返回 nil。"
    ),
    "HGETALL": (
        "HGETALL key —— 取哈希全部字段与值。\n"
        "· 语法：HGETALL item:1\n"
        "· 返回值：field value 交替的数组。大哈希慎用（可能大 key）。"
    ),
    "LPUSH": (
        "LPUSH key value [value ...] —— 从列表左侧插入。\n"
        "· 语法：LPUSH queue:orders 1001 1002\n"
        "· 返回值：插入后列表长度。可用于异步下单队列。"
    ),
    "RPUSH": (
        "RPUSH key value [value ...] —— 从列表右侧插入。\n"
        "· 语法：RPUSH queue:orders 1003\n"
        "· 返回值：插入后列表长度。"
    ),
    "LRANGE": (
        "LRANGE key start stop —— 取列表区间（含两端）。\n"
        "· 语法：LRANGE queue:orders 0 -1\n"
        "· 返回值：区间内的元素数组。LRANGE 0 -1 取全部，大列表慎用。"
    ),
    "SADD": (
        "SADD key member [member ...] —— 集合添加成员。\n"
        "· 语法：SADD uids:win 1001 1002\n"
        "· 返回值：新增成员数。可用于中奖/已购用户去重。"
    ),
    "SMEMBERS": (
        "SMEMBERS key —— 返回集合全部成员。\n"
        "· 语法：SMEMBERS uids:win\n"
        "· 返回值：成员数组。大集合慎用。"
    ),
    "ZADD": (
        "ZADD key score member —— 有序集合添加（按 score 排序）。\n"
        "· 语法：ZADD rank:score 99 alice\n"
        "· 返回值：新增成员数。可用于排行榜。"
    ),
    "ZSCORE": (
        "ZSCORE key member —— 取成员分数。\n"
        "· 语法：ZSCORE rank:score alice\n"
        "· 返回值：分数字符串；成员不存在返回 nil。"
    ),
    "ZRANGE": (
        "ZRANGE key start stop [WITHSCORES] —— 按排名取区间。\n"
        "· 语法：ZRANGE rank:score 0 9 WITHSCORES\n"
        "· 返回值：成员（及分数）。用于 TopN 排行榜。"
    ),
    "RENAME": (
        "RENAME key newkey —— 改名。\n"
        "· 语法：RENAME stock:1 stock:1:bak\n"
        "· 返回值：OK。危险，本 Agent 不直接调用。"
    ),
    "PING": (
        "PING —— 心跳/连通性探测。\n"
        "· 语法：PING\n"
        "· 返回值：PONG。健康检查用。"
    ),
    "HELLO": (
        "HELLO [version] —— 协议握手（Redis 6+ RESP3 入口）。\n"
        "· 语法：HELLO 2\n"
        "· 返回值：服务器能力信息。本引擎兼容 HELLO 2（RESP2）。"
    ),
    "CONFIG": (
        "CONFIG GET/SET param —— 运行时配置读写。\n"
        "· 语法：CONFIG GET maxmemory\n"
        "· 返回值：参数名与值。危险操作，本 Agent 不调用写。"
    ),
    "CLIENT": (
        "CLIENT LIST/SETNAME/KILL —— 客户端连接管理。\n"
        "· 语法：CLIENT LIST\n"
        "· 返回值：连接信息。KILL 可断连接，生产谨慎。"
    ),
}

# ===========================================================================
# 二、排障经验（20 条 —— 现象 + 原因 + 应对）
# ===========================================================================

TROUBLESHOOTING: dict[str, str] = {
    "decr_negative": (
        "现象：GET 库存 key 得到负数（如 -3）。\n"
        "原因：DECR 没有做『余额校验』就直接减了——并发或逻辑缺失导致库存被减穿。\n"
        "应对：标准做法是 DECR 后立刻读返回值，若 < 0 说明不足，应拒绝订单并把库存"
        "INCR 还原（回滚）；或改用 Lua/原子校验的扣减。本 Agent 的 simulate_decr 工具"
        "正是演示『DECR→判断→INCR 还原』这条安全路径。"
    ),
    "incr_overflow": (
        "现象：INCR/INCRBY 后值异常（变成很小或负数）。\n"
        "原因：本引擎 INCRBY 返回路径用 (int)num 做截断，超过 32 位有符号 int 范围"
        "（约 ±21 亿）会溢出/截断。\n"
        "应对：改用 64 位（long long / int64）承载计数与返回值；或确认库存量级不会触及。"
        "这是达到 100w+ QPS 前必须修正的隐患。"
    ),
    "ttl_minus_two": (
        "现象：TTL 返回 -2。\n"
        "含义：key 不存在（已过期、被 DEL、或从未写入）。\n"
        "应对：检查是否初始化失败、是否被误删、是否 TTL 设置过短被自然淘汰。"
        "秒杀开始前若库存 key 是 -2，说明没 SET 成功。"
    ),
    "ttl_minus_one": (
        "现象：TTL 返回 -1。\n"
        "含义：key 存在但永不过期（SET 时没带 EX）。\n"
        "应对：若希望活动结束自动失效，用 EXPIRE 补过期，或重新 SET 带 EX。"
    ),
    "hot_key_concentration": (
        "现象：STATS 的 hot_keys 里某几个 key 访问占比极高（如单个库存 key 占 90%）。\n"
        "含义：热点 key 集中在单分片/单线程，成为瓶颈。\n"
        "应对：库存拆多副本（stock:1:0..stock:1:31 分桶），扣减时随机选桶、"
        "查询时求和；或用本地缓存 + 异步扣减削峰。分桶是本引擎 100w+ QPS 的关键手段。"
    ),
    "qps_sudden_drop": (
        "现象：qps_report 显示 QPS 突然大幅下降。\n"
        "可能原因：① 跑了一条 KEYS * 阻塞主线程；② AOF 落盘（fsync）卡住；"
        "③ 连接数打满（见 connection_exhaustion）；④ 后端调用/慢查询；⑤ 客户端雪崩。\n"
        "应对：先看 STATS 的 cmd_counts 是否有异常命令（KEYS），再看连接数与"
        "AOF 积压，逐项排除。"
    ),
    "aof_backlog": (
        "现象：写命令堆积，AOF 文件增长但落盘慢，延迟抖动。\n"
        "原因：单线程既要处理命令又要同步写盘；fsync 频率过高或磁盘慢。\n"
        "应对：改用『组提交 + 批量 fsync』（攒一批再 fsync，降低系统调用次数），"
        "或用独立后台线程/进程做 AOF 重写与落盘，主线程只做 write 不 fsync。"
        "本引擎当前是后台线程每满 1000 条或 100ms fflush（非 fsync），"
        "可进一步加组提交提升确定性。"
    ),
    "connection_exhaustion": (
        "现象：连接数耗尽，新连接被拒或超时。\n"
        "原因：客户端没释放连接、没用连接池、或单实例连接上限太低。\n"
        "应对：客户端用连接池 + 正确释放；服务端提高连接上限；"
        "可引入代理层（如自研连接复用）。本引擎 STATS 的 connections 字段可监控。"
    ),
    "memory_growth": (
        "现象：内存持续上涨不回落。\n"
        "原因：大 key、未设过期的 key 累积、内存碎片、或统计结构（如 cmd_counts）"
        "无限增长。\n"
        "应对：给库存 key 设 TTL；定期清理；检查是否有 key 泄漏；"
        "观察 STATS 的 total_commands 与 key 数量增长速度。"
    ),
    "oversell": (
        "现象：实际卖出数量 > 库存数量（超卖）。\n"
        "原因：扣减不是原子校验，或 DECR 后没判断返回值就放行订单，"
        "或并发下『读-判断-写』非原子。\n"
        "应对：用 INCR/DECR 的原子返回值做唯一判据（DECR 返回值 < 0 即拒绝并回滚）；"
        "杜绝『先 GET 再判断再 DECR』的非原子写法。"
    ),
    "decr_not_restored": (
        "现象：演示/补偿扣减后库存没回到原值。\n"
        "原因：DECR 之后忘记 INCR 还原，或中途报错中断。\n"
        "应对：任何『只为了看效果』的扣减都必须配对 INCR 还原；"
        "用 try/finally 保证还原。本 Agent simulate_decr 严格遵循 DECR→读→INCR。"
    ),
    "keys_blocking": (
        "现象：执行 KEYS 后整体变慢、QPS 掉。\n"
        "原因：KEYS 在主线程全量扫描，O(N) 阻塞。\n"
        "应对：生产禁止 KEYS；用 SCAN 游标迭代（非阻塞）；"
        "本 Agent 仅用 KEYS 做模糊匹配的辅助发现，不用于大库全扫。"
    ),
    "inventory_inconsistent": (
        "现象：库存显示值与真实订单数对不上。\n"
        "原因：扣减与订单落库不是同一事务（异步丢消息）、重复扣减、"
        "或崩溃后 AOF 丢失部分命令。\n"
        "应对：扣减与订单状态机对齐；保证 AOF 组提交 + fsync 不丢；"
        "对账任务定期校准。"
    ),
    "resp_protocol_error": (
        "现象：客户端收到协议解析错误。\n"
        "原因：RESP 封包/解包错误（少 \r\n、bulk 长度不匹配、整数格式错）。\n"
        "应对：检查命令编码器是否严格按 RESP2（*N\\r\\n$len\\r\\n...\\r\\n），"
        "本引擎基于 RESP2 兼容实现。"
    ),
    "concurrent_race": (
        "现象：高并发下计数偶发错乱或超卖。\n"
        "原因：共享计数器的非原子操作、锁粒度不当。\n"
        "应对：INCR/DECR 走原子指令（本引擎已原子）；热点拆分减少单点竞争；"
        "必要时用无锁结构或分片。"
    ),
    "restart_data_loss": (
        "现象：重启后发现部分最新命令没生效（数据丢失）。\n"
        "原因：AOF 只 fflush 未 fsync，进程/机器崩溃时操作系统缓冲未落盘。\n"
        "应对：关键路径『组提交 + 每批 fsync』，保证一批命令落盘后才算成功；"
        "或 AOF 每秒 fsync（折中）。本引擎若要『机器挂都不丢』，必须加 fsync。"
    ),
    "slow_query": (
        "现象：个别命令延迟高。\n"
        "原因：大 key（HGETALL 巨大哈希）、KEYS、复杂命令、或落盘抖动态。\n"
        "应对：避免大 key；用 SCAN 替代 KEYS；拆分大结构；"
        "监控 STATS 的延迟分布。"
    ),
    "big_key": (
        "现象：单个 key 体积大，序列化/传输/删除都慢。\n"
        "原因：超大哈希/列表/集合，删除时阻塞主线程（DEL 是同步的）。\n"
        "应对：拆分大 key（按用户/时间分桶）；删除用异步/unlink 思路；"
        "预估容量时控制单 key 规模。"
    ),
    "cache_penetration": (
        "现象：大量查询不存在的 key，直接打到后端（或反复 miss）。\n"
        "原因：恶意或异常查询不存在的库存 key，缓存层没有保护。\n"
        "应对：对不存在的 key 缓存空值（短 TTL）；布隆过滤器前置拦截；"
        "本引擎可配合 EXISTS 先判存在。"
    ),
    "cache_avalanche": (
        "现象：大量 key 同时过期，瞬时请求全部失效打到后端。\n"
        "原因：key 过期时间集中（如都在活动结束点）。\n"
        "应对：过期时间加随机抖动（EXPIRE 基础上 + 随机秒）；"
        "分级缓存；预热。"
    ),
}


# ===========================================================================
# 组装与导出
# ===========================================================================

def build_knowledge_base() -> dict[str, dict[str, str]]:
    """返回 {大类: {主题: 正文}}，供 chunk_knowledge 切分。"""
    return {
        "命令使用手册": COMMANDS,
        "排障经验": TROUBLESHOOTING,
    }


def save_corpus(path: str | None = None) -> str:
    """把知识库原样落盘成 JSON，方便人工审阅/比对（不参与检索）。"""
    if path is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "knowledge_base", "corpus.json")
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(build_knowledge_base(), f, ensure_ascii=False, indent=2)
    return os.path.abspath(path)


if __name__ == "__main__":
    kb = build_knowledge_base()
    n_cmds = len(kb["命令使用手册"])
    n_tshoot = len(kb["排障经验"])
    print(f"命令手册 {n_cmds} 条，排障经验 {n_tshoot} 条，合计 {n_cmds + n_tshoot} 条")
    print("已落盘：", save_corpus())
