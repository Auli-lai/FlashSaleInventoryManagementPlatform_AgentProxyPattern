#include "log.h"
#include <ctime>
#include <cstdarg>
#include <iostream>

std::ofstream Log::log_file;
std::mutex Log::mtx;

void Log::init(const std::string& filename) {
    log_file.open(filename, std::ios::app);
    if (!log_file) {
        std::cerr << "Failed to open log file: " << filename << std::endl;
    }
}

void Log::info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log("INFO", format, args);
    va_end(args);
}

void Log::error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    log("ERROR", format, args);
    va_end(args);
}

void Log::close() {
    if (log_file.is_open()) log_file.close();
}

void Log::log(const char* level, const char* format, va_list args) {
    std::lock_guard<std::mutex> lock(mtx);

    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);

    if (log_file.is_open()) {
        log_file << "[" << time_str << "] [" << level << "] " << buffer << std::endl;
    }
    std::cout << "[" << time_str << "] [" << level << "] " << buffer << std::endl;
}
