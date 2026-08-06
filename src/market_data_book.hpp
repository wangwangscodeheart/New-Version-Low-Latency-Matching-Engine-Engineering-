#ifndef MARKET_DATA_BOOK_HPP
#define MARKET_DATA_BOOK_HPP

#include "feed_decoder.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <stdexcept>

struct MarketDataOrder { Price price; Quantity remaining; Side side; };
struct MarketDataSnapshotOrder {
    uint64_t order_id;
    Price price;
    Quantity remaining;
    Side side;
    bool operator==(const MarketDataSnapshotOrder& other) const noexcept {
        return order_id == other.order_id && price == other.price &&
               remaining == other.remaining && side == other.side;
    }
};
struct MarketDataBookState {
    std::vector<MarketDataSnapshotOrder> orders;
    bool operator==(const MarketDataBookState& other) const noexcept {
        return orders == other.orders;
    }
};

class MarketDataBook {
    std::unordered_map<uint64_t, MarketDataOrder> orders_;
    std::map<int64_t, uint64_t> bids_;
    std::map<int64_t, uint64_t> asks_;

    std::map<int64_t, uint64_t>& levels(Side side) noexcept {
        return side == Side::BUY ? bids_ : asks_;
    }
public:
    bool apply(const FeedAddOrder& add) {
        if (add.order_id == 0 || add.price.get() <= 0 || add.quantity.get() == 0 ||
            orders_.find(add.order_id) != orders_.end()) return false;
        orders_.emplace(add.order_id, MarketDataOrder{add.price, add.quantity, add.side});
        levels(add.side)[add.price.get()] += add.quantity.get();
        return true;
    }
    bool apply(const FeedCancelOrder& cancel) {
        const auto found = orders_.find(cancel.order_id);
        if (found == orders_.end()) return false;
        auto& side_levels = levels(found->second.side);
        auto level = side_levels.find(found->second.price.get());
        level->second -= found->second.remaining.get();
        if (level->second == 0) side_levels.erase(level);
        orders_.erase(found);
        return true;
    }
    bool apply(const FeedTrade& trade) noexcept {
        return trade.trade_id != 0 && trade.price.get() > 0 && trade.quantity.get() != 0;
    }
    bool apply(const FeedPayload& payload) {
        return std::visit([this](const auto& value) { return apply(value); }, payload);
    }
    size_t order_count() const noexcept { return orders_.size(); }
    uint64_t bid_quantity(Price price) const noexcept {
        const auto found = bids_.find(price.get());
        return found == bids_.end() ? 0 : found->second;
    }
    uint64_t ask_quantity(Price price) const noexcept {
        const auto found = asks_.find(price.get());
        return found == asks_.end() ? 0 : found->second;
    }
    MarketDataBookState capture_state() const {
        MarketDataBookState state;
        state.orders.reserve(orders_.size());
        for (const auto& entry : orders_) {
            state.orders.push_back(MarketDataSnapshotOrder{
                entry.first, entry.second.price, entry.second.remaining,
                entry.second.side});
        }
        std::sort(state.orders.begin(), state.orders.end(),
            [](const auto& left, const auto& right) {
                return left.order_id < right.order_id;
            });
        return state;
    }
    static MarketDataBook restore(const MarketDataBookState& state) {
        MarketDataBook book;
        for (const auto& order : state.orders) {
            if (!book.apply(FeedAddOrder{
                    order.order_id, order.price, order.remaining, order.side})) {
                throw std::runtime_error("invalid market data book checkpoint");
            }
        }
        return book;
    }
};

#endif
