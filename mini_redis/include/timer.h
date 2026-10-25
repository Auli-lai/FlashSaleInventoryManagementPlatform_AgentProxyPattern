#ifndef TIMER_H
#define TIMER_H

#include <unordered_map>
#include <vector>
#include <ctime>
#include <mutex>
#include <functional>

class Timer {
public:
    struct TimerNode {
        int key_hash;       // 通用 key 标识 (fd 或 string hash)
        time_t expire;
        bool operator<(const TimerNode& other) const {
            return expire > other.expire; // 小顶堆
        }
    };

    static void add(int key_hash, int timeout);
    static void remove(int key_hash);
    static void refresh(int key_hash, int timeout);
    static std::vector<int> tick();

private:
    static std::vector<TimerNode> heap;
    static std::unordered_map<int, size_t> key_to_idx;
    static std::mutex mtx;

    static void sift_up(size_t i);
    static void sift_down(size_t i);
    static void swap_nodes(size_t i, size_t j);
};

#endif // TIMER_H
