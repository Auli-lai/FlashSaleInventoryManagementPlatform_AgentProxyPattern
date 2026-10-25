#ifndef RESP_PARSER_H
#define RESP_PARSER_H

#include <string>
#include <vector>
#include <cstddef>

class RespParser {
public:
    enum ParseResult {
        PARSE_OK,       // 一条完整命令解析完成
        PARSE_AGAIN,    // 数据不完整，需要继续读
        PARSE_ERROR     // 协议错误
    };

    RespParser(int fd);
    void reset();

    // 喂数据（仅追加到内部缓冲区，不做解析）
    void feed(const char* data, size_t len);

    // 尝试从缓冲区解析一条完整命令
    // 成功返回 PARSE_OK，命令可通过 get_command() 获取
    // 数据不足返回 PARSE_AGAIN
    // 协议错误返回 PARSE_ERROR
    ParseResult try_parse();

    // 移除已解析完成的那条命令的数据（保留后续管道化命令）
    void consume_command();

    // 获取解析后的命令: 第一个元素是命令名(大写)，后续是参数
    const std::vector<std::string>& get_command() const { return command; }
    const std::string& get_error() const { return error_msg; }

private:
    int fd;

    // 输入缓冲区 (处理半包和管道化)
    std::string buffer;

    // 解析出来的命令
    std::vector<std::string> command;

    // 错误信息
    std::string error_msg;

    // 内部解析状态
    enum State {
        S_WAIT_TYPE,     // 等待 * $ : + -
        S_ARRAY_COUNT,   // 读取数组元素个数
        S_ARRAY_ITEM,    // 读取数组中的下一个元素（$长度行）
        S_BULK_DATA,     // 读取批量字符串数据
        S_SIMPLE,        // 内联命令（空格分隔）
    };

    State state;
    int array_count;     // 数组剩余元素数
    int bulk_len;        // 当前批量字符串预期长度

    // 解析过程中使用的临时索引（指向 buffer 中的位置）
    size_t parse_pos;
    size_t cmd_start;    // 当前命令在 buffer 中的起始偏移

    // 大小写不敏感字符串比较
    static bool iequals(const std::string& a, const std::string& b);
};

#endif // RESP_PARSER_H
