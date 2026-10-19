#pragma once

#include <cassert>
#include <cstddef>
#include <limits>
#include <vector>

template <typename T>
class MemPool final {
public:
    /// Pre-allocate storage for num_elems objects of type T and thread every
    /// slot onto the free list. No T is constructed here; that happens lazily
    /// in allocate() via placement new, so T need not be default-constructible.
    explicit MemPool(std::size_t num_elems) : store_(num_elems) {
        assert(num_elems > 0 && "MemPool must have a non-zero capacity");

        // Each free block points at the next one; the last terminates the list.
        for (std::size_t i = 0; i + 1 < num_elems; ++i) {
            store_[i].next_free_ = i + 1;
        }
        store_.back().next_free_ = kNullIndex;
        free_head_ = 0;
    }

    template <typename... Args>
    T *allocate(Args &&...args) noexcept; // TODO

    void deallocate(const T *elem) noexcept; // TODO

    // Deleted special members: the pool owns its backing storage and the free
    // list; copying would alias both.
    MemPool() = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &&) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &&) = delete("a MemPool owns its backing storage and free list");

private:
    static constexpr std::size_t kNullIndex = std::numeric_limits<std::size_t>::max();

    /// One slot of the pool: raw, suitably aligned storage for a T plus the
    /// bookkeeping for the intrusive free list. storage_ is the first member so
    /// a T* handed out by allocate() can be mapped back to its block.
    struct ObjectBlock {
        alignas(T) std::byte storage_[sizeof(T)];
        std::size_t next_free_ = kNullIndex;
        bool is_free_ = true;
    };

    std::vector<ObjectBlock> store_;
    std::size_t free_head_ = kNullIndex;
};
