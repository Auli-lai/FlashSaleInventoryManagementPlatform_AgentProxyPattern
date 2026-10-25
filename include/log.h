#ifndef LOG_H
#define LOG_H

#include <string>
#include <fstream>
#include <mutex>

// ═══════════════════════════════════════════════════════════
//  简易日志系统（从 mini_redis 项目复制）
//  线程安全，同时输出到文件和控制台
// ═══════════════════════════════════════════════════════════

class Log {
public:
    static void init(const std::string& filename = "mini_redis_gui.log");
    static void info(const char* format, ...);
    static void error(const char* format, ...);
    static void close();

private:
    static std::ofstream log_file;
    static std::mutex mtx;
    static void log(const char* level, const char* format, va_list args);
};

#endif // LOG_H
