#include "epoll.h"
#include "resp_parser.h"
#include "timer.h"
#include "log.h"
#include "command.h"
#include "db.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>

extern Database g_db;
extern CommandTable g_cmd;

Epoll::Epoll() {
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) Log::error("Failed to create epoll");
    events.resize(1024);
}

Epoll::~Epoll() {
    if (epoll_fd != -1) close(epoll_fd);
    for (auto& p : conns) {
        delete p.second->parser;
        delete p.second;
    }
}

int Epoll::add(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    return epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
}

int Epoll::mod(int fd, uint32_t events) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events;
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
}

int Epoll::del(int fd) {
    return epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
}

int Epoll::wait(int timeout) {
    return epoll_wait(epoll_fd, events.data(), events.size(), timeout);
}

void Epoll::mark_timed_out(int fd) {
    std::lock_guard<std::mutex> lock(mtx);
    timed_out_fds.insert(fd);
}

bool Epoll::is_timed_out(int fd) {
    std::lock_guard<std::mutex> lock(mtx);
    return timed_out_fds.find(fd) != timed_out_fds.end();
}

void Epoll::remove_timed_out(int fd) {
    std::lock_guard<std::mutex> lock(mtx);
    timed_out_fds.erase(fd);
}

// ── 工具函数 ──

static void close_connection(int fd, Epoll* epoll) {
    epoll->del(fd);
    close(fd);
    Timer::remove(fd);
    epoll->remove_timed_out(fd);
    g_db.dec_connection();   // 运维统计：当前连接数 -1
    {
        std::lock_guard<std::mutex> lock(epoll->mtx);
        auto it = epoll->conns.find(fd);
        if (it != epoll->conns.end()) {
            delete it->second->parser;
            delete it->second;
            epoll->conns.erase(it);
        }
    }
}

// ── 尝试发送输出缓冲区中的数据（非阻塞）──
// 返回值: true = 全部发送完成, false = 需要等待 EPOLLOUT
static bool flush_out_buf(int fd, ConnState* conn) {
    if (conn->out_buf.empty() || conn->out_offset >= conn->out_buf.size())
        return true;  // 没有待发送的数据

    while (conn->out_offset < conn->out_buf.size()) {
        ssize_t written = write(fd,
            conn->out_buf.c_str() + conn->out_offset,
            conn->out_buf.size() - conn->out_offset);
        if (written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;  // 发送缓冲区满，需要等待 EPOLLOUT
            }
            // 真正的写错误
            Log::error("Write failed (fd=%d): %s", fd, strerror(errno));
            return false;  // 调用方负责关闭连接
        }
        conn->out_offset += written;
    }

    // 全部发送完成，清空缓冲区
    conn->out_buf.clear();
    conn->out_offset = 0;
    return true;
}

// ── 将响应加入输出缓冲区并尝试发送 ──
// 返回值: true = 全部发送完成, false = 需要等待 EPOLLOUT
static bool send_response(int fd, Epoll* epoll, ConnState* conn,
                          const std::string& response) {
    // 如果之前有未发完的数据，追加到缓冲区
    if (!conn->out_buf.empty()) {
        conn->out_buf.append(response);
        return flush_out_buf(fd, conn);
    }

    // 尝试直接发送（避免缓冲区拷贝）
    size_t offset = 0;
    while (offset < response.size()) {
        ssize_t written = write(fd, response.c_str() + offset,
                                response.size() - offset);
        if (written == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 发送缓冲区满，将剩余数据放入缓冲区
                conn->out_buf = response.substr(offset);
                conn->out_offset = 0;
                // 注册 EPOLLOUT 等待可写
                epoll->mod(fd, EPOLLIN | EPOLLOUT | EPOLLET);
                return false;
            }
            Log::error("Write failed (fd=%d): %s", fd, strerror(errno));
            return false;
        }
        offset += written;
    }
    return true;
}

// ── 获取或创建连接的 ConnState ──
static ConnState* get_conn(int fd, Epoll* epoll) {
    std::lock_guard<std::mutex> lock(epoll->mtx);
    auto it = epoll->conns.find(fd);
    if (it == epoll->conns.end()) {
        ConnState* conn = new ConnState();
        conn->parser = new RespParser(fd);
        epoll->conns[fd] = conn;
        return conn;
    }
    return it->second;
}

// ── 处理单个客户端 fd 的可读事件 ──
static void process_client_read(int fd, Epoll* epoll, Database& /*db*/,
                                 CommandTable& cmd) {
    if (epoll->is_timed_out(fd)) {
        close_connection(fd, epoll);
        return;
    }

    ConnState* conn = get_conn(fd, epoll);
    RespParser* parser = conn->parser;

    // 如果之前正在等待 EPOLLOUT 完成发送，现在恢复为纯读模式

    // ── 1. 循环读取直到 EAGAIN（ET 模式必须排空 socket buffer）──
    char buf[4096];
    bool read_any = false;
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            read_any = true;
            parser->feed(buf, n);
        } else if (n == 0) {
            // 对端关闭连接
            close_connection(fd, epoll);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            Log::error("Read failed (fd=%d): %s", fd, strerror(errno));
            close_connection(fd, epoll);
            return;
        }
    }

    if (!read_any) return;  // 理论上不会发生（EPOLLIN 触发但没数据）

    Timer::refresh(fd, 60);

    // ── 2. 循环处理所有已解析完成的命令（支持管道化）──
    bool need_flush = false;
    while (true) {
        RespParser::ParseResult res = parser->try_parse();
        if (res == RespParser::PARSE_OK) {
            std::string response = cmd.execute(parser->get_command());
            parser->consume_command();  // 只移除已解析的命令，保留剩余数据

            // 发送响应
            if (!send_response(fd, epoll, conn, response)) {
                // 写阻塞：剩余数据已缓冲，等待 EPOLLOUT
                // 已在 send_response 中注册 EPOLLOUT
                need_flush = true;
                break;
            }
        } else if (res == RespParser::PARSE_AGAIN) {
            break;  // 数据不完整，等待下次 EPOLLIN
        } else {
            // 协议错误
            Log::error("RESP parse error (fd=%d): %s", fd,
                       parser->get_error().c_str());
            std::string err = "-ERR protocol error\r\n";
            send_response(fd, epoll, conn, err);
            // 标记关闭：让 EPOLLOUT/下次事件中关闭
            conn->closing = true;
            need_flush = true;
            break;
        }
    }

    // ── 3. 事件状态管理（无需重新注册，ET 模式下 fd 始终在 epoll 集合中）──
    if (need_flush) {
        // 有未发送完的数据，EPOLLOUT 已在 send_response 中设置
    } else if (conn->closing && conn->out_buf.empty()) {
        close_connection(fd, epoll);
    }
    // 正常情况：无需任何操作，fd 仍注册着 EPOLLIN|EPOLLET
}

// ── 处理单个客户端 fd 的可写事件（EPOLLOUT）──
static void process_client_write(int fd, Epoll* epoll) {
    ConnState* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(epoll->mtx);
        auto it = epoll->conns.find(fd);
        if (it == epoll->conns.end()) return;
        conn = it->second;
    }

    if (epoll->is_timed_out(fd)) {
        close_connection(fd, epoll);
        return;
    }

    // 尝试刷新输出缓冲区
    bool flushed = flush_out_buf(fd, conn);

    if (conn->closing && conn->out_buf.empty()) {
        // 协议错误后，响应已发完，关闭连接
        close_connection(fd, epoll);
        return;
    }

    if (flushed) {
        // 全部发送完成，恢复为只监听读事件
        epoll->mod(fd, EPOLLIN | EPOLLET);
    } else {
        // 仍未发完，继续等待 EPOLLOUT
        epoll->mod(fd, EPOLLIN | EPOLLOUT | EPOLLET);
    }
}

void Epoll::handle_events(int listen_fd, int nfds) {
    if (nfds <= 0) return;

    for (int i = 0; i < nfds; ++i) {
        int fd = events[i].data.fd;
        uint32_t ev = events[i].events;

        if (fd == listen_fd) {
            // ET 模式必须在循环中 accept 直到 EAGAIN，否则并发连接会丢失
            while (true) {
                struct sockaddr_in client_addr;
                socklen_t len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr,
                                       &len);
                if (client_fd == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    Log::error("accept failed"); break;
                }

                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                int tcp_nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                           &tcp_nodelay, sizeof(tcp_nodelay));

                add(client_fd, EPOLLIN | EPOLLET);
                Timer::add(client_fd, 60);
                g_db.add_connection();   // 运维统计：当前连接数 +1
            }
        } else if (ev & (EPOLLERR | EPOLLHUP)) {
            close_connection(fd, this);
        } else {
            // 可能同时有读和写事件
            if (ev & EPOLLOUT) {
                process_client_write(fd, this);
            }
            if (ev & EPOLLIN) {
                process_client_read(fd, this, g_db, g_cmd);
            }
        }
    }
}
