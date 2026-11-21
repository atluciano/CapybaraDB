#include "core/threading/db_session_worker.hpp"

namespace capybaradb::core {

DbSessionWorker::~DbSessionWorker() {
    stop();
}

void DbSessionWorker::ensure_started_locked() {
    if (!worker_.joinable() && !stopping_) {
        worker_ = std::thread([this]() { worker_loop(); });
    }
    if (!cancel_thread_.joinable() && !stopping_) {
        cancel_thread_ = std::thread([this]() { cancel_loop(); });
    }
}

CancelToken* DbSessionWorker::submit(TaskFn task) {
    CancelToken* tok = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (stopping_ || count_ >= kRingCapacity) {
            stopped_token_.cancel();
            return &stopped_token_;
        }
        ensure_started_locked();
        TaskSlot& slot = ring_[tail_];
        slot.cancel.reset();
        slot.fn = std::move(task);
        tok     = &slot.cancel;
        tail_   = (tail_ + 1u) % kRingCapacity;
        ++count_;
    }
    cv_.notify_one();
    return tok;
}

void DbSessionWorker::cancel_active() noexcept {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (active_cancel_ != nullptr) {
            active_cancel_->cancel();
        }
        size_t idx = head_;
        for (size_t i = 0; i < count_; ++i) {
            ring_[idx].cancel.cancel();
            idx = (idx + 1u) % kRingCapacity;
        }
    }

    // Wake dedicated cancel thread to dispatch PQcancel / SQLCancelHandle off the UI thread.
    {
        std::lock_guard<std::mutex> clk(cancel_mu_);
        ++cancel_epoch_;
    }
    cancel_cv_.notify_one();
}

void DbSessionWorker::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }

    cancel_active();
    cv_.notify_all();

    if (worker_.joinable()) {
        worker_.join();
    }

    {
        std::lock_guard<std::mutex> clk(cancel_mu_);
        cancel_stopping_ = true;
    }
    cancel_cv_.notify_all();
    if (cancel_thread_.joinable()) {
        cancel_thread_.join();
    }

    std::unique_ptr<drivers::DbDriver> leftover = replace_driver(nullptr);
    if (leftover) {
        leftover->disconnect();
    }
}

std::unique_ptr<drivers::DbDriver> DbSessionWorker::replace_driver(std::unique_ptr<drivers::DbDriver> next) {
    std::unique_ptr<drivers::DbDriver> old;
    {
        std::lock_guard<std::mutex> drv_lock(cancel_dispatch_mu_);
        old     = std::move(driver_);
        driver_ = std::move(next);
    }
    return old;
}

void DbSessionWorker::cancel_loop() {
    uint64_t seen_epoch = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> clk(cancel_mu_);
            cancel_cv_.wait(clk, [this, seen_epoch]() {
                return cancel_stopping_ || cancel_epoch_ != seen_epoch;
            });
            if (cancel_epoch_ != seen_epoch) {
                seen_epoch = cancel_epoch_;
            } else if (cancel_stopping_) {
                return;
            }
        }

        std::lock_guard<std::mutex> drv_lock(cancel_dispatch_mu_);
        if (driver_ != nullptr) {
            driver_->cancel_active();
        }
    }
}

void DbSessionWorker::worker_loop() {
    while (true) {
        TaskFn fn;
        CancelToken* cancel_tok = nullptr;
        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            cv_.wait(lock, [this]() { return stopping_ || count_ > 0; });
            if (stopping_ && count_ == 0) {
                return;
            }
            TaskSlot& slot = ring_[head_];
            fn             = std::move(slot.fn);
            cancel_tok     = &slot.cancel;
            active_cancel_ = cancel_tok;
            head_          = (head_ + 1u) % kRingCapacity;
            --count_;
        }

        if (fn && cancel_tok != nullptr && !cancel_tok->is_canceled()) {
            fn(*this, *cancel_tok);
        }

        {
            std::lock_guard<std::mutex> lock(queue_mu_);
            active_cancel_ = nullptr;
        }
    }
}

} // namespace capybaradb::core
