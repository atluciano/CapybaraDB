#include "core/arena/arena_allocator.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace capybaradb::core {
namespace {

constexpr size_t kHeaderReserve = 64;
static_assert(sizeof(ArenaBlock) <= kHeaderReserve);

constexpr size_t align_up(size_t value, size_t alignment) noexcept {
    const size_t a = (alignment == 0) ? 1 : alignment;
    return (value + (a - 1)) & ~(a - 1);
}

size_t os_page_size() noexcept {
#ifdef _WIN32
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize > 0 ? static_cast<size_t>(si.dwPageSize) : 4096u;
#else
    const long ps = ::sysconf(_SC_PAGESIZE);
    return ps > 0 ? static_cast<size_t>(ps) : 4096u;
#endif
}

#ifdef _WIN32
constexpr size_t kWinReserveChunkBytes = 512ull * 1024 * 1024; // 512 MiB virtual reservation (0 physical RAM until committed)
#else
std::byte* os_map_pages(size_t bytes) noexcept {
    void* ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    return static_cast<std::byte*>(ptr);
}
#endif

void os_unmap_region(std::byte* base, size_t bytes) noexcept {
    if (base == nullptr) {
        return;
    }
#ifdef _WIN32
    (void)bytes;
    (void)VirtualFree(base, 0, MEM_RELEASE);
#else
    (void)::munmap(base, bytes);
#endif
}

struct PendingUnmap {
    std::byte* base  = nullptr;
    size_t     bytes = 0;
};

constexpr size_t kAsyncUnmapThresholdBytes = 32ull * 1024 * 1024; // 32 MiB
constexpr size_t kReaperRingCapacity       = 256u;

/// Zero-heap-allocation fixed-ring OS page reaper that executes VirtualFree / munmap off the
/// caller thread for large (>= 32 MiB) arenas so reset() never stalls on kernel PTE teardown.
struct OsRegionReaper {
    std::mutex              mu;
    std::condition_variable cv;
    PendingUnmap            ring[kReaperRingCapacity]{};
    size_t                  head     = 0;
    size_t                  tail     = 0;
    size_t                  count    = 0;
    bool                    stopping = false;
    bool                    started  = false;
    std::thread             worker;

    OsRegionReaper() = default;

    ~OsRegionReaper() noexcept {
        {
            std::lock_guard<std::mutex> lock(mu);
            stopping = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void enqueue(const PendingUnmap* items, size_t item_count) noexcept {
        if (item_count == 0) {
            return;
        }
        size_t pushed = 0;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!stopping && !started) {
                started = true;
                worker  = std::thread([this]() noexcept { run(); });
            }
            if (!stopping) {
                while (pushed < item_count && count < kReaperRingCapacity) {
                    ring[tail] = items[pushed];
                    tail = (tail + 1) % kReaperRingCapacity;
                    ++count;
                    ++pushed;
                }
            }
        }
        if (pushed > 0) {
            cv.notify_one();
        }
        // Any overflow or post-stop regions are unmapped synchronously without throwing or allocating.
        for (size_t i = pushed; i < item_count; ++i) {
            os_unmap_region(items[i].base, items[i].bytes);
        }
    }

    void run() noexcept {
        PendingUnmap batch[64]{};
        for (;;) {
            size_t batch_count = 0;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait(lock, [this]() { return stopping || count > 0; });
                if (count == 0 && stopping) {
                    return;
                }
                while (count > 0 && batch_count < 64u) {
                    batch[batch_count++] = ring[head];
                    head = (head + 1) % kReaperRingCapacity;
                    --count;
                }
            }
            for (size_t i = 0; i < batch_count; ++i) {
                os_unmap_region(batch[i].base, batch[i].bytes);
            }
        }
    }
};

OsRegionReaper& global_os_reaper() noexcept {
    static OsRegionReaper reaper;
    return reaper;
}

} // namespace

ArenaAllocator::~ArenaAllocator() noexcept {
    reset();
}

ArenaAllocator::ArenaAllocator(ArenaAllocator&& other) noexcept
    : head_(other.head_)
    , current_(other.current_)
    , win_reg_base_(other.win_reg_base_)
    , win_reg_size_(other.win_reg_size_)
    , win_reg_commit_(other.win_reg_commit_)
    , next_block_cap_(other.next_block_cap_)
    , total_reserved_(other.total_reserved_)
    , total_used_(other.total_used_)
    , block_count_(other.block_count_) {
    other.head_           = nullptr;
    other.current_        = nullptr;
    other.win_reg_base_   = nullptr;
    other.win_reg_size_   = 0;
    other.win_reg_commit_ = 0;
    other.next_block_cap_ = kMinBlockBytes;
    other.total_reserved_ = 0;
    other.total_used_     = 0;
    other.block_count_    = 0;
}

ArenaAllocator& ArenaAllocator::operator=(ArenaAllocator&& other) noexcept {
    if (this != &other) {
        reset();
        head_           = other.head_;
        current_        = other.current_;
        win_reg_base_   = other.win_reg_base_;
        win_reg_size_   = other.win_reg_size_;
        win_reg_commit_ = other.win_reg_commit_;
        next_block_cap_ = other.next_block_cap_;
        total_reserved_ = other.total_reserved_;
        total_used_     = other.total_used_;
        block_count_    = other.block_count_;

        other.head_           = nullptr;
        other.current_        = nullptr;
        other.win_reg_base_   = nullptr;
        other.win_reg_size_   = 0;
        other.win_reg_commit_ = 0;
        other.next_block_cap_ = kMinBlockBytes;
        other.total_reserved_ = 0;
        other.total_used_     = 0;
        other.block_count_    = 0;
    }
    return *this;
}

bool ArenaAllocator::grow(size_t min_bytes, size_t alignment) noexcept {
    const size_t page = os_page_size();
    const size_t start_off = align_up(kHeaderReserve, alignment);
    if (min_bytes > SIZE_MAX - start_off - page) {
        return false;
    }
    const size_t required = align_up(start_off + min_bytes, page);
    const size_t cap = (std::max)(required, next_block_cap_);

    std::byte* raw = nullptr;
    bool owns_region = false;
    size_t region_size = cap;

#ifdef _WIN32
    if (win_reg_base_ == nullptr || win_reg_commit_ + cap > win_reg_size_) {
        const size_t res_sz = (std::max)(kWinReserveChunkBytes, cap);
        void* reserved = VirtualAlloc(nullptr, res_sz, MEM_RESERVE, PAGE_NOACCESS);
        if (reserved == nullptr) {
            return false;
        }
        win_reg_base_   = static_cast<std::byte*>(reserved);
        win_reg_size_   = res_sz;
        win_reg_commit_ = 0;
        owns_region     = true;
        region_size     = res_sz;
    }
    void* committed = VirtualAlloc(win_reg_base_ + win_reg_commit_, cap, MEM_COMMIT, PAGE_READWRITE);
    if (committed == nullptr) {
        if (owns_region) {
            (void)VirtualFree(win_reg_base_, 0, MEM_RELEASE);
            win_reg_base_   = nullptr;
            win_reg_size_   = 0;
            win_reg_commit_ = 0;
        }
        return false;
    }
    raw = static_cast<std::byte*>(committed);
    win_reg_commit_ += cap;
#else
    raw = os_map_pages(cap);
    if (raw == nullptr) {
        return false;
    }
    owns_region = true;
#endif

    auto* blk = new (raw) ArenaBlock{
        .next        = nullptr,
        .base        = raw,
        .capacity    = cap,
        .used        = kHeaderReserve,
        .owns_region = owns_region,
        .region_size = region_size,
    };

    if (head_ == nullptr) {
        head_    = blk;
        current_ = blk;
    } else {
        // Append at the tail of the block chain
        ArenaBlock* tail = current_;
        while (tail->next != nullptr) {
            tail = tail->next;
        }
        tail->next = blk;
        current_   = blk;
    }

    total_reserved_ += cap;
    ++block_count_;
    if (next_block_cap_ < kMaxBlockBytes) {
        next_block_cap_ = (std::min)(next_block_cap_ * 2, kMaxBlockBytes);
    }
    return true;
}

void* ArenaAllocator::alloc(size_t bytes, size_t alignment) noexcept {
    if (bytes == 0) {
        return nullptr;
    }
    const size_t eff_align = (alignment == 0) ? 1 : alignment;

    // Fast path: current block or any rewound blocks following current_ (from TempArena::end_temp)
    ArenaBlock* blk = (current_ != nullptr) ? current_ : head_;
    while (blk != nullptr) {
        const size_t aligned = align_up(blk->used, eff_align);
        if (aligned <= blk->capacity && bytes <= blk->capacity - aligned) {
            current_       = blk;
            void* ptr      = blk->base + aligned;
            blk->used      = aligned + bytes;
            total_used_   += bytes;
            return ptr;
        }
        blk = blk->next;
    }

    if (!grow(bytes, eff_align)) {
        return nullptr;
    }

    const size_t aligned = align_up(current_->used, eff_align);
    void* ptr = current_->base + aligned;
    current_->used = aligned + bytes;
    total_used_ += bytes;
    return ptr;
}

TempArena ArenaAllocator::begin_temp() noexcept {
    return TempArena{
        .arena       = this,
        .saved_block = current_,
        .saved_used  = (current_ != nullptr) ? current_->used : kHeaderReserve,
        .saved_total = total_used_,
    };
}

void ArenaAllocator::end_temp(TempArena temp) noexcept {
    if (temp.arena != this) {
        return;
    }
    if (temp.saved_block == nullptr) {
        // Rewind all blocks without unmapping OS pages so subsequent scratch allocations reuse them in O(1)
        for (ArenaBlock* b = head_; b != nullptr; b = b->next) {
            b->used = kHeaderReserve;
        }
        current_    = head_;
        total_used_ = 0;
        return;
    }

    temp.saved_block->used = temp.saved_used;
    for (ArenaBlock* b = temp.saved_block->next; b != nullptr; b = b->next) {
        b->used = kHeaderReserve;
    }
    current_    = temp.saved_block;
    total_used_ = temp.saved_total;
}

void ArenaAllocator::reset() noexcept {
    // Collect region owners before unmapping so we never read a freed block's next pointer.
    PendingUnmap regions[64]{};
    size_t       region_count = 0;
    const size_t reserved_snapshot = total_reserved_;

    ArenaBlock* blk = head_;
    while (blk != nullptr) {
        ArenaBlock* next = blk->next;
        if (blk->owns_region) {
            if (region_count < 64u) {
                regions[region_count] = PendingUnmap{blk->base, blk->region_size};
                ++region_count;
            } else {
                os_unmap_region(blk->base, blk->region_size);
            }
        }
        blk = next;
    }

    head_           = nullptr;
    current_        = nullptr;
    win_reg_base_   = nullptr;
    win_reg_size_   = 0;
    win_reg_commit_ = 0;
    next_block_cap_ = kMinBlockBytes;
    total_reserved_ = 0;
    total_used_     = 0;
    block_count_    = 0;

    if (region_count == 0) {
        return;
    }
    if (reserved_snapshot >= kAsyncUnmapThresholdBytes) {
        global_os_reaper().enqueue(regions, region_count);
    } else {
        for (size_t i = 0; i < region_count; ++i) {
            os_unmap_region(regions[i].base, regions[i].bytes);
        }
    }
}

} // namespace capybaradb::core
