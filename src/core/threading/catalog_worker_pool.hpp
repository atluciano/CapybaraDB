#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace capybaradb::core {

/// Fixed-size worker pool (2..4 threads by default, spawned lazily on first submit)
/// for Object Explorer, connection tests, and async database listing without blocking the UI thread.
class CatalogWorkerPool final {
public:
    explicit CatalogWorkerPool(size_t thread_count = 0);
    ~CatalogWorkerPool();

    CatalogWorkerPool(const CatalogWorkerPool&) = delete;
    CatalogWorkerPool& operator=(const CatalogWorkerPool&) = delete;
    CatalogWorkerPool(CatalogWorkerPool&&) = delete;
    CatalogWorkerPool& operator=(CatalogWorkerPool&&) = delete;

    void submit(std::function<void()> task);
    void stop();
    [[nodiscard]] size_t worker_count() const noexcept;
    [[nodiscard]] size_t configured_worker_count() const noexcept { return target_workers_; }

private:
    void ensure_started_locked();
    void worker_loop();

    size_t                            target_workers_ = 2;
    mutable std::mutex                mutex_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread>          workers_;
    bool                              stopping_ = false;
};

} // namespace capybaradb::core
