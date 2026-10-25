#ifndef EPOLL_H
#define EPOLL_H

#include <vector>
#include <sys/epoll.h>
#include <unordered_map>
#include <set>
#include <mutex>
#include <string>

class RespParser;

// 每个连接的完整状态
struct ConnState {
    RespParser* parser;
    std::string out_buf;   // 输出缓冲区（未发送完的响应数据）
    size_t out_offset;     // 输出缓冲区已发送偏移
    bool closing;          // 标记连接待关闭（写完剩余数据后关闭）

    ConnState() : parser(nullptr), out_offset(0), closing(false) {}
};

class Epoll {
public:
    Epoll();
    ~Epoll();
    int add(int fd, uint32_t events);
    int mod(int fd, uint32_t events);
    int del(int fd);
    int wait(int timeout = -1);
    void handle_events(int listen_fd, int nfds);

    // 连接状态管理
    std::unordered_map<int, ConnState*> conns;
    std::mutex mtx;  // 保护 conns 和 timed_out_fds
    void mark_timed_out(int fd);
    bool is_timed_out(int fd);
    void remove_timed_out(int fd);

private:
    int epoll_fd;
    std::vector<struct epoll_event> events;
    std::set<int> timed_out_fds;
};

#endif // EPOLL_H
