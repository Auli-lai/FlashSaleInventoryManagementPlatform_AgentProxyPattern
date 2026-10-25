#ifndef LOG_H
#define LOG_H

#include <string>
#include <fstream>
#include <mutex>

class Log {
private:
    static std::ofstream log_file;
    static std::mutex mtx;

public:
    static void init(const std::string& filename = "mini_redis.log");
    static void info(const char* format, ...);
    static void error(const char* format, ...);
    static void close();

private:
    static void log(const char* level, const char* format, va_list args);
};

#endif // LOG_H
