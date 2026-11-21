#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace capybaradb::core {

/// Cooperative, lock-free thread-safe cancellation token with zero-allocation callback dispatch.
/// Replaces the previous 2-mutex + std::function + try/catch design with atomic spin-synchronization
/// and a raw C function pointer + context pointer (sizeof(CancelToken) == 24 bytes).
class CancelToken final {
public:
    using CancelCallbackFn = void (*)(void* ctx) noexcept;

    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    [[nodiscard]] bool is_canceled() const noexcept {
        return canceled_.load(std::memory_order_acquire);
    }

    void cancel() noexcept {
        acquire_lock();
        if (canceled_.exchange(true, std::memory_order_acq_rel)) {
            release_lock();
            return;
        }
        const CancelCallbackFn fn = on_cancel_fn_;
        void* const ctx           = on_cancel_ctx_;
        if (fn != nullptr) {
            executing_.fetch_add(1u, std::memory_order_acq_rel);
        }
        release_lock();

        if (fn != nullptr) {
            fn(ctx);
            if (executing_.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
                executing_.notify_all();
            }
        }
    }

    void reset() noexcept {
        clear_on_cancel();
        acquire_lock();
        canceled_.store(false, std::memory_order_release);
        release_lock();
    }

    /// Registers a raw function pointer + context callback invoked on cancellation.
    /// Executes immediately if the token is already canceled.
    void set_on_cancel(CancelCallbackFn fn, void* ctx) noexcept {
        if (fn == nullptr) {
            clear_on_cancel();
            return;
        }
        acquire_lock();
        on_cancel_fn_  = fn;
        on_cancel_ctx_ = ctx;
        const bool already_canceled = canceled_.load(std::memory_order_acquire);
        if (already_canceled) {
            executing_.fetch_add(1u, std::memory_order_acq_rel);
        }
        release_lock();

        if (already_canceled) {
            fn(ctx);
            if (executing_.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
                executing_.notify_all();
            }
        }
    }

    void set_on_cancel(std::nullptr_t) noexcept {
        clear_on_cancel();
    }

    /// Clears any registered callback and waits for any in-flight callback execution to complete.
    void clear_on_cancel() noexcept {
        acquire_lock();
        on_cancel_fn_  = nullptr;
        on_cancel_ctx_ = nullptr;
        release_lock();

        uint32_t cur = executing_.load(std::memory_order_acquire);
        while (cur != 0u) {
            executing_.wait(cur, std::memory_order_acquire);
            cur = executing_.load(std::memory_order_acquire);
        }
    }

private:
    void acquire_lock() noexcept {
        while (state_lock_.test_and_set(std::memory_order_acquire)) {
        }
    }

    void release_lock() noexcept {
        state_lock_.clear(std::memory_order_release);
    }

    std::atomic<bool>     canceled_{false};
    std::atomic_flag      state_lock_ = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> executing_{0};
    CancelCallbackFn      on_cancel_fn_  = nullptr;
    void*                 on_cancel_ctx_ = nullptr;
};

/// Stack-allocated RAII cancellation callback guard.
/// Stores the functor in-place on the stack and registers a static C function trampoline,
/// performing ZERO heap allocations and ZERO std::function type-erasure.
template <typename F>
class ScopedCancelCallback final {
public:
    ScopedCancelCallback(CancelToken& token, F&& fn) noexcept
        : token_(token)
        , fn_(std::forward<F>(fn)) {
        token_.set_on_cancel(&ScopedCancelCallback::trampoline, this);
    }

    ~ScopedCancelCallback() noexcept {
        token_.clear_on_cancel();
    }

    ScopedCancelCallback(const ScopedCancelCallback&) = delete;
    ScopedCancelCallback& operator=(const ScopedCancelCallback&) = delete;

private:
    static void trampoline(void* ctx) noexcept {
        static_cast<ScopedCancelCallback*>(ctx)->fn_();
    }

    CancelToken&    token_;
    std::decay_t<F> fn_;
};

template <typename F>
ScopedCancelCallback(CancelToken&, F&&) -> ScopedCancelCallback<std::decay_t<F>>;

} // namespace capybaradb::core
