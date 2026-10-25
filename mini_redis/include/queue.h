#ifndef QUEUE_H
#define QUEUE_H

#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue;
    mutable std::mutex mtx;
    std::condition_variable cv;
    bool shutdown = false;

public:
    void push(const T& item);
    void push(T&& item);
    bool pop(T& item);
    void shutdown_queue();
    bool empty() const;
    size_t size() const;
};

template <typename T>
void ThreadSafeQueue<T>::push(const T& item) {
    std::lock_guard<std::mutex> lock(mtx);
    if (shutdown) return;
    queue.push(item);
    cv.notify_one();
}

template <typename T>
void ThreadSafeQueue<T>::push(T&& item) {
    std::lock_guard<std::mutex> lock(mtx);
    if (shutdown) return;
    queue.push(std::move(item));
    cv.notify_one();
}

template <typename T>
bool ThreadSafeQueue<T>::pop(T& item) {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [this]() { return !queue.empty() || shutdown; });
    if (queue.empty() && shutdown) return false;
    item = std::move(queue.front());
    queue.pop();
    return true;
}

template <typename T>
void ThreadSafeQueue<T>::shutdown_queue() {
    std::lock_guard<std::mutex> lock(mtx);
    shutdown = true;
    cv.notify_all();
}

template <typename T>
bool ThreadSafeQueue<T>::empty() const {
    std::lock_guard<std::mutex> lock(mtx);
    return queue.empty();
}

template <typename T>
size_t ThreadSafeQueue<T>::size() const {
    std::lock_guard<std::mutex> lock(mtx);
    return queue.size();
}

#endif // QUEUE_H
