#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "common/locked_queue.h"
#include "common/order.h"

namespace Exchange {
/// What happened to the book.
enum class MarketUpdateType : uint8_t { ADD, CANCEL };

inline auto toString(MarketUpdateType type) noexcept -> std::string_view {
    switch (type) {
        case MarketUpdateType::ADD:
            return "ADD";
        case MarketUpdateType::CANCEL:
            return "CANCEL";
    }
    return "UNKNOWN";
}

/// One message on the incremental feed: a single change to the book.
///
/// Every update carries the next sequence number (1, 2, 3, ...), so a client
/// can tell which updates a snapshot already includes and notice a gap.
struct MarketUpdate {
    uint64_t seq_ = 0;
    MarketUpdateType type_ = MarketUpdateType::ADD;
    Common::Order order_;  // ADD: the full order. CANCEL: only order_id_ is used.

    /// e.g. "#7 ADD Order[id:3 client:1 AAPL BUY 100 LIMIT @ $99.50]" or "#8 CANCEL id:3".
    auto toString() const -> std::string {
        if (type_ == MarketUpdateType::CANCEL) return std::format("#{} CANCEL id:{}", seq_, order_.order_id_);
        return std::format("#{} {} {}", seq_, Exchange::toString(type_), order_.toString());
    }
};

/// The full state of the book as of update seq_: every live order, listed so
/// that adding them in order to an empty book rebuilds it with the same
/// priorities (see OrderBook::orders()). An empty book has seq_ 0 if nothing
/// has been published yet.
struct BookSnapshot {
    uint64_t seq_ = 0;
    std::vector<Common::Order> orders_;
};

/// Sends each update to every subscriber's queue, numbering updates as it goes.
///
/// publish() is called only by the matching engine thread. subscribe() and
/// unsubscribe() may be called from any thread, so a mutex guards the
/// subscriber list.
class MarketDataPublisher final {
public:
    using FeedQueue = Common::LockedQueue<MarketUpdate>;

    MarketDataPublisher() = default;

    /// Start sending updates to queue. It receives every update published after
    /// this call returns.
    void subscribe(FeedQueue *queue) {
        std::lock_guard lock(mutex_);
        subscribers_.push_back(queue);
    }

    /// Stop sending updates to queue. Call this before destroying the queue.
    void unsubscribe(FeedQueue *queue) {
        std::lock_guard lock(mutex_);
        std::erase(subscribers_, queue);
    }

    /// Number the update, send it to every subscriber, and return its number.
    auto publish(MarketUpdateType type, const Common::Order &order) -> uint64_t {
        std::lock_guard lock(mutex_);
        const MarketUpdate update{++last_seq_, type, order};
        for (FeedQueue *queue : subscribers_) queue->push(update);
        return last_seq_;
    }

    /// The number of the most recent update, or 0 if none has been published.
    auto lastSeq() const -> uint64_t {
        std::lock_guard lock(mutex_);
        return last_seq_;
    }

    // Deleted special members: subscribers hold pointers into the feed they
    // registered with; copying would split it in two.
    MarketDataPublisher(const MarketDataPublisher &) = delete("a MarketDataPublisher owns its subscriber list");
    MarketDataPublisher(const MarketDataPublisher &&) = delete("a MarketDataPublisher owns its subscriber list");
    MarketDataPublisher &operator=(const MarketDataPublisher &) =
        delete("a MarketDataPublisher owns its subscriber list");
    MarketDataPublisher &operator=(const MarketDataPublisher &&) =
        delete("a MarketDataPublisher owns its subscriber list");

private:
    mutable std::mutex mutex_;
    std::vector<FeedQueue *> subscribers_;
    uint64_t last_seq_ = 0;
};
}  // namespace Exchange
