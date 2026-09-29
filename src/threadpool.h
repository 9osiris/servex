#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// fixed pool of worker threads draining a shared job queue.
// connections are dispatched here instead of spawning a thread each.
class ThreadPool {
public:
    explicit ThreadPool(int workers);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void start();
    // stop accepting jobs, run the queued ones, then join the workers
    void stop();
    // false when the pool is stopping
    bool submit(std::function<void()> job);
    int worker_count() const { return (int)workers_.size(); }
    int queue_depth() const;

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool stopping_ = false;
    bool started_ = false;
};
