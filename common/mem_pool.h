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
    explicit MemPool(std::size_t num_elems) : store_(num_elems, {T(), true}) {
        assert(num_elems > 0 && "MemPool must have a non-zero capacity");
    }

    /// The vector owns every block and destroys each T when it is released, so
    /// there is nothing else to free here.
    ~MemPool() = default;

    /// Construct a T from args in the first free block and return a pointer to
    /// it. If every block is occupied, nothing is stored, an error is printed,
    /// and nullptr is returned.
    template <typename... Args>
    T *allocate(Args &&...args) noexcept {
        for (auto &block : store_) {
            if (block.is_free_) {
                // The block always holds a live T, so end its lifetime before
                // building the new one in its place.
                std::destroy_at(&block.object_);
                std::construct_at(&block.object_, std::forward<Args>(args)...);
                block.is_free_ = false;
                return &block.object_;
            }
        }

        std::fprintf(stderr, "MemPool::allocate: pool exhausted (%zu blocks in use)\n", store_.size());
        return nullptr;
    }

    void deallocate(const T *elem) noexcept; // TODO

    // Deleted special members: the pool owns its backing storage and the free
    // list; copying would alias both.
    MemPool() = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool(const MemPool &&) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &) = delete("a MemPool owns its backing storage and free list");
    MemPool &operator=(const MemPool &&) = delete("a MemPool owns its backing storage and free list");

private:
    /// One slot of the pool: the stored object plus whether it is free to use.
    /// object_ is the first member so a T* handed out by allocate() can be
    /// mapped back to its block.
    struct ObjectBlock {
        T object_;
        bool is_free_ = true;
    };

    std::vector<ObjectBlock> store_;
};
