#pragma once

#include <cstddef>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <format>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem_pool_o.h"
#include "order.h"

namespace Common {
/// A limit order book for a single ticker. Resting orders are kept in two
/// priority queues, bids and asks, each ordered by price-time priority: best
/// price first, and among equal prices the earliest order first.
///
/// Every order is stored in a block from a MemPool sized at construction, so
/// add() and cancel() do no heap allocation for the orders themselves.
///
/// A priority queue can only remove its top element, so cancel() is lazy: the
/// order is dropped from the id index straight away, and its heap entry is
/// discarded (and its block returned to the pool) once it reaches the top of
/// its queue. Until then the cancelled order still occupies a pool block.
class OrderBook final {
public:
    OrderBook(std::string ticker, std::size_t capacity) : ticker_(std::move(ticker)), pool_(capacity) {
        std::vector<Entry> bid_storage;
        std::vector<Entry> ask_storage;
        bid_storage.reserve(capacity);
        ask_storage.reserve(capacity);
        bids_ = BidQueue(BidPriority(), std::move(bid_storage));
        asks_ = AskQueue(AskPriority(), std::move(ask_storage));
        live_.reserve(capacity);
    }

    /// The pool owns every order block and releases them when it is destroyed,
    /// so there is nothing else to free here.
    ~OrderBook() = default;

    /// Copy order into the book. Returns false, and leaves the book unchanged,
    /// if the order is for a different ticker, is a MARKET order (there is no
    /// matching yet, and a market order cannot rest), has zero size, reuses the
    /// id of a live order, or the pool is full.
    auto add(const Order &order) noexcept -> bool {
        if (order.ticker_ != ticker_) {
            std::fprintf(stderr, "OrderBook::add: order %" PRIu64 " is for %s, not %s\n", order.order_id_,
                         order.ticker_.c_str(), ticker_.c_str());
            return false;
        }
        if (order.type_ != OrderType::LIMIT) {
            std::fprintf(stderr, "OrderBook::add: order %" PRIu64 " rejected, only LIMIT orders can rest\n",
                         order.order_id_);
            return false;
        }
        if (order.size_ == 0) {
            std::fprintf(stderr, "OrderBook::add: order %" PRIu64 " rejected, size is zero\n", order.order_id_);
            return false;
        }
        if (live_.contains(order.order_id_)) {
            std::fprintf(stderr, "OrderBook::add: order id %" PRIu64 " is already in the book\n", order.order_id_);
            return false;
        }

        Order *stored = pool_.allocate(order);
        if (stored == nullptr) return false;

        const Entry entry{stored, next_seq_++};
        if (stored->side_ == Side::BUY) {
            bids_.push(entry);
        } else {
            asks_.push(entry);
        }
        live_.emplace(stored->order_id_, stored);
        return true;
    }

    /// Remove the order with order_id from the book. Returns false if no live
    /// order has that id.
    auto cancel(uint64_t order_id) noexcept -> bool {
        const auto it = live_.find(order_id);
        if (it == live_.end()) return false;

        const Side side = it->second->side_;
        live_.erase(it);
        if (side == Side::BUY) {
            discardDeadTop(bids_);
        } else {
            discardDeadTop(asks_);
        }
        return true;
    }

    /// Every live order, asks then bids, each side best price first, e.g.
    ///   OrderBook[AAPL]
    ///     ASKS
    ///       Order[id:2 client:7 AAPL SELL 50 LIMIT @ $101.00]
    ///     BIDS
    ///       Order[id:1 client:9 AAPL BUY 100 LIMIT @ $100.00]
    auto toString() const -> std::string {
        std::string out = std::format("OrderBook[{}]\n", ticker_);
        out += "  ASKS\n";
        appendSide(out, asks_);
        out += "  BIDS\n";
        appendSide(out, bids_);
        return out;
    }

    // Deleted special members: the book owns its pool, and the queues and index
    // point into it; copying would alias them.
    OrderBook() = delete("an OrderBook owns its order pool");
    OrderBook(const OrderBook &) = delete("an OrderBook owns its order pool");
    OrderBook(const OrderBook &&) = delete("an OrderBook owns its order pool");
    OrderBook &operator=(const OrderBook &) = delete("an OrderBook owns its order pool");
    OrderBook &operator=(const OrderBook &&) = delete("an OrderBook owns its order pool");

private:
    /// One heap entry: the pooled order plus its arrival sequence number, which
    /// breaks price ties in favour of the earlier order.
    struct Entry {
        Order *order_;
        uint64_t seq_;
    };

    /// std::priority_queue keeps the "largest" element on top, so each
    /// comparator returns true when a should sit below b.
    struct BidPriority {
        auto operator()(const Entry &a, const Entry &b) const noexcept -> bool {
            if (a.order_->price_ != b.order_->price_) return a.order_->price_ < b.order_->price_;
            return a.seq_ > b.seq_;
        }
    };
    struct AskPriority {
        auto operator()(const Entry &a, const Entry &b) const noexcept -> bool {
            if (a.order_->price_ != b.order_->price_) return a.order_->price_ > b.order_->price_;
            return a.seq_ > b.seq_;
        }
    };

    using BidQueue = std::priority_queue<Entry, std::vector<Entry>, BidPriority>;
    using AskQueue = std::priority_queue<Entry, std::vector<Entry>, AskPriority>;

    /// An entry is live while the index still maps its id to this exact block.
    /// Comparing the pointer, not just the id, means a cancelled order is not
    /// revived by a later order that reuses its id.
    auto isLive(const Entry &entry) const noexcept -> bool {
        const auto it = live_.find(entry.order_->order_id_);
        return it != live_.end() && it->second == entry.order_;
    }

    /// Pop cancelled entries off the top of queue and return their blocks to
    /// the pool. Cancelled entries deeper in the queue are left for later.
    template <typename Queue>
    void discardDeadTop(Queue &queue) noexcept {
        while (!queue.empty() && !isLive(queue.top())) {
            pool_.deallocate(queue.top().order_);
            queue.pop();
        }
    }

    /// Append each live order in queue, in priority order. Works on a copy,
    /// since a priority queue can only be read by popping.
    template <typename Queue>
    void appendSide(std::string &out, Queue queue) const {
        for (; !queue.empty(); queue.pop()) {
            if (isLive(queue.top())) {
                out += std::format("    {}\n", queue.top().order_->toString());
            }
        }
    }

    std::string ticker_;
    MemPool<Order> pool_;
    BidQueue bids_;
    AskQueue asks_;
    std::unordered_map<uint64_t, Order *> live_;  // order id -> pooled order, for live orders only
    uint64_t next_seq_ = 0;
};
}  // namespace Common
