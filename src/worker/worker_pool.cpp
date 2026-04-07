#include "worker/worker_pool.h"

#include <exception>
#include <iostream>
#include <utility>

WorkerPool::WorkerPool(size_t worker_count) {
    workers_.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back(&WorkerPool::WorkerLoop, this);
    }
}

WorkerPool::~WorkerPool() {
    Stop();
}

bool WorkerPool::Submit(Task task) {
    if (!task) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        tasks_.push(std::move(task));
    }

    cv_.notify_one();
    return true;
}

void WorkerPool::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
    workers_.clear();
}

void WorkerPool::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& ex) {
            std::cerr << "worker task threw exception: " << ex.what() << '\n';
        } catch (...) {
            std::cerr << "worker task threw unknown exception\n";
        }
    }
}
