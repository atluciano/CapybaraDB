#include "core/threading/catalog_worker_pool.hpp"

#include <algorithm>
#include <utility>

namespace capybaradb::core {

CatalogWorkerPool::CatalogWorkerPool(size_t thread_count) {
    const unsigned hw = std::thread::hardware_concurrency();
    target_workers_ = (thread_count > 0)
        ? thread_count
        : std::clamp<size_t>(hw == 0 ? 2 : hw, 2, 4);
}

CatalogWorkerPool::~CatalogWorkerPool() {
    stop();
}

void CatalogWorkerPool::ensure_started_locked() {
    if (!workers_.empty() || stopping_) {
        return;
    }
    workers_.reserve(target_workers_);
    for (size_t i = 0; i < target_workers_; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

size_t CatalogWorkerPool::worker_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.size();
}

void CatalogWorkerPool::submit(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        ensure_started_locked();
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void CatalogWorkerPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && workers_.empty()) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void CatalogWorkerPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

} // namespace capybaradb::core
