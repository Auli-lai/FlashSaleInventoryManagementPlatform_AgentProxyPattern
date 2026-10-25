#include "db.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <cstdint>

// 把字符串转义成 JSON 字符串字面量（key 名可能含特殊字符）
static std::string json_escape(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

// ═══════════════════════════════════════════════════════════
//  跳表实现
// ═══════════════════════════════════════════════════════════

SkipList::SkipList() : level(0), count(0), rng(std::random_device{}()) {
    header = new SkipListNode("", -INFINITY, MAX_LEVEL);
}

SkipList::~SkipList() {
    SkipListNode* cur = header;
    while (cur) {
        SkipListNode* next = cur->forward[0];
        delete cur;
        cur = next;
    }
}

int SkipList::random_level() {
    int lvl = 1;
    std::uniform_int_distribution<int> dist(0, 3);
    while (lvl < MAX_LEVEL && dist(rng) == 0) lvl++;
    return lvl;
}

void SkipList::insert(const std::string& member, double score) {
    SkipListNode* update[MAX_LEVEL] = {};
    SkipListNode* cur = header;

    // 从最高层往下找插入位置
    for (int i = level; i >= 0; --i) {
        while (cur->forward[i] &&
               (cur->forward[i]->score < score ||
                (cur->forward[i]->score == score && cur->forward[i]->member < member))) {
            cur = cur->forward[i];
        }
        update[i] = cur;
    }

    // 检查是否已存在
    cur = cur->forward[0];
    if (cur && cur->score == score && cur->member == member) {
        cur->score = score; // 更新 score
        return;
    }

    int new_level = random_level() - 1; // 0-indexed
    if (new_level > level) {
        for (int i = level + 1; i <= new_level; ++i)
            update[i] = header;
        level = new_level;
    }

    SkipListNode* node = new SkipListNode(member, score, new_level + 1);
    for (int i = 0; i <= new_level; ++i) {
        node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = node;
    }
    count++;
}

bool SkipList::remove(const std::string& member) {
    SkipListNode* update[MAX_LEVEL] = {};
    SkipListNode* cur = header;

    for (int i = level; i >= 0; --i) {
        while (cur->forward[i] && cur->forward[i]->member != member) {
            // 使用 score 加速查找: 实际上我们需要按 member 查找，
            // 但跳表是按 score 排序的。这里做一个简化：按 member 遍历。
            if (cur->forward[i]) cur = cur->forward[i];
            else break;
        }
        update[i] = cur;
    }

    // 简化: 从头遍历找到 member
    cur = header->forward[0];
    SkipListNode* target = nullptr;
    while (cur) {
        if (cur->member == member) {
            target = cur;
            break;
        }
        cur = cur->forward[0];
    }

    if (!target) return false;

    // 更新各层指针（使用已记录的 update 前驱节点）
    for (int i = 0; i <= level; ++i) {
        if (update[i]->forward[i] == target)
            update[i]->forward[i] = target->forward[i];
    }

    delete target;
    count--;

    while (level > 0 && header->forward[level] == nullptr) level--;
    return true;
}

double SkipList::get_score(const std::string& member) const {
    SkipListNode* cur = header->forward[0];
    while (cur) {
        if (cur->member == member) return cur->score;
        cur = cur->forward[0];
    }
    return 0.0;
}

int SkipList::get_rank(const std::string& member) const {
    int rank = 0;
    SkipListNode* cur = header->forward[0];
    while (cur) {
        if (cur->member == member) return rank;
        rank++;
        cur = cur->forward[0];
    }
    return -1;
}

std::vector<std::string> SkipList::range(int start, int stop) const {
    std::vector<std::string> result;
    int total = (int)count;
    if (start < 0) start = total + start;
    if (stop < 0) stop = total + stop;
    if (start < 0) start = 0;
    if (stop >= total) stop = total - 1;
    if (start > stop) return result;

    int idx = 0;
    SkipListNode* cur = header->forward[0];
    while (cur && idx <= stop) {
        if (idx >= start) result.push_back(cur->member);
        idx++;
        cur = cur->forward[0];
    }
    return result;
}

std::vector<std::pair<std::string, double>> SkipList::range_with_scores(int start, int stop) const {
    std::vector<std::pair<std::string, double>> result;
    int total = (int)count;
    if (start < 0) start = total + start;
    if (stop < 0) stop = total + stop;
    if (start < 0) start = 0;
    if (stop >= total) stop = total - 1;
    if (start > stop) return result;

    int idx = 0;
    SkipListNode* cur = header->forward[0];
    while (cur && idx <= stop) {
        if (idx >= start) result.emplace_back(cur->member, cur->score);
        idx++;
        cur = cur->forward[0];
    }
    return result;
}

// ═══════════════════════════════════════════════════════════
//  数据库引擎
// ═══════════════════════════════════════════════════════════

Database::~Database() {
    aof_stop_writer();   // 先停后台线程并刷完剩余缓冲，再关闭文件
    if (aof_fp) fclose(aof_fp);
    for (auto& kv : store) delete kv.second;
}

// ── 辅助函数 ──

RedisObject* Database::get_or_create(const std::string& key, ValueType type) {
    auto it = store.find(key);
    if (it != store.end()) {
        // 类型检查: 如果类型不匹配，返回 nullptr
        if (it->second->type != type) return nullptr;
        return it->second;
    }
    RedisObject* obj = new RedisObject();
    obj->type = type;
    if (type == TYPE_ZSET) obj->zset_val = new SkipList();
    store[key] = obj;
    return obj;
}

RedisObject* Database::get_value(const std::string& key) {
    // 调用方必须持有 mtx
    auto it = store.find(key);
    if (it == store.end()) return nullptr;
    _expire_key_if_needed_nolock(key);
    it = store.find(key); // 可能已被删除
    if (it == store.end()) return nullptr;
    return it->second;
}

bool Database::is_expired(const std::string& key) const {
    auto it = expires.find(key);
    if (it == expires.end()) return false;
    return time(nullptr) > it->second;
}

void Database::expire_key_if_needed(const std::string& key) {
    // 公开接口：自己加锁（外部调用安全）
    std::lock_guard<std::mutex> lock(mtx);
    if (is_expired(key)) {
        _del_nolock({key});
    }
}

void Database::_expire_key_if_needed_nolock(const std::string& key) {
    if (is_expired(key)) {
        _del_nolock({key});
    }
}

void Database::check_expire(const std::string& key) {
    expire_key_if_needed(key);
}

void Database::active_expire_cycle(int max_samples) {
    // 采样检查：最多检查 max_samples 个过期 key
    std::vector<std::string> to_del;
    {
        std::lock_guard<std::mutex> lock(mtx);
        time_t now = time(nullptr);
        int sampled = 0;
        for (auto& kv : expires) {
            if (++sampled > max_samples) break;
            if (now > kv.second) to_del.push_back(kv.first);
        }
    }
    for (auto& key : to_del) {
        del({key});
    }
    if (!to_del.empty())
        Log::info("Expired %zu keys", to_del.size());
}

bool Database::match_pattern(const std::string& pattern, const std::string& key) const {
    // 简单通配符匹配: * 匹配任意字符序列, ? 匹配单个字符
    if (pattern == "*") return true;
    size_t pi = 0, ki = 0;
    size_t ps = pattern.size(), ks = key.size();
    while (pi < ps && ki < ks) {
        if (pattern[pi] == '*') {
            pi++;
            if (pi >= ps) return true; // 末尾的 * 匹配剩余全部
            // 找到下一个 * 或末尾
            while (ki < ks) {
                if (match_pattern(pattern.substr(pi), key.substr(ki))) return true;
                ki++;
            }
            return false;
        } else if (pattern[pi] == '?' || pattern[pi] == key[ki]) {
            pi++; ki++;
        } else {
            return false;
        }
    }
    while (pi < ps && pattern[pi] == '*') pi++;
    return pi == ps && ki == ks;
}

// ── AOF 持久化 ──

void Database::aof_append(const std::vector<std::string>& args) {
    if (aof_filename.empty() || loading) return;

    // 以标准 RESP 协议格式记录（*N\r\n$len\r\ndata...），保证：
    //  1. value 里含空格/换行也不会被破坏（旧的纯文本格式会解析错乱）；
    //  2. 启动时能被 load_aof() 完整解析回放。
    std::string line;
    line.reserve(16 + args.size() * 8);
    line += "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args) {
        line += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    }

    {
        std::lock_guard<std::mutex> lock(aof_mtx);
        // 首次使用 AOF 时启动后台落盘线程
        if (!aof_thread_started) {
            aof_thread_started = true;
            aof_thread = std::thread(&Database::aof_writer_loop, this);
        }
        aof_buf += line;               // 只追加内存缓冲，不做任何磁盘 I/O
        if (++aof_buf_cmds >= 1000)    // 积压够多就唤醒后台线程批量落盘
            aof_cond.notify_one();
    }
}

// 后台 AOF 落盘线程主循环
void Database::aof_writer_loop() {
    while (true) {
        std::string batch;
        {
            std::unique_lock<std::mutex> lock(aof_mtx);
            // 唤醒条件：缓冲里有数据（达到 1000 条会被 aof_append notify），
            // 或收到停止信号；即使一直不满足，每 100ms 也会主动醒来一次，
            // 保证少量写入也能及时落盘。
            aof_cond.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return aof_stop || !aof_buf.empty();
            });
            if (aof_buf.empty()) {
                if (aof_stop) break;   // 已停止且没有剩余数据，退出
                continue;              // 空转，继续等
            }
            std::swap(batch, aof_buf); // 取走整个缓冲，请求线程可以继续追加
            aof_buf_cmds = 0;
        }
        if (!batch.empty()) aof_write_batch(batch);   // 在锁外写盘，不阻塞请求线程
    }
}

void Database::aof_write_batch(const std::string& data) {
    if (!aof_fp) {
        aof_fp = fopen(aof_filename.c_str(), "a");
        if (!aof_fp) {
            Log::error("Failed to open AOF file: %s", aof_filename.c_str());
            return;
        }
        // 大缓冲减少 write 系统调用次数（真正的慢 I/O 已在后台线程，这里再省一点）
        setvbuf(aof_fp, nullptr, _IOFBF, 4 * 1024 * 1024);
    }
    size_t written = fwrite(data.data(), 1, data.size(), aof_fp);
    if (written != data.size())
        Log::error("AOF write short: %zu/%zu bytes", written, data.size());
    // fflush 把数据交给内核页缓存。注意这不是 fsync：最多丢最近 ~100ms 的数据；
    // 生产级 Redis 用 appendfsync everysec 做定时 fsync，这里为吞吐先不引入。
    fflush(aof_fp);
}

void Database::aof_stop_writer() {
    {
        std::lock_guard<std::mutex> lock(aof_mtx);
        aof_stop = true;
    }
    aof_cond.notify_all();
    if (aof_thread.joinable()) aof_thread.join();
}

void Database::load_aof() {
    if (aof_filename.empty()) return;
    std::ifstream aof(aof_filename, std::ios::binary);
    if (!aof) return;

    // 一次性读入后按 RESP 逐条回放（AOF 只在启动时加载一次，没必要做流式读取）
    loading = true;  // 回放期间禁止把命令重新写回 AOF
    std::string data((std::istreambuf_iterator<char>(aof)),
                     std::istreambuf_iterator<char>());
    aof.close();

    size_t pos = 0;
    int replayed = 0;
    while (pos < data.size() && data[pos] == '*') {
        // 解析数组头 *N
        size_t le = data.find("\r\n", pos);
        if (le == std::string::npos) break;
        int argc = 0;
        try { argc = std::stoi(data.substr(pos + 1, le - pos - 1)); }
        catch (...) { break; }
        pos = le + 2;
        if (argc <= 0 || argc > 1024) break;

        // 解析 N 个批量字符串
        std::vector<std::string> args;
        args.reserve(argc);
        bool ok = true;
        for (int i = 0; i < argc; ++i) {
            if (pos >= data.size() || data[pos] != '$') { ok = false; break; }
            size_t e2 = data.find("\r\n", pos);
            if (e2 == std::string::npos) { ok = false; break; }
            int len = 0;
            try { len = std::stoi(data.substr(pos + 1, e2 - pos - 1)); }
            catch (...) { ok = false; break; }
            pos = e2 + 2;
            if (len < 0 || pos + (size_t)len + 2 > data.size()) { ok = false; break; }
            args.push_back(data.substr(pos, len));
            pos += (size_t)len + 2;
        }
        if (!ok) break;

        if (replay_command(args)) replayed++;
    }

    loading = false;
    Log::info("AOF replay: %d commands loaded", replayed);
}

// 回放一条写命令（只在启动加载 AOF 时调用）
bool Database::replay_command(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    std::string cmd = args[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);
    try {
        if (cmd == "SET" && args.size() >= 3) { set(args[1], args[2]); return true; }
        if (cmd == "INCRBY" && args.size() >= 3) { incrby(args[1], std::stoll(args[2])); return true; }
        if (cmd == "DEL" && args.size() >= 2) { del({args.begin() + 1, args.end()}); return true; }
        if (cmd == "LPUSH" && args.size() >= 2) { lpush(args[1], {args.begin() + 2, args.end()}); return true; }
        if (cmd == "RPUSH" && args.size() >= 2) { rpush(args[1], {args.begin() + 2, args.end()}); return true; }
        if (cmd == "HSET" && args.size() >= 4) { hset(args[1], args[2], args[3]); return true; }
        if (cmd == "ZADD" && args.size() >= 4) { zadd(args[1], std::stod(args[2]), args[3]); return true; }
        if (cmd == "EXPIRE" && args.size() >= 3) { expire(args[1], std::stoi(args[2])); return true; }
        if (cmd == "RENAME" && args.size() >= 3) { rename(args[1], args[2]); return true; }
        if (cmd == "FLUSHDB") { flushdb(); return true; }
    } catch (const std::exception& e) {
        Log::error("AOF replay command failed: %s", e.what());
    }
    return false;
}

// ═══════════════════════════════════════════════════════════
//  命令实现
// ═══════════════════════════════════════════════════════════

// ── String ──

std::string Database::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = store.find(key);
    if (it != store.end()) {
        delete it->second;
        store.erase(it);
    }
    RedisObject* obj = new RedisObject();
    obj->type = TYPE_STRING;
    obj->str_val = value;
    store[key] = obj;
    aof_append({ "SET", key, value });
    return "+OK\r\n";
}

std::string Database::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    _expire_key_if_needed_nolock(key);
    auto it = store.find(key);
    if (it == store.end() || it->second->type != TYPE_STRING)
        return "$-1\r\n";
    std::string& val = it->second->str_val;
    return "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
}

int Database::incr(const std::string& key) {
    return incrby(key, 1);
}

int Database::incrby(const std::string& key, int64_t delta) {
    std::lock_guard<std::mutex> lock(mtx);
    _expire_key_if_needed_nolock(key);
    auto it = store.find(key);
    if (it == store.end()) {
        // key 不存在，初始化为 0 然后加 delta
        RedisObject* obj = new RedisObject();
        obj->type = TYPE_STRING;
        obj->str_val = std::to_string(delta);
        store[key] = obj;
        // 注意：首次创建也要写 AOF，否则重启后第一次 INCR 丢失，计数会少一
        aof_append({ "INCRBY", key, std::to_string(delta) });
        return (int)delta;
    }
    if (it->second->type != TYPE_STRING) {
        throw std::runtime_error("WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    std::string& val = it->second->str_val;
    int64_t num;
    try {
        num = std::stoll(val);
    } catch (...) {
        throw std::runtime_error("ERR value is not an integer or out of range");
    }
    num += delta;
    val = std::to_string(num);
    // INCR/INCRBY/DECR/DECRBY 都会汇到这里，必须进 AOF，否则重启后库存/计数器丢失
    aof_append({ "INCRBY", key, std::to_string(delta) });
    return (int)num;
}

int Database::decr(const std::string& key) {
    return decrby(key, 1);
}

int Database::decrby(const std::string& key, int64_t delta) {
    return incrby(key, -delta);
}

int Database::del(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mtx);
    int n = _del_nolock(keys);
    if (n > 0) {
        std::vector<std::string> args;
        args.reserve(keys.size() + 1);
        args.push_back("DEL");
        args.insert(args.end(), keys.begin(), keys.end());
        aof_append(args);
    }
    return n;
}

bool Database::rename(const std::string& old_key, const std::string& new_key) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = store.find(old_key);
    if (it == store.end()) return false;
    // 如果 new_key 已存在，先删除
    auto nit = store.find(new_key);
    if (nit != store.end()) {
        delete nit->second;
        store.erase(nit);
        expires.erase(new_key);
    }
    store[new_key] = it->second;
    store.erase(it);
    // 迁移过期时间
    auto eit = expires.find(old_key);
    if (eit != expires.end()) {
        expires[new_key] = eit->second;
        expires.erase(eit);
    }
    aof_append({ "RENAME", old_key, new_key });
    return true;
}

int Database::_del_nolock(const std::vector<std::string>& keys) {
    // 调用方必须已持有 mtx
    int count = 0;
    for (auto& key : keys) {
        auto it = store.find(key);
        if (it != store.end()) {
            delete it->second;
            store.erase(it);
            expires.erase(key);
            count++;
        }
    }
    return count;
}

int Database::exists(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    _expire_key_if_needed_nolock(key);
    return store.count(key) ? 1 : 0;
}

std::vector<std::string> Database::keys(const std::string& pattern) {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> result;
    for (auto& kv : store) {
        if (!is_expired(kv.first) && match_pattern(pattern, kv.first))
            result.push_back(kv.first);
    }
    return result;
}

// ── List ──

int Database::lpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_or_create(key, TYPE_LIST);
    if (!obj) return -1;
    for (auto it = values.rbegin(); it != values.rend(); ++it)
        obj->list_val.push_front(*it);
    int len = (int)obj->list_val.size();
    // 把实际 value 写进 AOF（旧实现只记了数量，回放时会 push 空串）
    std::vector<std::string> args;
    args.reserve(values.size() + 2);
    args.push_back("LPUSH");
    args.push_back(key);
    args.insert(args.end(), values.begin(), values.end());
    aof_append(args);
    return len;
}

int Database::rpush(const std::string& key, const std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_or_create(key, TYPE_LIST);
    if (!obj) return -1;
    for (auto& v : values) obj->list_val.push_back(v);
    int len = (int)obj->list_val.size();
    std::vector<std::string> args;
    args.reserve(values.size() + 2);
    args.push_back("RPUSH");
    args.push_back(key);
    args.insert(args.end(), values.begin(), values.end());
    aof_append(args);
    return len;
}

std::string Database::lpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_LIST || obj->list_val.empty())
        return "$-1\r\n";
    std::string val = obj->list_val.front();
    obj->list_val.pop_front();
    return "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
}

std::string Database::rpop(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_LIST || obj->list_val.empty())
        return "$-1\r\n";
    std::string val = obj->list_val.back();
    obj->list_val.pop_back();
    return "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
}

std::vector<std::string> Database::lrange(const std::string& key, int start, int stop) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    std::vector<std::string> result;
    if (!obj || obj->type != TYPE_LIST) return result;

    auto& lst = obj->list_val;
    int size = (int)lst.size();
    if (start < 0) start = size + start;
    if (stop < 0) stop = size + stop;
    if (start < 0) start = 0;
    if (stop >= size) stop = size - 1;
    if (start > stop) return result;

    int idx = 0;
    for (auto& v : lst) {
        if (idx >= start && idx <= stop) result.push_back(v);
        if (idx > stop) break;
        idx++;
    }
    return result;
}

int Database::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_LIST) return 0;
    return (int)obj->list_val.size();
}

// ── Hash ──

int Database::hset(const std::string& key, const std::string& field, const std::string& value) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_or_create(key, TYPE_HASH);
    if (!obj) return -1;
    int created = obj->hash_val.count(field) ? 0 : 1;
    obj->hash_val[field] = value;
    aof_append({ "HSET", key, field, value });
    return created;
}

std::string Database::hget(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_HASH) return "$-1\r\n";
    auto it = obj->hash_val.find(field);
    if (it == obj->hash_val.end()) return "$-1\r\n";
    return "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
}

int Database::hdel(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_HASH) return 0;
    return obj->hash_val.erase(field) ? 1 : 0;
}

std::vector<std::string> Database::hgetall(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    std::vector<std::string> result;
    if (!obj || obj->type != TYPE_HASH) return result;
    for (auto& kv : obj->hash_val) {
        result.push_back(kv.first);
        result.push_back(kv.second);
    }
    return result;
}

int Database::hexists(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_HASH) return 0;
    return obj->hash_val.count(field) ? 1 : 0;
}

// ── Set ──

int Database::sadd(const std::string& key, const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_or_create(key, TYPE_SET);
    if (!obj) return -1;
    int added = 0;
    for (auto& m : members) {
        if (obj->set_val.insert(m).second) added++;
    }
    return added;
}

int Database::srem(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_SET) return 0;
    return obj->set_val.erase(member) ? 1 : 0;
}

std::vector<std::string> Database::smembers(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    std::vector<std::string> result;
    if (!obj || obj->type != TYPE_SET) return result;
    for (auto& m : obj->set_val) result.push_back(m);
    return result;
}

int Database::sismember(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_SET) return 0;
    return obj->set_val.count(member) ? 1 : 0;
}

int Database::scard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_SET) return 0;
    return (int)obj->set_val.size();
}

// ── Sorted Set ──

int Database::zadd(const std::string& key, double score, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_or_create(key, TYPE_ZSET);
    if (!obj) return -1;
    if (!obj->zset_val) obj->zset_val = new SkipList();
    size_t old_count = obj->zset_val->size();
    obj->zset_val->insert(member, score);
    aof_append({ "ZADD", key, std::to_string(score), member });
    return (obj->zset_val->size() > old_count) ? 1 : 0;
}

int Database::zrem(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_ZSET || !obj->zset_val) return 0;
    return obj->zset_val->remove(member) ? 1 : 0;
}

std::string Database::zscore(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_ZSET || !obj->zset_val) return "$-1\r\n";
    double score = obj->zset_val->get_score(member);
    std::string s = std::to_string(score);
    // 去掉尾部多余的 0
    s.erase(s.find_last_not_of('0') + 1, std::string::npos);
    if (s.back() == '.') s.pop_back();
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::vector<std::string> Database::zrange(const std::string& key, int start, int stop, bool withscores) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    std::vector<std::string> result;
    if (!obj || obj->type != TYPE_ZSET || !obj->zset_val) return result;

    auto items = obj->zset_val->range_with_scores(start, stop);
    for (auto& kv : items) {
        result.push_back(kv.first);
        if (withscores) {
            std::string s = std::to_string(kv.second);
            s.erase(s.find_last_not_of('0') + 1, std::string::npos);
            if (s.back() == '.') s.pop_back();
            result.push_back(s);
        }
    }
    return result;
}

int Database::zrank(const std::string& key, const std::string& member) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_ZSET || !obj->zset_val) return -1;
    return obj->zset_val->get_rank(member);
}

int Database::zcard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    RedisObject* obj = get_value(key);
    if (!obj || obj->type != TYPE_ZSET || !obj->zset_val) return 0;
    return (int)obj->zset_val->size();
}

// ── 过期 ──

int Database::expire(const std::string& key, int seconds) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!store.count(key)) return 0;
    expires[key] = time(nullptr) + seconds;
    aof_append({ "EXPIRE", key, std::to_string(seconds) });
    return 1;
}

int Database::ttl(const std::string& key) {
    std::lock_guard<std::mutex> lock(mtx);
    if (!store.count(key)) return -2; // key 不存在
    auto it = expires.find(key);
    if (it == expires.end()) return -1; // 永不过期
    time_t remain = it->second - time(nullptr);
    return remain < 0 ? -2 : (int)remain;
}

// ── 管理 ──

int Database::dbsize() const {
    std::lock_guard<std::mutex> lock(mtx);
    return (int)store.size();
}

std::string Database::flushdb() {
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& kv : store) delete kv.second;
    store.clear();
    expires.clear();
    aof_append({ "FLUSHDB" });
    return "+OK\r\n";
}

std::string Database::info() {
    std::lock_guard<std::mutex> lock(mtx);
    std::ostringstream ss;
    ss << "# Server\r\n";
    ss << "mini_redis_version:1.0.0\r\n";
    ss << "# Keyspace\r\n";
    ss << "db0:keys=" << store.size() << ",expires=" << expires.size() << "\r\n";
    std::string s = ss.str();
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

// ── 运维统计 ──

void Database::record_command(const std::string& cmd, const std::string& key) {
    if (start_time_ == 0) start_time_ = time(nullptr);
    time_t now = time(nullptr);

    // 独立的统计锁：与主存储锁 mtx 隔离，record_command 不会拖慢 INCR/DECR。
    {
        std::lock_guard<std::mutex> lock(stats_mtx);
        cmd_counts_[cmd]++;
        total_commands_++;
        if (!key.empty()) key_hits_[key]++;
        // QPS 滑动窗口：按"秒"分桶，切换秒时把上一秒的计数推入窗口并丢弃 >5 秒的旧桶。
        if (now != window_sec_) {
            if (window_sec_ != 0)
                qps_window_.push_back({window_sec_, window_cur_});
            window_sec_ = now;
            window_cur_ = 1;
            while ((int)qps_window_.size() > 5)
                qps_window_.pop_front();
        } else {
            window_cur_++;
        }
    }
}

void Database::add_connection() {
    ++connections_;
}

void Database::dec_connection() {
    int v = --connections_;
    (void)v;
}

std::string Database::stats(int topn) {
    std::lock_guard<std::mutex> lock(stats_mtx);
    if (start_time_ == 0) start_time_ = time(nullptr);

    time_t now = time(nullptr);
    double uptime = std::max((double)(now - start_time_), 1.0);

    // 最近 5 秒命令数 → 估算 QPS
    uint64_t window_total = window_cur_;
    for (auto& p : qps_window_) window_total += p.second;
    int window_secs = (int)qps_window_.size() + 1;
    double qps = window_total / (double)window_secs;

    // 命令分布（RedisObject 之外，便于 Agent 看热点操作类型）
    typedef std::pair<std::string, uint64_t> CountPair;
    std::vector<CountPair> cmd_sorted(cmd_counts_.begin(), cmd_counts_.end());
    std::sort(cmd_sorted.begin(), cmd_sorted.end(),
              [](const CountPair& a, const CountPair& b) { return a.second > b.second; });

    // 热点 key TopN
    std::vector<CountPair> hot(key_hits_.begin(), key_hits_.end());
    std::sort(hot.begin(), hot.end(),
              [](const CountPair& a, const CountPair& b) { return a.second > b.second; });
    if (topn > 0 && (int)hot.size() > topn) hot.resize(topn);

    // 手工拼 JSON，避免引入第三方 JSON 库
    std::ostringstream ss;
    ss << "{";
    ss << "\"uptime_sec\":" << (int64_t)uptime << ",";
    ss << "\"total_commands\":" << total_commands_ << ",";
    ss << "\"connections\":" << connections_.load() << ",";
    ss << "\"qps_estimate\":" << std::fixed << std::setprecision(2) << qps << ",";
    ss << "\"qps_window_secs\":" << window_secs << ",";
    ss << "\"cmd_counts\":{";
    for (size_t i = 0; i < cmd_sorted.size(); ++i) {
        if (i) ss << ",";
        ss << "\"" << cmd_sorted[i].first << "\":" << cmd_sorted[i].second;
    }
    ss << "},";
    ss << "\"hot_keys\":[";
    for (size_t i = 0; i < hot.size(); ++i) {
        if (i) ss << ",";
        ss << "{\"key\":" << json_escape(hot[i].first) << ",\"hits\":" << hot[i].second << "}";
    }
    ss << "]";
    ss << "}";

    std::string s = ss.str();
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
