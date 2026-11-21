#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace capybaradb::core {

/// Intrusive header placed at the beginning of each OS-mapped page block
/// (VirtualAlloc on Windows, mmap on Linux). Zero heap/malloc overhead.
struct ArenaBlock {
    ArenaBlock* next;        // Intrusive linked list of mapped OS blocks
    std::byte*  base;        // Page-aligned pointer returned by VirtualAlloc / mmap
    size_t      capacity;    // Mapped/committed capacity in bytes
    size_t      used;        // Monotonically increasing bump pointer
    bool        owns_region; // true if base starts an OS reservation to release on reset()
    size_t      region_size; // Total reserved span (Windows MEM_RESERVE) or capacity (Linux mmap)
};

class ArenaAllocator;

/// Temporary memory checkpoint (BeginTemporaryMemory / EndTemporaryMemory).
/// Rewinds an ArenaAllocator bump pointer in O(1) without unmapping already-committed OS pages.
struct TempArena {
    ArenaAllocator* arena        = nullptr;
    ArenaBlock*     saved_block  = nullptr;
    size_t          saved_used   = 0;
    size_t          saved_total  = 0;
};

/// Monotonic, pointer-stable virtual-memory bump allocator.
/// Single-writer; never reallocates or moves previously returned addresses.
/// Supports O(1) scratch checkpoints (begin_temp / end_temp) and O(block_count) OS release (reset).
class ArenaAllocator final {
public:
    static constexpr size_t kMinBlockBytes = 1ull * 1024 * 1024;  // 1 MiB
    static constexpr size_t kMaxBlockBytes = 16ull * 1024 * 1024; // 16 MiB

    ArenaAllocator() noexcept = default;
    ~ArenaAllocator() noexcept;

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;
    ArenaAllocator(ArenaAllocator&& other) noexcept;
    ArenaAllocator& operator=(ArenaAllocator&& other) noexcept;

    /// Linear lock-free bump allocation (single writer). Never invalidates prior pointers.
    [[nodiscard]] void* alloc(size_t bytes, size_t alignment = 8) noexcept;

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] T* alloc_struct() noexcept {
        void* mem = alloc(sizeof(T), alignof(T));
        if (mem == nullptr) {
            return nullptr;
        }
        std::memset(mem, 0, sizeof(T));
        return static_cast<T*>(mem);
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] T* alloc_array(size_t count) noexcept {
        if (count == 0) {
            return nullptr;
        }
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] std::span<T> push_span(size_t count) noexcept {
        T* ptr = alloc_array<T>(count);
        if (ptr == nullptr) {
            return {};
        }
        std::memset(ptr, 0, sizeof(T) * count);
        return std::span<T>(ptr, count);
    }

    /// Reserves a contiguous raw byte buffer (e.g. for TextSlab or UTF-8 copies).
    [[nodiscard]] char* alloc_bytes(size_t count) noexcept {
        if (count == 0) {
            return nullptr;
        }
        return static_cast<char*>(alloc(count, 1));
    }

    /// Copies a UTF-8 string slice into the arena and returns a non-owning string_view into arena memory.
    [[nodiscard]] std::string_view push_string(std::string_view text) noexcept {
        if (text.empty()) {
            return {};
        }
        char* dst = alloc_bytes(text.size());
        if (dst == nullptr) {
            return {};
        }
        std::memcpy(dst, text.data(), text.size());
        return std::string_view(dst, text.size());
    }

    /// Saves a temporary memory checkpoint (BeginTemporaryMemory).
    [[nodiscard]] TempArena begin_temp() noexcept;

    /// Restores the arena bump pointer to a saved checkpoint in O(1), keeping committed blocks hot for reuse.
    void end_temp(TempArena temp) noexcept;

    /// Releases all mapped OS blocks back to the operating system in O(block_count).
    void reset() noexcept;

    [[nodiscard]] size_t   bytes_reserved() const noexcept { return total_reserved_; }
    [[nodiscard]] size_t   bytes_used() const noexcept { return total_used_; }
    [[nodiscard]] uint32_t block_count() const noexcept { return block_count_; }

private:
    bool grow(size_t min_bytes, size_t alignment) noexcept;

    ArenaBlock* head_           = nullptr;
    ArenaBlock* current_        = nullptr;
    std::byte*  win_reg_base_   = nullptr;
    size_t      win_reg_size_   = 0;
    size_t      win_reg_commit_ = 0;
    size_t      next_block_cap_ = kMinBlockBytes;
    size_t      total_reserved_ = 0;
    size_t      total_used_     = 0;
    uint32_t    block_count_    = 0;
};

/// Scoped RAII wrapper around TempArena for scratch allocations within a function.
struct ScopedTempArena {
    TempArena temp;

    explicit ScopedTempArena(ArenaAllocator& arena) noexcept
        : temp(arena.begin_temp()) {}

    ~ScopedTempArena() noexcept {
        if (temp.arena != nullptr) {
            temp.arena->end_temp(temp);
        }
    }

    ScopedTempArena(const ScopedTempArena&) = delete;
    ScopedTempArena& operator=(const ScopedTempArena&) = delete;
};

[[nodiscard]] inline TempArena begin_temp_arena(ArenaAllocator& arena) noexcept {
    return arena.begin_temp();
}

inline void end_temp_arena(TempArena temp) noexcept {
    if (temp.arena != nullptr) {
        temp.arena->end_temp(temp);
    }
}

} // namespace capybaradb::core
