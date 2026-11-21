#pragma once

#include "drivers/common/db_types.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace capybaradb::core {

struct NoticeEntry {
    std::string severity;
    std::string sql_state;
    std::string message;
    int32_t     position    = -1;
    int32_t     native_code = 0;
};

/// Bounded, thread-safe capture for query notices
/// (max 100 notices, 4096 chars per item, 65536 chars total).
class BoundedNoticeLog final {
public:
    static constexpr size_t kMaxNotices        = 100;
    static constexpr size_t kMaxCharsPerNotice = 4096;
    static constexpr size_t kMaxTotalChars     = 65536;

    void add(
        std::string severity,
        std::string sql_state,
        std::string message,
        int32_t position = -1,
        int32_t native_code = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        const size_t len = severity.size() + sql_state.size() + message.size();
        if (items_.size() >= kMaxNotices || len > kMaxCharsPerNotice || total_chars_ + len > kMaxTotalChars) {
            if (omitted_ < UINT64_MAX) {
                ++omitted_;
            }
            return;
        }
        total_chars_ += len;
        items_.push_back(NoticeEntry{
            std::move(severity),
            std::move(sql_state),
            std::move(message),
            position,
            native_code,
        });
    }

    [[nodiscard]] std::vector<NoticeEntry> snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return items_;
    }

    [[nodiscard]] uint64_t omitted_count() const noexcept {
        std::lock_guard<std::mutex> lock(mu_);
        return omitted_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        items_.clear();
        total_chars_ = 0;
        omitted_     = 0;
    }

private:
    mutable std::mutex       mu_;
    std::vector<NoticeEntry> items_;
    size_t                   total_chars_ = 0;
    uint64_t                 omitted_     = 0;
};

/// Lock-free SPSC publisher/notifier between DB Worker thread and UI thread.
/// Implements exact CapybaraDB milestones (1, 30, 100, every 10,000 rows + 250 ms heartbeat)
/// and guarantees at most 1 queued UI update pending at any time via std::atomic_flag.
/// Stores its callback inline in a 64-byte fixed buffer without std::function or std::mutex.
class StreamPublishSink final {
public:
    using CallbackFn  = void (*)(void* ctx);
    using DestroyFn   = void (*)(void* ctx) noexcept;
    static constexpr int64_t kHeartbeatMs      = 250;
    static constexpr size_t  kInlineStorageCap = 64u;

    StreamPublishSink()
        : last_notify_time_(std::chrono::steady_clock::now()) {}

    ~StreamPublishSink() noexcept {
        clear_callback();
    }

    StreamPublishSink(const StreamPublishSink&) = delete;
    StreamPublishSink& operator=(const StreamPublishSink&) = delete;

    void set_callback(std::nullptr_t) noexcept {
        clear_callback();
    }

    void set_callback(CallbackFn fn, void* ctx) noexcept {
        acquire_cb_lock();
        destroy_inline_locked();
        cb_fn_  = fn;
        cb_ctx_ = ctx;
        release_cb_lock();
    }

    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, std::nullptr_t> &&
                  !std::is_convertible_v<F, CallbackFn>)
    void set_callback(F&& fn) noexcept {
        using Stored = std::decay_t<F>;
        static_assert(sizeof(Stored) <= kInlineStorageCap && alignof(Stored) <= alignof(std::max_align_t),
                      "StreamPublishSink callback functor must fit in 64-byte inline buffer");
        acquire_cb_lock();
        destroy_inline_locked();
        void* slot = static_cast<void*>(inline_buf_);
        new (slot) Stored(std::forward<F>(fn));
        cb_fn_ = +[](void* ctx) {
            (*static_cast<Stored*>(ctx))();
        };
        cb_destroy_ = +[](void* ctx) noexcept {
            static_cast<Stored*>(ctx)->~Stored();
        };
        cb_ctx_ = slot;
        release_cb_lock();
    }

    void clear_callback() noexcept {
        acquire_cb_lock();
        destroy_inline_locked();
        cb_fn_  = nullptr;
        cb_ctx_ = nullptr;
        release_cb_lock();
    }

    /// Returns true if a milestone or 250 ms heartbeat triggered a notification check.
    bool on_rows_published(uint64_t total_rows) {
        const auto now = std::chrono::steady_clock::now();
        bool should_notify =
            (total_rows == 1 || total_rows == 30 || total_rows == 100 || (total_rows > 0 && (total_rows % 10000) == 0));
        if (!should_notify) {
            const auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_notify_time_).count();
            should_notify = (elapsed_ms >= kHeartbeatMs);
        }
        if (should_notify) {
            last_notify_time_ = now;
            notify_if_unscheduled();
            return true;
        }
        return false;
    }

    /// Coalesced notification: dispatches callback only if no UI notification is already in flight.
    void notify_if_unscheduled() {
        if (ui_update_pending_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        acquire_cb_lock();
        const CallbackFn fn = cb_fn_;
        void* const ctx     = cb_ctx_;
        if (fn != nullptr) {
            fn(ctx);
            release_cb_lock();
        } else {
            release_cb_lock();
            ui_update_pending_.clear(std::memory_order_release);
        }
    }

    /// Called by the UI thread at the start of its update handler to re-arm notifications.
    void clear_pending() noexcept {
        ui_update_pending_.clear(std::memory_order_release);
    }

    [[nodiscard]] bool is_pending_for_test() noexcept {
        if (!ui_update_pending_.test_and_set(std::memory_order_acq_rel)) {
            ui_update_pending_.clear(std::memory_order_release);
            return false;
        }
        return true;
    }

private:
    void acquire_cb_lock() noexcept {
        while (cb_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }

    void release_cb_lock() noexcept {
        cb_lock_.clear(std::memory_order_release);
    }

    void destroy_inline_locked() noexcept {
        if (cb_destroy_ != nullptr && cb_ctx_ != nullptr) {
            cb_destroy_(cb_ctx_);
        }
        cb_destroy_ = nullptr;
    }

    alignas(std::max_align_t) std::byte   inline_buf_[kInlineStorageCap]{};
    std::atomic_flag                      cb_lock_           = ATOMIC_FLAG_INIT;
    CallbackFn                            cb_fn_             = nullptr;
    DestroyFn                             cb_destroy_        = nullptr;
    void*                                 cb_ctx_            = nullptr;
    std::atomic_flag                      ui_update_pending_ = ATOMIC_FLAG_INIT;
    std::chrono::steady_clock::time_point last_notify_time_;
};

} // namespace capybaradb::core
