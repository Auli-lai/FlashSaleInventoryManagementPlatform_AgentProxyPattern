#ifndef SERVER_H
#define SERVER_H

#include <string>
#include <cstdint>

class Server {
public:
    Server(uint16_t port, const std::string& ip = "0.0.0.0");
    ~Server();
    void start();
    void stop();

private:
    int listen_fd;
    uint16_t port;
    std::string ip;
};

#endif // SERVER_H
