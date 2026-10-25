#include "command.h"
#include "db.h"
#include "log.h"
#include <algorithm>
#include <sstream>
#include <cctype>

// 大小写不敏感字符串比较
static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    }
    return true;
}

CommandTable::CommandTable(Database* db) : db(db) {
    register_commands();
}

// ── RESP 响应构造 ──

std::string CommandTable::ok_resp() { return "+OK\r\n"; }
std::string CommandTable::err_resp(const std::string& msg) { return "-ERR " + msg + "\r\n"; }
std::string CommandTable::int_resp(int val) { return ":" + std::to_string(val) + "\r\n"; }
std::string CommandTable::null_bulk_resp() { return "$-1\r\n"; }

std::string CommandTable::bulk_resp(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

std::string CommandTable::array_resp(const std::vector<std::string>& items) {
    std::ostringstream ss;
    ss << "*" << items.size() << "\r\n";
    for (auto& item : items) {
        ss << "$" << item.size() << "\r\n" << item << "\r\n";
    }
    return ss.str();
}

std::string CommandTable::ping_resp(const std::string& msg) {
    if (msg.empty()) return "+PONG\r\n";
    return "$" + std::to_string(msg.size()) + "\r\n" + msg + "\r\n";
}

// ── 命令注册 ──

void CommandTable::register_commands() {
    // ── PING ──
    handlers["PING"] = [this](const std::vector<std::string>& args) {
        return ping_resp(args.empty() ? "" : args[0]);
    };

    // ── String ──
    handlers["SET"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for SET");
        return db->set(args[0], args[1]);
    };

    handlers["GET"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for GET");
        return db->get(args[0]);
    };

    handlers["INCR"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for INCR");
        int result = db->incr(args[0]);
        return int_resp(result);
    };

    handlers["INCRBY"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for INCRBY");
        int result = db->incrby(args[0], std::stoll(args[1]));
        return int_resp(result);
    };

    handlers["DECR"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for DECR");
        int result = db->decr(args[0]);
        return int_resp(result);
    };

    handlers["DECRBY"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for DECRBY");
        int result = db->decrby(args[0], std::stoll(args[1]));
        return int_resp(result);
    };

    handlers["DEL"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for DEL");
        return int_resp(db->del(args));
    };

    handlers["EXISTS"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for EXISTS");
        return int_resp(db->exists(args[0]));
    };

    handlers["RENAME"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for RENAME");
        return db->rename(args[0], args[1]) ? ok_resp()
            : err_resp("no such key");
    };

    handlers["KEYS"] = [this](const std::vector<std::string>& args) {
        auto keys = db->keys(args.empty() ? "*" : args[0]);
        return array_resp(keys);
    };

    // ── List ──
    handlers["LPUSH"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for LPUSH");
        std::vector<std::string> vals(args.begin() + 1, args.end());
        return int_resp(db->lpush(args[0], vals));
    };

    handlers["RPUSH"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for RPUSH");
        std::vector<std::string> vals(args.begin() + 1, args.end());
        return int_resp(db->rpush(args[0], vals));
    };

    handlers["LPOP"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for LPOP");
        return db->lpop(args[0]);
    };

    handlers["RPOP"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for RPOP");
        return db->rpop(args[0]);
    };

    handlers["LRANGE"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 3) return err_resp("wrong number of arguments for LRANGE");
        auto range = db->lrange(args[0], std::stoi(args[1]), std::stoi(args[2]));
        return array_resp(range);
    };

    handlers["LLEN"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for LLEN");
        return int_resp(db->llen(args[0]));
    };

    // ── Hash ──
    handlers["HSET"] = [this](const std::vector<std::string>& args) {
        // HSET key field1 value1 [field2 value2 ...]
        if (args.size() < 3 || (args.size() - 1) % 2 != 0)
            return err_resp("wrong number of arguments for HSET");
        int created = 0;
        for (size_t i = 1; i + 1 < args.size(); i += 2)
            created += db->hset(args[0], args[i], args[i + 1]);
        return int_resp(created);
    };

    handlers["HGET"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for HGET");
        return db->hget(args[0], args[1]);
    };

    handlers["HDEL"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for HDEL");
        return int_resp(db->hdel(args[0], args[1]));
    };

    handlers["HGETALL"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for HGETALL");
        return array_resp(db->hgetall(args[0]));
    };

    handlers["HEXISTS"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for HEXISTS");
        return int_resp(db->hexists(args[0], args[1]));
    };

    // ── Set ──
    handlers["SADD"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for SADD");
        std::vector<std::string> members(args.begin() + 1, args.end());
        return int_resp(db->sadd(args[0], members));
    };

    handlers["SREM"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for SREM");
        return int_resp(db->srem(args[0], args[1]));
    };

    handlers["SMEMBERS"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for SMEMBERS");
        return array_resp(db->smembers(args[0]));
    };

    handlers["SISMEMBER"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for SISMEMBER");
        return int_resp(db->sismember(args[0], args[1]));
    };

    handlers["SCARD"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for SCARD");
        return int_resp(db->scard(args[0]));
    };

    // ── Sorted Set ──
    handlers["ZADD"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 3) return err_resp("wrong number of arguments for ZADD");
        return int_resp(db->zadd(args[0], std::stod(args[1]), args[2]));
    };

    handlers["ZREM"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for ZREM");
        return int_resp(db->zrem(args[0], args[1]));
    };

    handlers["ZSCORE"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for ZSCORE");
        return db->zscore(args[0], args[1]);
    };

    handlers["ZRANGE"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 3) return err_resp("wrong number of arguments for ZRANGE");
        bool withscores = (args.size() >= 4 && args[3] == "WITHSCORES");
        auto range = db->zrange(args[0], std::stoi(args[1]), std::stoi(args[2]), withscores);
        return array_resp(range);
    };

    handlers["ZRANK"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for ZRANK");
        int rank = db->zrank(args[0], args[1]);
        return rank >= 0 ? int_resp(rank) : null_bulk_resp();
    };

    handlers["ZCARD"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for ZCARD");
        return int_resp(db->zcard(args[0]));
    };

    // ── 过期 ──
    handlers["EXPIRE"] = [this](const std::vector<std::string>& args) {
        if (args.size() < 2) return err_resp("wrong number of arguments for EXPIRE");
        return int_resp(db->expire(args[0], std::stoi(args[1])));
    };

    handlers["TTL"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for TTL");
        return int_resp(db->ttl(args[0]));
    };

    // ── 管理 ──
    handlers["DBSIZE"] = [this](const std::vector<std::string>&) {
        return int_resp(db->dbsize());
    };

    handlers["FLUSHDB"] = [this](const std::vector<std::string>&) {
        return db->flushdb();
    };

    handlers["INFO"] = [this](const std::vector<std::string>&) {
        return db->info();
    };

    handlers["COMMAND"] = [this](const std::vector<std::string>&) {
        return ok_resp();
    };

    // ── STATS（库存引擎智能运维 Agent 的数据来源，只读）──
    // 用法：STATS        -> 返回热点 key Top10 + 命令分布 + QPS 估算
    //       STATS 5      -> 返回热点 key Top5
    handlers["STATS"] = [this](const std::vector<std::string>& args) {
        int topn = 10;
        if (!args.empty()) {
            try { topn = std::stoi(args[0]); } catch (...) { topn = 10; }
        }
        return db->stats(topn);
    };

    // ── SELECT / AUTH (兼容) ──
    handlers["SELECT"] = [this](const std::vector<std::string>&) {
        return ok_resp();
    };

    handlers["AUTH"] = [this](const std::vector<std::string>&) {
        return ok_resp();
    };

    // ── CONFIG (redis-benchmark 兼容) ──
    handlers["CONFIG"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for CONFIG");
        std::string sub = args[0];
        std::transform(sub.begin(), sub.end(), sub.begin(), ::toupper);
        if (sub == "GET" && args.size() >= 2) {
            // CONFIG GET <key> — 返回键值对数组
            std::string key = args[1];
            std::string value;
            // redis-benchmark 会查询这两个配置
            if (iequals(key, "save")) {
                value = "";  // 空字符串表示默认配置
            } else if (iequals(key, "appendonly")) {
                value = "no";
            } else if (iequals(key, "dir")) {
                value = ".";
            } else if (iequals(key, "databases")) {
                value = "1";
            } else {
                value = "";  // 未知配置返回空
            }
            std::vector<std::string> resp = {key, value};
            return array_resp(resp);
        }
        return err_resp("unsupported CONFIG subcommand");
    };

    // ── CLIENT (redis-benchmark 兼容) ──
    handlers["CLIENT"] = [this](const std::vector<std::string>& args) {
        if (args.empty()) return err_resp("wrong number of arguments for CLIENT");
        std::string sub = args[0];
        std::transform(sub.begin(), sub.end(), sub.begin(), ::toupper);
        if (sub == "SETNAME") {
            // CLIENT SETNAME connection-name — 记录客户端名称（丢弃即可）
            return ok_resp();
        } else if (sub == "GETNAME") {
            // CLIENT GETNAME — 返回客户端名称
            return null_bulk_resp();
        }
        return err_resp("unsupported CLIENT subcommand");
    };

    // ── HELLO (Redis 6+ 兼容) ──
    handlers["HELLO"] = [this](const std::vector<std::string>&) {
        // 返回一个简单的 map 回复，声明 RESP 版本和服务器信息
        // 格式: %2\r\n$6\r\nserver\r\n$9\r\nmini_redis\r\n$7\r\nversion\r\n$5\r\n1.0.0\r\n
        // 简化: 返回一个数组 [server, mini_redis, version, 1.0.0]
        std::vector<std::string> info = {
            "server", "mini_redis",
            "version", "1.0.0",
            "proto", "2"
        };
        return array_resp(info);
    };
}

// ── 命令执行入口 ──

std::string CommandTable::execute(const std::vector<std::string>& args) {
    if (args.empty()) return err_resp("empty command");

    std::string cmd = args[0];
    // 转为大写
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

    auto it = handlers.find(cmd);
    if (it == handlers.end()) {
        return err_resp("unknown command '" + args[0] + "'");
    }

    // 第一个参数是命令名，剩余是参数
    std::vector<std::string> cmd_args(args.begin() + 1, args.end());

    // 运维统计：记录命令类型与热点 key。只读、独立锁，绝不影响主路径正确性。
    // 即便统计逻辑异常也用 try 包住，确保不会让正常命令失败。
    try {
        db->record_command(cmd, cmd_args.empty() ? "" : cmd_args[0]);
    } catch (...) {}

    try {
        return it->second(cmd_args);
    } catch (const std::exception& e) {
        Log::error("Command %s failed: %s", cmd.c_str(), e.what());
        return err_resp(std::string("execution error: ") + e.what());
    }
}
