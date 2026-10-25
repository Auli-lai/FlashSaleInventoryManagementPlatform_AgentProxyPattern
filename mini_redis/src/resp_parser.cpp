#include "resp_parser.h"
#include "log.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>

RespParser::RespParser(int fd)
    : fd(fd), state(S_WAIT_TYPE), array_count(0),
      bulk_len(0), parse_pos(0), cmd_start(0) {}

void RespParser::reset() {
    buffer.clear();
    command.clear();
    error_msg.clear();
    state = S_WAIT_TYPE;
    array_count = 0;
    bulk_len = 0;
    parse_pos = 0;
    cmd_start = 0;
}

// ── 仅追加数据，不做解析 ──
void RespParser::feed(const char* data, size_t len) {
    buffer.append(data, len);
}

// ── 移除已解析命令，保留后续管道化数据 ──
void RespParser::consume_command() {
    // 移除从 cmd_start 到 parse_pos 的已解析数据
    if (parse_pos > cmd_start && parse_pos <= buffer.size()) {
        buffer.erase(0, parse_pos);
    }
    // 重置解析状态，准备解析下一条命令
    command.clear();
    state = S_WAIT_TYPE;
    array_count = 0;
    bulk_len = 0;
    parse_pos = 0;
    cmd_start = 0;
}

// ── 大小写不敏感比较 ──
bool RespParser::iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(a[i]) != tolower(b[i])) return false;
    }
    return true;
}

// ── 尝试从缓冲区解析一条命令 ──
RespParser::ParseResult RespParser::try_parse() {
    if (buffer.empty()) return PARSE_AGAIN;

    // 确定顶层类型（仅在新命令开始时）
    if (state == S_WAIT_TYPE && command.empty()) {
        cmd_start = 0;
        parse_pos = 0;
        if (buffer[0] == '*') {
            state = S_ARRAY_COUNT;
            // parse_pos = 0，从 '*' 开始（\r\n 查找会包含完整行）
        } else {
            state = S_SIMPLE;
        }
    }

    // 逐状态驱动解析
    while (parse_pos < buffer.size()) {
        // 对于 BULK_DATA 状态，不需要等待 \r\n
        if (state == S_BULK_DATA) {
            if (buffer.size() - parse_pos < (size_t)bulk_len + 2) {
                return PARSE_AGAIN;
            }
            command.push_back(buffer.substr(parse_pos, bulk_len));
            parse_pos += bulk_len + 2; // 跳过 data + \r\n
            if (--array_count > 0) {
                state = S_ARRAY_ITEM;
            } else {
                return PARSE_OK;
            }
            continue;
        }

        // 其他状态：查找 \r\n
        size_t crlf = buffer.find("\r\n", parse_pos);
        if (crlf == std::string::npos) {
            return PARSE_AGAIN;
        }

        std::string line = buffer.substr(parse_pos, crlf - parse_pos);
        parse_pos = crlf + 2;

        switch (state) {
        case S_ARRAY_COUNT: {
            if (line.empty() || line[0] != '*') {
                // 回退：作为内联命令处理
                state = S_SIMPLE;
                parse_pos = 0;
                command.clear();
                continue;
            }
            array_count = std::stoi(line.substr(1));
            if (array_count <= 0) {
                // 空数组，如 *0\r\n
                return PARSE_OK;
            }
            state = S_ARRAY_ITEM;
            break;
        }
        case S_ARRAY_ITEM: {
            if (line.empty() || line[0] != '$') {
                error_msg = "Expected bulk string ($) but got: " +
                            (line.empty() ? "<empty>" : line.substr(0, 20));
                return PARSE_ERROR;
            }
            bulk_len = std::stoi(line.substr(1));
            if (bulk_len == -1) {
                // NULL bulk string: 空值
                command.push_back("");
                if (--array_count > 0) {
                    state = S_ARRAY_ITEM;
                } else {
                    return PARSE_OK;
                }
                break;
            }
            if (bulk_len < 0 || bulk_len > 512 * 1024 * 1024) {
                error_msg = "Invalid bulk length: " + std::to_string(bulk_len);
                return PARSE_ERROR;
            }
            state = S_BULK_DATA;
            break;
        }
        case S_SIMPLE: {
            // 内联命令: 按空格分割参数
            std::string cmd_line = line;
            // 去掉行尾可能残留的 \r
            if (!cmd_line.empty() && cmd_line.back() == '\r')
                cmd_line.pop_back();

            size_t pos = 0;
            while (pos < cmd_line.size()) {
                while (pos < cmd_line.size() && cmd_line[pos] == ' ') pos++;
                if (pos >= cmd_line.size()) break;
                size_t end = cmd_line.find(' ', pos);
                if (end == std::string::npos) end = cmd_line.size();
                command.push_back(cmd_line.substr(pos, end - pos));
                pos = end;
            }
            return PARSE_OK;
        }
        default:
            error_msg = "Unknown parser state";
            return PARSE_ERROR;
        }
    }

    return PARSE_AGAIN;
}
