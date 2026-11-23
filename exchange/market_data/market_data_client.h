#pragma once

#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "exchange/market_data/market_data.h"

namespace Exchange {
/// A client's own copy of the exchange's order book, built from market data.
///
/// Book is any order book with add(order), cancel(order_id) and orders(), e.g.
/// either version of Common::OrderBook. The client's copy is the same class as
/// the exchange's, so applying the same changes gives the same book.
///
/// A client joining mid-day catches up in four steps:
///   1. Subscribe to the incremental feed and pass every update to onUpdate().
///      Until a snapshot arrives the updates are only buffered.
///   2. Request a snapshot and pass it to onSnapshot(), which loads it as the
///      initial book. It reflects every update up to its sequence number S.
///   3. onSnapshot() then replays the buffered updates, discarding any <= S,
///      since the snapshot already includes them.
///   4. The client is now live: onUpdate() applies each update as it arrives.
template <typename Book>
class MarketDataClient final {
public:
    /// Construct the client's empty book from book_args, e.g. the same
    /// ticker, capacity and price range as the exchange's book.
    template <typename... BookArgs>
    explicit MarketDataClient(BookArgs &&...book_args) : book_(std::forward<BookArgs>(book_args)...) {}

    /// Steps 1 and 4: buffer the update until the snapshot is loaded, then
    /// apply it straight to the book.
    void onUpdate(const MarketUpdate &update) {
        if (!live_) {
            buffered_.push_back(update);
            return;
        }
        apply(update);
    }

    /// Steps 2 and 3: load snapshot as the initial book, replay the buffered
    /// updates that came after it, and go live.
    void onSnapshot(const BookSnapshot &snapshot) {
        assert(!live_ && "MarketDataClient::onSnapshot: snapshot already loaded");

        for (const Common::Order &order : snapshot.orders_) book_.add(order);
        last_seq_ = snapshot.seq_;

        for (const MarketUpdate &update : buffered_) {
            if (update.seq_ > last_seq_) apply(update);
        }
        buffered_.clear();
        live_ = true;
    }

    /// Whether the snapshot has been loaded and updates are applied live.
    auto isLive() const noexcept -> bool { return live_; }

    /// The number of the last update reflected in the book.
    auto lastSeq() const noexcept -> uint64_t { return last_seq_; }

    /// Updates received before the snapshot that have not been applied yet.
    auto bufferedCount() const noexcept -> std::size_t { return buffered_.size(); }

    auto book() const noexcept -> const Book & { return book_; }

    // Deleted special members: the book owns its order pool and cannot be copied.
    MarketDataClient(const MarketDataClient &) = delete("a MarketDataClient owns its book");
    MarketDataClient(const MarketDataClient &&) = delete("a MarketDataClient owns its book");
    MarketDataClient &operator=(const MarketDataClient &) = delete("a MarketDataClient owns its book");
    MarketDataClient &operator=(const MarketDataClient &&) = delete("a MarketDataClient owns its book");

private:
    /// Apply one update to the book. Updates the book already reflects are
    /// skipped. A jump in sequence numbers means updates were lost and the book
    /// may be wrong, so it is reported.
    void apply(const MarketUpdate &update) {
        if (update.seq_ <= last_seq_) return;
        if (update.seq_ != last_seq_ + 1) {
            std::fprintf(stderr, "MarketDataClient: gap in feed, expected #%" PRIu64 " but got #%" PRIu64 "\n",
                         last_seq_ + 1, update.seq_);
        }
        last_seq_ = update.seq_;

        switch (update.type_) {
            case MarketUpdateType::ADD:
                book_.add(update.order_);
                break;
            case MarketUpdateType::CANCEL:
                book_.cancel(update.order_.order_id_);
                break;
        }
    }

    Book book_;
    std::vector<MarketUpdate> buffered_;  // updates received before the snapshot
    uint64_t last_seq_ = 0;
    bool live_ = false;
};
}  // namespace Exchange
