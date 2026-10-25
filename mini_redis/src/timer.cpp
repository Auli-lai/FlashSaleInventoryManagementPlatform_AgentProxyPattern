#include "timer.h"
#include "log.h"

std::vector<Timer::TimerNode> Timer::heap;
std::unordered_map<int, size_t> Timer::key_to_idx;
std::mutex Timer::mtx;

void Timer::add(int key_hash, int timeout) {
    std::lock_guard<std::mutex> lock(mtx);
    time_t expire = time(nullptr) + timeout;
    heap.push_back({key_hash, expire});
    size_t i = heap.size() - 1;
    key_to_idx[key_hash] = i;
    sift_up(i);
}

void Timer::remove(int key_hash) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = key_to_idx.find(key_hash);
    if (it != key_to_idx.end()) {
        size_t idx = it->second;
        swap_nodes(idx, heap.size() - 1);
        heap.pop_back();
        key_to_idx.erase(it);
        if (idx < heap.size()) {
            sift_up(idx);
            sift_down(idx);
        }
    }
}

void Timer::refresh(int key_hash, int timeout) {
    std::lock_guard<std::mutex> lock(mtx);
    auto it = key_to_idx.find(key_hash);
    if (it != key_to_idx.end()) {
        time_t new_expire = time(nullptr) + timeout;
        time_t old_expire = heap[it->second].expire;
        heap[it->second].expire = new_expire;
        if (new_expire < old_expire)
            sift_up(it->second);
        else
            sift_down(it->second);
    }
}

std::vector<int> Timer::tick() {
    std::vector<int> expired;
    std::lock_guard<std::mutex> lock(mtx);
    time_t now = time(nullptr);
    while (!heap.empty() && heap[0].expire <= now) {
        int key = heap[0].key_hash;
        key_to_idx.erase(key);
        swap_nodes(0, heap.size() - 1);
        heap.pop_back();
        if (!heap.empty()) sift_down(0);
        expired.push_back(key);
    }
    return expired;
}

void Timer::sift_up(size_t i) {
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (heap[i] < heap[parent]) {
            swap_nodes(i, parent);
            i = parent;
        } else break;
    }
}

void Timer::sift_down(size_t i) {
    size_t n = heap.size();
    while (true) {
        size_t left = 2 * i + 1, right = 2 * i + 2, smallest = i;
        if (left < n && heap[left] < heap[smallest]) smallest = left;
        if (right < n && heap[right] < heap[smallest]) smallest = right;
        if (smallest != i) {
            swap_nodes(i, smallest);
            i = smallest;
        } else break;
    }
}

void Timer::swap_nodes(size_t i, size_t j) {
    std::swap(heap[i], heap[j]);
    key_to_idx[heap[i].key_hash] = i;
    key_to_idx[heap[j].key_hash] = j;
}
