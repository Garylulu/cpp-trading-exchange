#pragma once

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

template <typename T>
class MemPool final {
public:
    /// Pre-allocate num_elems blocks up front, each holding a default-constructed
    /// T, and chain them all into the free list. No further heap allocation
    /// happens after this.
    explicit MemPool(std::size_t num_elems) : store_(num_elems, T()), next_free_(num_elems) {
        assert(num_elems > 0 && "MemPool must have a non-zero capacity");
        for (std::size_t i = 0; i < num_elems; ++i) {
            next_free_[i] = i + 1;
        }
        next_free_[num_elems - 1] = kEnd;
        free_head_ = 0;
    }

    /// The vectors own every object and link and destroy each T when they are
    /// released, so there is nothing else to free here.
    ~MemPool() = default;

    /// Construct a T from args in the block at the head of the free list and
    /// return a pointer to it. If every block is occupied, nothing is stored,
    /// an error is printed, and nullptr is returned.
    template <typename... Args>
    T *allocate(Args &&...args) noexcept {
        if (free_head_ == kEnd) {
            std::fprintf(stderr, "MemPool::allocate: pool exhausted (%zu blocks in use)\n", store_.size());
            return nullptr;
        }

        // Pop the head of the free list.
        const std::size_t i = free_head_;
        free_head_ = next_free_[i];
        next_free_[i] = kInUse;

        // The block always holds a live T, so end its lifetime before building
        // the new one in its place.
        T *object = &store_[i];
        std::destroy_at(object);
        std::construct_at(object, std::forward<Args>(args)...);
        return object;
    }

    /// Return the block holding elem to the pool. The stored object is replaced
    /// with a default-constructed T, releasing whatever it held, and the block
    /// is pushed onto the free list for the next allocate().
    void deallocate(const T *elem) noexcept {
        // Objects are contiguous, so elem's offset from the start is its index
        // into both vectors.
        const auto index = elem - store_.data();
        assert(index >= 0 && static_cast<std::size_t>(index) < store_.size() &&
               "MemPool::deallocate: element does not belong to this pool");

        const auto i = static_cast<std::size_t>(index);
        assert(next_free_[i] == kInUse && "MemPool::deallocate: double free");

        // Keep the invariant that every block holds a live T.
        T *object = &store_[i];
        std::destroy_at(object);
        std::construct_at(object);

        // Push the block onto the head of the free list.
        next_free_[i] = free_head_;
        free_head_ = i;
    }

    // Deleted special members: the pool owns its backing storage and the free
    // list; copying would alias both.
    MemPool() = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &&) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &&) = delete("a MemPool owns its backing storage and free list");

private:
    /// Marks the end of the free list.
    static constexpr std::size_t kEnd = std::numeric_limits<std::size_t>::max();
    /// Marks a block that is allocated, i.e. not on the free list.
    static constexpr std::size_t kInUse = kEnd - 1;

    /// The stored objects, and an index-linked free list over them: for a free
    /// block i, next_free_[i] is the index of the next free block (or kEnd);
    /// for an allocated block it is kInUse. free_head_ is the first free block.
    std::vector<T> store_;
    std::vector<std::size_t> next_free_;
    std::size_t free_head_ = kEnd;
};
