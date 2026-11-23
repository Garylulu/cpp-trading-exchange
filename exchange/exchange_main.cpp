// A small exchange: one matching engine thread owns the order book, trading
// client threads send it orders, and a market data client joins mid-day,
// catches up from a snapshot, and keeps its own copy of the book.
//
// Usage: exchange_main [seconds]
//   Runs until Ctrl+C, or for the given number of seconds. On shutdown it
//   checks that the market data client's book matches the exchange's.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <random>
#include <stop_token>
#include <thread>
#include <vector>

#include "common/locked_queue.h"
#include "common/order.h"
#include "common/order_book_ta.h"  // or "common/order_book.h", with the matching book arguments below
#include "exchange/market_data/market_data.h"
#include "exchange/market_data/market_data_client.h"
#include "exchange/matcher/matching_engine.h"

using namespace std::chrono_literals;
using Common::Order;
using Common::OrderBook;
using Common::OrderType;
using Common::Side;
using Engine = Exchange::MatchingEngine<OrderBook>;

namespace {
// The exchange and every client build their book with the same arguments:
// ticker, capacity, and (for the tick array book) the price range and tick
// size, in cents. With "common/order_book.h", use just the ticker and capacity.
#define BOOK_ARGS "AAPL", 10000, 9000, 11000, 5

constexpr int kNumTraders = 3;
constexpr std::size_t kMaxOrdersPerTrader = 5;

// Set from the signal handler. A handler may only touch async-signal-safe
// state, so it does nothing but flip this atomic flag.
std::atomic<bool> g_running{true};

void handleSigint(int) { g_running = false; }

// Order ids must be unique across all traders, so they come from one shared
// atomic counter.
std::atomic<uint64_t> g_next_order_id{1};

/// A trading client: keeps up to kMaxOrdersPerTrader orders resting, adding a
/// new one or cancelling its oldest every 50ms. Bids are priced below $100.00
/// and asks above it, so the book never crosses (there is no matching yet).
void runTrader(std::stop_token stop, Engine &engine, uint32_t client_id) {
    std::mt19937 rng(client_id);
    std::uniform_int_distribution<int64_t> ticks_away(1, 20);
    std::deque<uint64_t> my_orders;

    while (!stop.stop_requested()) {
        if (my_orders.size() >= kMaxOrdersPerTrader) {
            engine.cancel(my_orders.front());
            my_orders.pop_front();
        } else {
            const Side side = rng() % 2 == 0 ? Side::BUY : Side::SELL;
            const int64_t price = side == Side::BUY ? 10000 - 5 * ticks_away(rng) : 10000 + 5 * ticks_away(rng);
            const uint32_t size = 100 * static_cast<uint32_t>(1 + rng() % 5);
            const uint64_t order_id = g_next_order_id++;
            engine.add(Order("AAPL", size, side, OrderType::LIMIT, price, client_id, order_id));
            my_orders.push_back(order_id);
        }
        std::this_thread::sleep_for(50ms);
    }
}

/// A market data client that joins mid-day: it waits a while, then catches up
/// from a snapshot and follows the feed until stopped.
void runMarketDataClient(std::stop_token stop, Engine &engine, Exchange::MarketDataClient<OrderBook> &client) {
    std::this_thread::sleep_for(1s);  // join after trading has started
    if (stop.stop_requested()) return;

    // Step 1: subscribe to the feed, so no update is missed from here on.
    Exchange::MarketDataPublisher::FeedQueue feed;
    engine.marketData().subscribe(&feed);

    // Step 2: request a snapshot. Until it arrives, feed updates are buffered.
    // A real snapshot request takes a while to reach the exchange; this sleep
    // stands in for that, so some updates arrive before the snapshot.
    std::this_thread::sleep_for(200ms);
    Common::LockedQueue<Exchange::BookSnapshot> snapshots;
    engine.requestSnapshot(snapshots);
    while (!client.isLive() && !stop.stop_requested()) {
        while (auto update = feed.tryPop()) client.onUpdate(*update);
        if (auto snapshot = snapshots.tryPop()) {
            std::cout << "[md client] snapshot at #" << snapshot->seq_ << " with " << snapshot->orders_.size()
                      << " orders, " << client.bufferedCount() << " updates buffered\n";
            // Steps 3 and 4: replay the buffered updates after the snapshot, go live.
            client.onSnapshot(*snapshot);
            std::cout << "[md client] live at #" << client.lastSeq() << "\n";
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    // Live: apply each update as it arrives.
    while (auto update = feed.pop(stop)) client.onUpdate(*update);

    // Stopped. The engine has already stopped, so apply whatever is left.
    while (auto update = feed.tryPop()) client.onUpdate(*update);
    engine.marketData().unsubscribe(&feed);
}
}  // namespace

int main(int argc, char **argv) {
    std::signal(SIGINT, handleSigint);  // catch Ctrl+C
    using Clock = std::chrono::steady_clock;
    const auto deadline = argc > 1 ? Clock::now() + std::chrono::seconds(std::atoi(argv[1])) : Clock::time_point::max();

    Engine engine(BOOK_ARGS);
    Exchange::MarketDataClient<OrderBook> md_client(BOOK_ARGS);

    std::jthread engine_thread([&](std::stop_token stop) { engine.run(stop); });
    std::jthread md_thread([&](std::stop_token stop) { runMarketDataClient(stop, engine, md_client); });
    std::vector<std::jthread> traders;
    for (uint32_t client_id = 1; client_id <= kNumTraders; ++client_id) {
        traders.emplace_back([&engine, client_id](std::stop_token stop) { runTrader(stop, engine, client_id); });
    }
    std::cout << "Exchange running with " << kNumTraders << " traders. Press Ctrl+C to stop.\n";

    while (g_running && Clock::now() < deadline) std::this_thread::sleep_for(50ms);
    std::cout << "\nShutting down...\n";

    // Stop in dependency order: traders first, then the engine, so no more
    // updates are published, then the market data client, so it can drain them.
    for (auto &trader : traders) {
        trader.request_stop();
        trader.join();
    }
    engine_thread.request_stop();
    engine_thread.join();
    md_thread.request_stop();
    md_thread.join();

    std::cout << "Exchange book after " << engine.marketData().lastSeq() << " updates:\n"
              << engine.book().toString();
    if (!md_client.isLive()) {
        std::cout << "Market data client never went live.\n";
        return 0;
    }
    const bool match = md_client.book().toString() == engine.book().toString();
    std::cout << "Market data client is at #" << md_client.lastSeq() << " and its book "
              << (match ? "matches" : "DOES NOT match") << " the exchange's.\n";
    return match ? 0 : 1;
}
