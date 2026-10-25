#ifndef RESP_CODEC_H
#define RESP_CODEC_H

// ═══════════════════════════════════════════════════════════════
//  RESP 协议编解码器（客户端侧）
//
//  借鉴 mini_redis/src/resp_parser.cpp 的解析技法:
//    · find("\r\n") 定位 RESP 行尾
//    · stoi() 解析 bulk string 长度字段
//    · 循环消费缓冲区 + 不完整就返回等待的异步模式
//
//  职责:
//    resp_encode() → 命令数组编码为 RESP 协议字节流
//    resp_decode() → 服务端 RESP 响应解码为 RespValue 结构
// ═══════════════════════════════════════════════════════════════

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>

// ── RESP 值类型（解析后的统一表示）──
struct RespValue {
    enum Type { STRING, ERROR, INTEGER, NIL, ARRAY };

    Type type = NIL;
    std::string str_val;
    int64_t int_val = 0;
    std::vector<RespValue> array_val;

    // 便捷判断
    bool is_ok()    const { return type == STRING && str_val == "OK"; }
    bool is_error() const { return type == ERROR; }
    bool is_nil()   const { return type == NIL; }

    // 转为可打印字符串（调试用）
    std::string to_string() const {
        switch (type) {
        case STRING:  return str_val;
        case ERROR:   return "(error) " + str_val;
        case INTEGER: return "(integer) " + std::to_string(int_val);
        case NIL:     return "(nil)";
        case ARRAY: {
            std::ostringstream ss;
            ss << "[";
            for (size_t i = 0; i < array_val.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << array_val[i].to_string();
            }
            ss << "]";
            return ss.str();
        }
        }
        return "?";
    }
};

// ── RESP 解码结果 ──
struct RespDecodeResult {
    RespValue value;
    size_t consumed = 0;    // 消耗的字节数
    bool complete = false;  // 数据是否完整
    std::string error;      // 错误信息（complete=false 时为空）
};

// ═══════════════════════════════════════════════════════════════
//  RESP 编码: 命令数组 → RESP 协议字节流
// ═══════════════════════════════════════════════════════════════

inline std::string resp_encode(const std::vector<std::string>& args) {
    std::ostringstream ss;
    ss << "*" << args.size() << "\r\n";
    for (const auto& arg : args) {
        ss << "$" << arg.size() << "\r\n";
        ss << arg << "\r\n";
    }
    return ss.str();
}

// ═══════════════════════════════════════════════════════════════
//  RESP 解码: RESP 字节流 → RespValue
//
//  支持服务端响应的 5 种类型:
//    +  Simple String:  +OK\r\n
//    -  Error:          -ERR message\r\n
//    :  Integer:        :42\r\n
//    $  Bulk String:    $5\r\nhello\r\n  或  $-1\r\n (null)
//    *  Array:          *2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
//
//  借鉴 mini_redis RespParser::try_parse() 的设计:
//    不完整数据 → complete=false（调用方保留缓冲区，等下次数据）
//    完整数据   → complete=true，consumed 标明消费的字节数
// ═══════════════════════════════════════════════════════════════

inline RespDecodeResult resp_decode(const char* data, size_t len) {
    RespDecodeResult result;
    result.complete = false;
    if (len == 0) return result;

    char type = data[0];

    // ── Simple String / Error ──
    if (type == '+' || type == '-') {
        for (size_t i = 1; i + 1 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                result.value.type = (type == '+') ? RespValue::STRING
                                                   : RespValue::ERROR;
                result.value.str_val = std::string(data + 1, i - 1);
                result.consumed = i + 2;
                result.complete = true;
                return result;
            }
        }
        return result; // 数据不完整
    }

    // ── Integer ──
    if (type == ':') {
        for (size_t i = 1; i + 1 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                result.value.type = RespValue::INTEGER;
                result.value.int_val = std::stoll(
                    std::string(data + 1, i - 1));
                result.consumed = i + 2;
                result.complete = true;
                return result;
            }
        }
        return result;
    }

    // ── Bulk String ──
    if (type == '$') {
        // 找到 $长度 行的 \r\n
        size_t crlf1 = 0;
        for (size_t i = 1; i + 1 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                crlf1 = i;
                break;
            }
        }
        if (crlf1 == 0) return result;

        int64_t bulk_len = std::stoll(std::string(data + 1, crlf1 - 1));

        // NULL bulk string: $-1\r\n
        if (bulk_len == -1) {
            result.value.type = RespValue::NIL;
            result.consumed = crlf1 + 2;
            result.complete = true;
            return result;
        }

        // 检查数据是否足够
        size_t data_start = crlf1 + 2;
        if (len - data_start < (size_t)bulk_len + 2) {
            return result; // 数据不完整，等下次（借鉴 mini_redis 的 PARSE_AGAIN 模式）
        }

        result.value.type = RespValue::STRING;
        result.value.str_val = std::string(data + data_start, bulk_len);
        result.consumed = data_start + bulk_len + 2;
        result.complete = true;
        return result;
    }

    // ── Array ──
    if (type == '*') {
        size_t crlf1 = 0;
        for (size_t i = 1; i + 1 < len; ++i) {
            if (data[i] == '\r' && data[i + 1] == '\n') {
                crlf1 = i;
                break;
            }
        }
        if (crlf1 == 0) return result;

        int64_t array_len = std::stoll(std::string(data + 1, crlf1 - 1));

        // 空数组或 NULL 数组
        if (array_len <= 0) {
            result.value.type = RespValue::ARRAY;
            result.consumed = crlf1 + 2;
            result.complete = true;
            return result;
        }

        // 递归解析每个元素（借鉴 mini_redis 循环解析数组元素的模式）
        result.value.type = RespValue::ARRAY;
        size_t offset = crlf1 + 2;
        for (int64_t i = 0; i < array_len; ++i) {
            auto item = resp_decode(data + offset, len - offset);
            if (!item.complete) {
                // 子元素数据不完整，整体不完整
                return result;
            }
            if (!item.error.empty()) {
                result.error = item.error;
                return result;
            }
            result.value.array_val.push_back(item.value);
            offset += item.consumed;
        }
        result.consumed = offset;
        result.complete = true;
        return result;
    }

    // ── 未知类型 ──
    result.error = "Unknown RESP type byte: " + std::string(1, type);
    return result;
}

#endif // RESP_CODEC_H
