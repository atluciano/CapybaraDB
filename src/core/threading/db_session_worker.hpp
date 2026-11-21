#pragma once

#include "core/threading/cancel_token.hpp"
#include "drivers/common/db_driver.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace capybaradb::core {

/// Dedicated background worker owning 1 database session thread per query tab
/// (strict thread affinity for libpq / ODBC handles and immediate cancel signaling).
/// Uses a fixed 64-slot ring buffer with embedded CancelTokens (zero std::deque or std::make_shared per submit)
/// and single-owner std::unique_ptr<drivers::DbDriver>.
class DbSessionWorker final {
public:
    static constexpr size_t kRingCapacity = 64u;
    using TaskFn = std::function<void(DbSessionWorker&, CancelToken&)>;

    DbSessionWorker() = default;
    ~DbSessionWorker();

    DbSessionWorker(const DbSessionWorker&) = delete;
    DbSessionWorker& operator=(const DbSessionWorker&) = delete;

    /// Queues work sequentially in the pre-allocated ring buffer on the dedicated session thread.
    /// Returns a pointer to the slot's embedded CancelToken (also canceled by cancel_active()).
    CancelToken* submit(TaskFn task);

    /// Thread-safe, NON-BLOCKING cancellation callable from the UI thread or hotkey filters:
    /// flips active/queued CancelTokens immediately and dispatches driver_->cancel_active()
    /// (`PQcancel` / `SQLCancelHandle`) on a dedicated cancel thread so the UI never freezes.
    void cancel_active() noexcept;

    /// Cancels all operations and joins the dedicated worker and cancel threads (safe to call repeatedly).
    void stop() noexcept;

    // --- Accessors intended for use from within tasks running on the session thread ---
    [[nodiscard]] drivers::DbDriver* driver() const noexcept { return driver_.get(); }

    /// Replaces the session driver safely under cancel_dispatch_mu_ and returns the prior instance
    /// so cleanup/disconnect can execute without holding cancel_dispatch_mu_.
    std::unique_ptr<drivers::DbDriver> replace_driver(std::unique_ptr<drivers::DbDriver> next);

private:
    struct TaskSlot {
        CancelToken cancel;
        TaskFn      fn;
    };

    void ensure_started_locked();
    void worker_loop();
    void cancel_loop();

    std::mutex                           queue_mu_;
    std::condition_variable              cv_;
    std::array<TaskSlot, kRingCapacity>  ring_{};
    size_t                               head_          = 0;
    size_t                               tail_          = 0;
    size_t                               count_         = 0;
    CancelToken*                         active_cancel_ = nullptr;
    CancelToken                          stopped_token_;
    bool                                 stopping_      = false;
    std::thread                          worker_;

    std::mutex              cancel_mu_;
    std::condition_variable cancel_cv_;
    uint64_t                cancel_epoch_    = 0;
    bool                    cancel_stopping_ = false;
    std::thread             cancel_thread_;

    mutable std::mutex                 cancel_dispatch_mu_;
    std::unique_ptr<drivers::DbDriver> driver_;
};

} // namespace capybaradb::core
