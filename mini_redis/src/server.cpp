#include "server.h"
#include "epoll.h"
#include "timer.h"
#include "log.h"
#include "db.h"
#include "command.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <csignal>

// 全局对象（epoll.cpp 和 main.cpp 会引用）
Database g_db;
CommandTable g_cmd(&g_db);

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) g_running = 0;
}

Server::Server(uint16_t port, const std::string& ip)
    : listen_fd(-1), port(port), ip(ip) {}

Server::~Server() {
    if (listen_fd != -1) close(listen_fd);
}

void Server::start() {
    // 信号处理
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // 加载 AOF 文件
    g_db.set_aof_file("appendonly.aof");
    g_db.load_aof();

    // 创建监听 socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1) { Log::error("socket failed"); return; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        Log::error("bind failed"); close(listen_fd); return;
    }
    if (listen(listen_fd, 512) == -1) {
        Log::error("listen failed"); close(listen_fd); return;
    }

    // 设置 listen_fd 为非阻塞，配合 ET accept 循环
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    Log::info("mini-redis started on %s:%d", ip.c_str(), port);
    Log::info("Connect: redis-cli -p %d", port);

    Epoll epoll;
    epoll.add(listen_fd, EPOLLIN | EPOLLET);

    time_t last_expire_check = time(nullptr);

    while (g_running) {
        int nfds = epoll.wait(10); // 10ms — 降低延迟，避免空等
        if (nfds == -1) {
            if (errno == EINTR) continue;
            Log::error("epoll_wait failed"); continue;
        }

        // 连接超时检查
        std::vector<int> timed_out = Timer::tick();
        for (int fd : timed_out) epoll.mark_timed_out(fd);

        // 处理事件
        epoll.handle_events(listen_fd, nfds);

        // 定期键过期检查（每秒最多一次）
        time_t now = time(nullptr);
        if (now - last_expire_check >= 1) {
            g_db.active_expire_cycle(20);
            last_expire_check = now;
        }
    }

    Log::info("Shutting down...");
    close(listen_fd);
    listen_fd = -1;
    Log::info("mini-redis stopped");
}

void Server::stop() {
    if (listen_fd != -1) { close(listen_fd); listen_fd = -1; }
}
