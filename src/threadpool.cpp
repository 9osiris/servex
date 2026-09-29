// fixed worker thread pool with a shared job queue
#include "threadpool.h"

ThreadPool::ThreadPool(int workers) {
    if (workers < 1) workers = 1;
    if (workers > 256) workers = 256;
    workers_.reserve(workers);
    for (int i = 0; i < workers; i++) workers_.emplace_back();
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    for (auto& t : workers_) t = std::thread(&ThreadPool::worker_loop, this);
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stopping_ = true;
    }
    cond_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    // allow restart after a full stop
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    stopping_ = false;
}

bool ThreadPool::submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || !started_) return false;
        queue_.push_back(std::move(job));
    }
    cond_.notify_one();
    return true;
}

int ThreadPool::queue_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int)queue_.size();
}

void ThreadPool::worker_loop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return; // stopping and drained
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (...) {
            // a bad job must not kill the worker
        }
    }
}
