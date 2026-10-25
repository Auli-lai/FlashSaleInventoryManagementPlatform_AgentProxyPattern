#include "server.h"
#include "log.h"

int main(int argc, char* argv[]) {
    Log::init("mini_redis.log");

    uint16_t port = 6379;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    Log::info("mini-redis v1.0 starting...");

    Server server(port);
    server.start();

    Log::close();
    return 0;
}
