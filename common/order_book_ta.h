#pragma once

#include <cassert>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mem_pool_o.h"
#include "order.h"

namespace Common {
/// A limit order book for a single ticker, laid out as a tick array: each side
/// has one price level per tick between min_price and max_price, so a price
/// maps straight to its level by index. Each level keeps its orders in a FIFO
/// doubly linked list, which gives price-time priority: best price first, and
/// among equal prices the earliest order first.
///
/// The list is intrusive: every order lives in a node from a MemPool sized at
/// construction, and the node carries its own prev/next links. With the id
/// index pointing at nodes, cancel() unlinks an order in O(1) and returns its
/// block to the pool immediately.
class OrderBook final {
public:
    /// Prices are in cents. The book accepts prices in [min_price, max_price]
    /// that are a whole number of tick_size steps above min_price, e.g.
    /// OrderBook("AAPL", 10000, 0, 100000, 1) covers $0.00 to $1000.00 in 1c ticks.
    OrderBook(std::string ticker, std::size_t capacity, int64_t min_price, int64_t max_price, int64_t tick_size)
        : ticker_(std::move(ticker)),
          min_price_(min_price),
          tick_size_(tick_size),
          pool_(capacity),
          bids_(numLevels(min_price, max_price, tick_size)),
          asks_(bids_.size()) {
        live_.reserve(capacity);
    }

    /// The pool owns every order node and releases them when it is destroyed,
    /// so there is nothing else to free here.
    ~OrderBook() = default;

    /// Copy order into the book. Returns false, and leaves the book unchanged,
    /// if the order is for a different ticker, is a MARKET order (there is no
    /// matching yet, and a market order cannot rest), has zero size, has a
    /// price outside the range or off the tick grid, reuses the id of a live
    /// order, or the pool is full.
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
        const std::size_t index = priceToIndex(order.price_);
        if (index == kNone) {
            std::fprintf(stderr, "OrderBook::add: order %" PRIu64 " rejected, price %" PRId64 " is off the tick grid\n",
                         order.order_id_, order.price_);
            return false;
        }
        if (live_.contains(order.order_id_)) {
            std::fprintf(stderr, "OrderBook::add: order id %" PRIu64 " is already in the book\n", order.order_id_);
            return false;
        }

        OrderNode *node = pool_.allocate(order);
        if (node == nullptr) return false;

        // Append to the tail, so the level stays in arrival order.
        Level &level = levels(order.side_)[index];
        node->prev_ = level.tail_;
        if (level.tail_ != nullptr) {
            level.tail_->next_ = node;
        } else {
            level.head_ = node;
        }
        level.tail_ = node;

        // A new order can only improve the best price, never worsen it.
        if (order.side_ == Side::BUY) {
            if (best_bid_ == kNone || index > best_bid_) best_bid_ = index;
        } else {
            if (best_ask_ == kNone || index < best_ask_) best_ask_ = index;
        }

        live_.emplace(order.order_id_, node);
        return true;
    }

    /// Remove the order with order_id from the book and return its block to the
    /// pool. Returns false if no live order has that id.
    auto cancel(uint64_t order_id) noexcept -> bool {
        const auto it = live_.find(order_id);
        if (it == live_.end()) return false;

        OrderNode *node = it->second;
        const Side side = node->order_.side_;
        const std::size_t index = priceToIndex(node->order_.price_);
        Level &level = levels(side)[index];

        // Unlink the node from its level.
        if (node->prev_ != nullptr) {
            node->prev_->next_ = node->next_;
        } else {
            level.head_ = node->next_;
        }
        if (node->next_ != nullptr) {
            node->next_->prev_ = node->prev_;
        } else {
            level.tail_ = node->prev_;
        }

        live_.erase(it);
        pool_.deallocate(node);

        // If that emptied the best level, walk away from the spread to the next
        // level that still has orders.
        if (level.head_ == nullptr) {
            if (side == Side::BUY && index == best_bid_) {
                best_bid_ = kNone;
                for (std::size_t i = index; i-- > 0;) {
                    if (bids_[i].head_ != nullptr) {
                        best_bid_ = i;
                        break;
                    }
                }
            } else if (side == Side::SELL && index == best_ask_) {
                best_ask_ = kNone;
                for (std::size_t i = index + 1; i < asks_.size(); ++i) {
                    if (asks_[i].head_ != nullptr) {
                        best_ask_ = i;
                        break;
                    }
                }
            }
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
        if (best_ask_ != kNone) {
            for (std::size_t i = best_ask_; i < asks_.size(); ++i) appendLevel(out, asks_[i]);
        }
        out += "  BIDS\n";
        if (best_bid_ != kNone) {
            for (std::size_t i = best_bid_ + 1; i-- > 0;) appendLevel(out, bids_[i]);
        }
        return out;
    }

    // Deleted special members: the book owns its pool, and the levels and index
    // point into it; copying would alias them.
    OrderBook() = delete("an OrderBook owns its order pool");
    OrderBook(const OrderBook &) = delete("an OrderBook owns its order pool");
    OrderBook(const OrderBook &&) = delete("an OrderBook owns its order pool");
    OrderBook &operator=(const OrderBook &) = delete("an OrderBook owns its order pool");
    OrderBook &operator=(const OrderBook &&) = delete("an OrderBook owns its order pool");

private:
    /// One pooled order plus its links to the orders before and after it at the
    /// same price level.
    struct OrderNode {
        Order order_;
        OrderNode *prev_ = nullptr;
        OrderNode *next_ = nullptr;

        OrderNode() = default;
        explicit OrderNode(const Order &order) : order_(order) {}
    };

    /// All orders at one price on one side, oldest at the head.
    struct Level {
        OrderNode *head_ = nullptr;
        OrderNode *tail_ = nullptr;
    };

    /// Marks "no index": no best price on an empty side, or a price that is not
    /// on the tick grid.
    static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

    /// The number of ticks in [min_price, max_price]. Checks the range here,
    /// since it must be valid before the level arrays are sized.
    static auto numLevels(int64_t min_price, int64_t max_price, int64_t tick_size) noexcept -> std::size_t {
        assert(tick_size > 0 && "OrderBook tick size must be positive");
        assert(min_price <= max_price && "OrderBook price range is empty");
        assert((max_price - min_price) % tick_size == 0 && "OrderBook price range must be a whole number of ticks");
        return static_cast<std::size_t>((max_price - min_price) / tick_size) + 1;
    }

    /// The level index for price, or kNone if price is out of range or off the
    /// tick grid.
    auto priceToIndex(int64_t price) const noexcept -> std::size_t {
        if (price < min_price_ || (price - min_price_) % tick_size_ != 0) return kNone;
        const auto index = static_cast<std::size_t>((price - min_price_) / tick_size_);
        return index < bids_.size() ? index : kNone;
    }

    auto levels(Side side) noexcept -> std::vector<Level> & { return side == Side::BUY ? bids_ : asks_; }

    /// Append each order at level, oldest first.
    static void appendLevel(std::string &out, const Level &level) {
        for (const OrderNode *node = level.head_; node != nullptr; node = node->next_) {
            out += std::format("    {}\n", node->order_.toString());
        }
    }

    std::string ticker_;
    int64_t min_price_;
    int64_t tick_size_;
    MemPool<OrderNode> pool_;
    std::vector<Level> bids_;  // index i holds the bids at min_price_ + i * tick_size_
    std::vector<Level> asks_;  // index i holds the asks at min_price_ + i * tick_size_
    std::size_t best_bid_ = kNone;  // index of the highest non-empty bid level
    std::size_t best_ask_ = kNone;  // index of the lowest non-empty ask level
    std::unordered_map<uint64_t, OrderNode *> live_;  // order id -> pooled node, for live orders only
};
}  // namespace Common
