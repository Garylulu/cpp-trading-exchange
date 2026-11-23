#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <utility>

namespace Common {
/// A first-in, first-out queue that any number of threads can push to and pop
/// from. A mutex guards the queue, and a condition variable lets a consumer
/// sleep until an item arrives instead of spinning.
template <typename T>
class LockedQueue final {
public:
    LockedQueue() = default;

    /// Add item to the back of the queue and wake one waiting consumer.
    void push(T item) {
        {
            std::lock_guard lock(mutex_);
            items_.push(std::move(item));
        }
        not_empty_.notify_one();
    }

    /// Block until an item is available and return it. Returns nullopt instead
    /// if stop is requested while waiting, so a std::jthread can shut down.
    auto pop(std::stop_token stop) -> std::optional<T> {
        std::unique_lock lock(mutex_);
        if (!not_empty_.wait(lock, stop, [this] { return !items_.empty(); })) return std::nullopt;
        return takeFront();
    }

    /// Return the front item if there is one, without waiting.
    auto tryPop() -> std::optional<T> {
        std::lock_guard lock(mutex_);
        if (items_.empty()) return std::nullopt;
        return takeFront();
    }

    // Deleted special members: the mutex and condition variable cannot be
    // copied or moved, and waiting threads hold references to them.
    LockedQueue(const LockedQueue &) = delete("a LockedQueue is shared between threads by reference");
    LockedQueue(const LockedQueue &&) = delete("a LockedQueue is shared between threads by reference");
    LockedQueue &operator=(const LockedQueue &) = delete("a LockedQueue is shared between threads by reference");
    LockedQueue &operator=(const LockedQueue &&) = delete("a LockedQueue is shared between threads by reference");

private:
    /// Remove and return the front item. The caller must hold mutex_.
    auto takeFront() -> T {
        T item = std::move(items_.front());
        items_.pop();
        return item;
    }

    std::mutex mutex_;
    std::condition_variable_any not_empty_;  // the _any variant can wait on a std::stop_token
    std::queue<T> items_;
};
}  // namespace Common
