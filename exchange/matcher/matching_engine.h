#pragma once

#include <cstdint>
#include <stop_token>
#include <utility>

#include "common/locked_queue.h"
#include "common/order.h"
#include "exchange/market_data/market_data.h"

namespace Exchange {
/// A request from a client to the matching engine.
struct Request {
    enum class Type : uint8_t { ADD, CANCEL, SNAPSHOT };

    Type type_ = Type::ADD;
    Common::Order order_;  // ADD: the full order. CANCEL: only order_id_ is used.
    Common::LockedQueue<BookSnapshot> *snapshot_reply_ = nullptr;  // SNAPSHOT: where to send the snapshot
};

/// Owns the order book and is the only thread that touches it, so the book
/// needs no lock. Clients on other threads send requests through a locked
/// inbound queue, and every change to the book goes out on the market data
/// feed. There is no matching yet: orders only rest in the book or are
/// cancelled.
///
/// Book is any order book with add(order), cancel(order_id) and orders(), e.g.
/// either version of Common::OrderBook.
template <typename Book>
class MatchingEngine final {
public:
    /// Construct the engine's empty book from book_args.
    template <typename... BookArgs>
    explicit MatchingEngine(BookArgs &&...book_args) : book_(std::forward<BookArgs>(book_args)...) {}

    // Called by client threads.

    void add(const Common::Order &order) { inbound_.push({Request::Type::ADD, order, nullptr}); }

    void cancel(uint64_t order_id) {
        Common::Order order;
        order.order_id_ = order_id;
        inbound_.push({Request::Type::CANCEL, order, nullptr});
    }

    /// Ask for a snapshot of the book. It is pushed to reply once the engine
    /// reaches this request, after every request sent before it.
    void requestSnapshot(Common::LockedQueue<BookSnapshot> &reply) {
        inbound_.push({Request::Type::SNAPSHOT, {}, &reply});
    }

    auto marketData() noexcept -> MarketDataPublisher & { return publisher_; }

    // Called by the engine thread.

    /// Handle requests one at a time until stop is requested, e.g.
    /// std::jthread engine_thread([&](std::stop_token stop) { engine.run(stop); });
    void run(std::stop_token stop) {
        while (auto request = inbound_.pop(stop)) handle(*request);
    }

    /// The engine's book. Only read it once run() has returned.
    auto book() const noexcept -> const Book & { return book_; }

    // Deleted special members: client threads hold references to the engine.
    MatchingEngine(const MatchingEngine &) = delete("a MatchingEngine is shared between threads by reference");
    MatchingEngine(const MatchingEngine &&) = delete("a MatchingEngine is shared between threads by reference");
    MatchingEngine &operator=(const MatchingEngine &) =
        delete("a MatchingEngine is shared between threads by reference");
    MatchingEngine &operator=(const MatchingEngine &&) =
        delete("a MatchingEngine is shared between threads by reference");

private:
    /// Apply one request. Only changes that succeed are published, so the feed
    /// never describes a change the book rejected.
    void handle(const Request &request) {
        switch (request.type_) {
            case Request::Type::ADD:
                if (book_.add(request.order_)) publisher_.publish(MarketUpdateType::ADD, request.order_);
                break;
            case Request::Type::CANCEL:
                if (book_.cancel(request.order_.order_id_)) publisher_.publish(MarketUpdateType::CANCEL, request.order_);
                break;
            case Request::Type::SNAPSHOT:
                // Nothing else changes the book or publishes while this runs,
                // so the orders and the sequence number agree.
                request.snapshot_reply_->push(BookSnapshot{publisher_.lastSeq(), book_.orders()});
                break;
        }
    }

    Book book_;
    MarketDataPublisher publisher_;
    Common::LockedQueue<Request> inbound_;
};
}  // namespace Exchange
