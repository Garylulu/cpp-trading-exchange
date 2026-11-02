#pragma once

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

template <typename T>
class MemPool final {
public:
    /// Pre-allocate num_elems blocks up front, each holding a default-constructed
    /// T and marked free. No further heap allocation happens after this.
    explicit MemPool(std::size_t num_elems) : store_(num_elems, T()), is_free_(num_elems, true) {
        assert(num_elems > 0 && "MemPool must have a non-zero capacity");
    }

    /// The vectors own every object and flag and destroy each T when they are
    /// released, so there is nothing else to free here.
    ~MemPool() = default;

    /// Construct a T from args in the first free block and return a pointer to
    /// it. If every block is occupied, nothing is stored, an error is printed,
    /// and nullptr is returned.
    template <typename... Args>
    T *allocate(Args &&...args) noexcept {
        for (std::size_t i = 0; i < is_free_.size(); ++i) {
            if (is_free_[i]) {
                // The block always holds a live T, so end its lifetime before
                // building the new one in its place.
                T *object = &store_[i];
                std::destroy_at(object);
                std::construct_at(object, std::forward<Args>(args)...);
                is_free_[i] = false;
                return object;
            }
        }

        std::fprintf(stderr, "MemPool::allocate: pool exhausted (%zu blocks in use)\n", store_.size());
        return nullptr;
    }

    /// Return the block holding elem to the pool. The stored object is replaced
    /// with a default-constructed T, releasing whatever it held, and the block
    /// is marked free for the next allocate().
    void deallocate(const T *elem) noexcept {
        // Objects are contiguous, so elem's offset from the start is its index
        // into both vectors.
        const auto index = elem - store_.data();
        assert(index >= 0 && static_cast<std::size_t>(index) < store_.size() &&
               "MemPool::deallocate: element does not belong to this pool");

        const auto i = static_cast<std::size_t>(index);
        assert(!is_free_[i] && "MemPool::deallocate: double free");

        // Keep the invariant that every block holds a live T.
        T *object = &store_[i];
        std::destroy_at(object);
        std::construct_at(object);
        is_free_[i] = true;
    }

    // Deleted special members: the pool owns its backing storage and the free
    // list; copying would alias both.
    MemPool() = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &&) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &&) = delete("a MemPool owns its backing storage and free list");

private:
    /// The stored objects and their free flags live in parallel vectors: the
    /// flag at index i says whether store_[i] is free to use.
    std::vector<T> store_;
    std::vector<bool> is_free_;
};
