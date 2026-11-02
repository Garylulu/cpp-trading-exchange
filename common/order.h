#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace Common {
/// Which side of the book an order is on.
enum class Side : uint8_t { BUY, SELL };

/// LIMIT orders trade only at their price or better; MARKET orders trade at the
/// best available price and ignore the price field.
enum class OrderType : uint8_t { LIMIT, MARKET };

inline auto toString(Side side) noexcept -> std::string_view {
    switch (side) {
        case Side::BUY:
            return "BUY";
        case Side::SELL:
            return "SELL";
    }
    return "UNKNOWN";
}

inline auto toString(OrderType type) noexcept -> std::string_view {
    switch (type) {
        case OrderType::LIMIT:
            return "LIMIT";
        case OrderType::MARKET:
            return "MARKET";
    }
    return "UNKNOWN";
}

/// A single order submitted by a client.
///
/// Price is stored as an integer number of cents (e.g. $100.00 is 10000) so
/// prices compare and add exactly, with no floating-point rounding.
struct Order {
    std::string ticker_;  // e.g. "AAPL"; short tickers fit in the string's inline buffer, so no heap allocation
    uint32_t size_ = 0;   // number of shares, e.g. 100
    Side side_ = Side::BUY;
    OrderType type_ = OrderType::LIMIT;
    int64_t price_ = 0;       // in cents, e.g. 10000 for $100.00; unused for MARKET orders
    uint32_t client_id_ = 0;  // e.g. 1234
    uint64_t order_id_ = 0;   // e.g. 567890

    Order() = default;
    Order(std::string ticker, uint32_t size, Side side, OrderType type, int64_t price, uint32_t client_id,
          uint64_t order_id)
        : ticker_(std::move(ticker)),
          size_(size),
          side_(side),
          type_(type),
          price_(price),
          client_id_(client_id),
          order_id_(order_id) {}

    /// e.g. "Order[id:567890 client:1234 AAPL BUY 100 LIMIT @ $100.00]".
    /// MARKET orders print "@ MKT" instead of a price.
    auto toString() const -> std::string {
        const std::string price =
            type_ == OrderType::MARKET
                ? std::string("MKT")
                : std::format("{}${}.{:02}", price_ < 0 ? "-" : "", (price_ < 0 ? -price_ : price_) / 100,
                              (price_ < 0 ? -price_ : price_) % 100);
        return std::format("Order[id:{} client:{} {} {} {} {} @ {}]", order_id_, client_id_, ticker_,
                           Common::toString(side_), size_, Common::toString(type_), price);
    }
};
}  // namespace Common
