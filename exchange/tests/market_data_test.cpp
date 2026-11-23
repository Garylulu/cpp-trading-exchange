// Market data tests. Built twice (see exchange/tests/BUILD): once against the
// priority queue book, and once with USE_TICK_ARRAY_BOOK against the tick
// array book, to check that market data works with either.

#include <cstdint>
#include <stop_token>
#include <thread>
#include <vector>

#include "common/locked_queue.h"
#include "common/order.h"
#include "exchange/market_data/market_data.h"
#include "exchange/market_data/market_data_client.h"
#include "exchange/matcher/matching_engine.h"
#include "gtest/gtest.h"

#ifdef USE_TICK_ARRAY_BOOK
#include "common/order_book_ta.h"
#define BOOK_ARGS "AAPL", 100, 9000, 11000, 5
#else
#include "common/order_book.h"
#define BOOK_ARGS "AAPL", 100
#endif

namespace {
using Common::Order;
using Common::OrderBook;
using Common::OrderType;
using Common::Side;
using Exchange::BookSnapshot;
using Exchange::MarketDataClient;
using Exchange::MarketDataPublisher;
using Exchange::MarketUpdate;
using Exchange::MarketUpdateType;

auto limit(uint64_t order_id, Side side, int64_t price, uint32_t size = 100) -> Order {
    return Order("AAPL", size, side, OrderType::LIMIT, price, 1, order_id);
}

/// A stand-in for the exchange without threads: changes its book and publishes
/// each change, the way MatchingEngine does.
struct FakeExchange {
    OrderBook book{BOOK_ARGS};
    MarketDataPublisher publisher;

    void add(const Order &order) {
        ASSERT_TRUE(book.add(order));
        publisher.publish(MarketUpdateType::ADD, order);
    }
    void cancel(uint64_t order_id) {
        ASSERT_TRUE(book.cancel(order_id));
        Order order;
        order.order_id_ = order_id;
        publisher.publish(MarketUpdateType::CANCEL, order);
    }
    auto snapshot() const -> BookSnapshot { return {publisher.lastSeq(), book.orders()}; }
};

/// Pass every update waiting in feed to client.
void drain(MarketDataPublisher::FeedQueue &feed, MarketDataClient<OrderBook> &client) {
    while (auto update = feed.tryPop()) client.onUpdate(*update);
}

TEST(MarketDataPublisher, NumbersUpdatesAndSendsThemToSubscribers) {
    MarketDataPublisher publisher;
    MarketDataPublisher::FeedQueue first;
    MarketDataPublisher::FeedQueue second;
    publisher.subscribe(&first);
    publisher.subscribe(&second);

    EXPECT_EQ(publisher.publish(MarketUpdateType::ADD, limit(1, Side::BUY, 9900)), 1u);
    publisher.unsubscribe(&second);
    EXPECT_EQ(publisher.publish(MarketUpdateType::ADD, limit(2, Side::BUY, 9900)), 2u);
    EXPECT_EQ(publisher.lastSeq(), 2u);

    EXPECT_EQ(first.tryPop()->seq_, 1u);
    EXPECT_EQ(first.tryPop()->seq_, 2u);
    EXPECT_FALSE(first.tryPop().has_value());
    EXPECT_EQ(second.tryPop()->order_.order_id_, 1u);
    EXPECT_FALSE(second.tryPop().has_value());  // unsubscribed before update 2
}

TEST(MarketDataClient, BuffersUpdatesUntilTheSnapshotArrives) {
    FakeExchange exchange;
    MarketDataClient<OrderBook> client(BOOK_ARGS);
    MarketDataPublisher::FeedQueue feed;
    exchange.publisher.subscribe(&feed);

    exchange.add(limit(1, Side::BUY, 9900));
    drain(feed, client);

    EXPECT_FALSE(client.isLive());
    EXPECT_EQ(client.bufferedCount(), 1u);
    EXPECT_TRUE(client.book().orders().empty());
}

TEST(MarketDataClient, BootstrapsMidDayAndMatchesTheExchange) {
    FakeExchange exchange;
    exchange.add(limit(1, Side::BUY, 9900));  // #1, before the client joins
    exchange.add(limit(2, Side::SELL, 10100));  // #2

    // Step 1: the client subscribes and starts buffering.
    MarketDataClient<OrderBook> client(BOOK_ARGS);
    MarketDataPublisher::FeedQueue feed;
    exchange.publisher.subscribe(&feed);
    exchange.add(limit(3, Side::BUY, 9950));  // #3, buffered, and in the snapshot
    exchange.cancel(1);                       // #4, buffered, and in the snapshot

    // Step 2: the snapshot is taken at #4, but more updates happen before it arrives.
    const BookSnapshot snapshot = exchange.snapshot();
    EXPECT_EQ(snapshot.seq_, 4u);
    exchange.add(limit(4, Side::SELL, 10050));  // #5, buffered, not in the snapshot
    exchange.cancel(2);                         // #6, buffered, not in the snapshot
    drain(feed, client);
    EXPECT_EQ(client.bufferedCount(), 4u);

    // Step 3: load the snapshot and replay only #5 and #6. Replaying #3 or #4
    // would fail: order 3 would be added twice and order 1 is already gone.
    client.onSnapshot(snapshot);
    EXPECT_TRUE(client.isLive());
    EXPECT_EQ(client.lastSeq(), 6u);
    EXPECT_EQ(client.bufferedCount(), 0u);
    EXPECT_EQ(client.book().toString(), exchange.book.toString());

    // Step 4: live updates go straight to the book.
    exchange.add(limit(5, Side::BUY, 9950));  // #7
    exchange.cancel(3);                       // #8
    drain(feed, client);
    EXPECT_EQ(client.lastSeq(), 8u);
    EXPECT_EQ(client.book().toString(), exchange.book.toString());
}

TEST(MarketDataClient, SnapshotKeepsTimePriorityWithinAPrice) {
    FakeExchange exchange;
    exchange.add(limit(7, Side::BUY, 9900, 100));
    exchange.add(limit(3, Side::BUY, 9900, 200));  // same price, arrived second
    exchange.add(limit(5, Side::BUY, 9950, 300));

    MarketDataClient<OrderBook> client(BOOK_ARGS);
    client.onSnapshot(exchange.snapshot());

    const std::vector<Order> orders = client.book().orders();
    ASSERT_EQ(orders.size(), 3u);
    EXPECT_EQ(orders[0].order_id_, 5u);  // best price
    EXPECT_EQ(orders[1].order_id_, 7u);  // then the older order at 99.00
    EXPECT_EQ(orders[2].order_id_, 3u);
}

TEST(MarketDataClient, IgnoresUpdatesItAlreadyHas) {
    FakeExchange exchange;
    exchange.add(limit(1, Side::BUY, 9900));
    MarketDataClient<OrderBook> client(BOOK_ARGS);
    client.onSnapshot(exchange.snapshot());

    // A repeat of #1, which the snapshot already includes.
    client.onUpdate(MarketUpdate{1, MarketUpdateType::CANCEL, limit(1, Side::BUY, 9900)});

    EXPECT_EQ(client.lastSeq(), 1u);
    EXPECT_EQ(client.book().orders().size(), 1u);
}

TEST(MatchingEngine, PublishesOnlyChangesTheBookAccepted) {
    Exchange::MatchingEngine<OrderBook> engine(BOOK_ARGS);
    MarketDataPublisher::FeedQueue feed;
    engine.marketData().subscribe(&feed);
    Common::LockedQueue<BookSnapshot> snapshots;
    {
        std::jthread engine_thread([&](std::stop_token stop) { engine.run(stop); });
        engine.add(limit(1, Side::BUY, 9900));
        engine.add(limit(1, Side::SELL, 10100));  // rejected: duplicate id
        engine.cancel(42);                        // rejected: no such order
        engine.cancel(1);
        engine.requestSnapshot(snapshots);

        const auto snapshot = snapshots.pop(std::stop_token{});  // waits for the engine
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->seq_, 2u);
        EXPECT_TRUE(snapshot->orders_.empty());
    }  // the jthread stops and joins the engine here

    EXPECT_EQ(feed.tryPop()->type_, MarketUpdateType::ADD);
    EXPECT_EQ(feed.tryPop()->type_, MarketUpdateType::CANCEL);
    EXPECT_FALSE(feed.tryPop().has_value());
}
}  // namespace
