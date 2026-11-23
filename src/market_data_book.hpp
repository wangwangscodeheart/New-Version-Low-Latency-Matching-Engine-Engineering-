#ifndef MARKET_DATA_BOOK_HPP
#define MARKET_DATA_BOOK_HPP

#include "feed_decoder.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <limits>
#include <optional>
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
            (add.side != Side::BUY && add.side != Side::SELL) ||
            orders_.find(add.order_id) != orders_.end()) return false;
        auto& side_levels = levels(add.side);
        auto level = side_levels.find(add.price.get());
        if (level != side_levels.end() &&
            add.quantity.get() > std::numeric_limits<uint64_t>::max() - level->second) {
            return false;
        }

        // Stage the level before inserting the order. If either allocation
        // fails, neither an orphan order nor an empty price level may remain.
        const bool new_level = level == side_levels.end();
        if (new_level) level = side_levels.emplace(add.price.get(), 0).first;
        try {
            orders_.emplace(add.order_id,
                            MarketDataOrder{add.price, add.quantity, add.side});
        } catch (...) {
            if (new_level) side_levels.erase(level);
            throw;
        }
        level->second += add.quantity.get();
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
        if (trade.trade_id == 0 || trade.passive_order_id == 0 ||
            trade.price.get() <= 0 || trade.quantity.get() == 0) return false;
        const auto found = orders_.find(trade.passive_order_id);
        if (found == orders_.end() || found->second.price != trade.price ||
            trade.quantity.get() > found->second.remaining.get()) return false;
        auto& side_levels = levels(found->second.side);
        const auto level = side_levels.find(found->second.price.get());
        if (level == side_levels.end() || level->second < trade.quantity.get()) return false;
        level->second -= trade.quantity.get();
        if (level->second == 0) side_levels.erase(level);
        found->second.remaining = Quantity(
            found->second.remaining.get() - trade.quantity.get());
        if (found->second.remaining.get() == 0) orders_.erase(found);
        return true;
    }
    // Native public-feed lifecycle operations. These mutate the reconstructed
    // visible book only; they are not commands to the private matching engine.
    // The existing simulated FeedPayload contract remains unchanged.
    bool add_order(uint64_t order_id, Price price, Quantity remaining, Side side) {
        return apply(FeedAddOrder{order_id, price, remaining, side});
    }
    bool erase_order(uint64_t order_id) {
        return apply(FeedCancelOrder{order_id});
    }
    bool reduce_order(uint64_t order_id, Quantity reduction) noexcept {
        if (order_id == 0 || reduction.get() == 0) return false;
        const auto found = orders_.find(order_id);
        if (found == orders_.end() || reduction.get() > found->second.remaining.get()) return false;
        auto& side_levels = levels(found->second.side);
        const auto level = side_levels.find(found->second.price.get());
        if (level == side_levels.end() || level->second < reduction.get()) return false;
        // Native X/E/C reduce at the stored resting price. An execution price
        // carried by C must not move the order or select a different level.
        level->second -= reduction.get();
        if (level->second == 0) side_levels.erase(level);
        found->second.remaining = Quantity(found->second.remaining.get() - reduction.get());
        if (found->second.remaining.get() == 0) orders_.erase(found);
        return true;
    }
    bool replace_order(uint64_t old_id, uint64_t new_id, Price new_price, Quantity new_remaining) {
        if (old_id == 0 || new_id == 0 || old_id == new_id ||
            new_price.get() <= 0 || new_remaining.get() == 0) return false;
        const auto found = orders_.find(old_id);
        if (found == orders_.end() || orders_.find(new_id) != orders_.end()) return false;
        // Keep a value copy: inserting the new node may rehash orders_ and
        // invalidate its iterators. Stock is implicit in this per-symbol book;
        // replacement inherits the old side and specifies a new TOTAL size.
        const MarketDataOrder old = found->second;
        auto& side_levels = levels(old.side);
        const auto old_level = side_levels.find(old.price.get());
        if (old_level == side_levels.end() || old_level->second < old.remaining.get()) return false;
        auto target = side_levels.find(new_price.get());
        const bool same_price = old.price == new_price;
        const uint64_t retained_at_target = same_price ? old_level->second - old.remaining.get() :
            (target == side_levels.end() ? uint64_t{0} : target->second);
        if (new_remaining.get() > std::numeric_limits<uint64_t>::max() - retained_at_target) return false;

        // Stage both allocating operations before changing any existing order
        // or quantity. Roll back a newly allocated empty level if node/bucket
        // allocation fails. Map iterators survive insertion; old order removal
        // uses its key rather than the potentially invalidated iterator.
        const bool new_level = target == side_levels.end();
        if (new_level) target = side_levels.emplace(new_price.get(), 0).first;
        try {
            const auto inserted = orders_.emplace(new_id,
                MarketDataOrder{new_price, new_remaining, old.side});
            if (!inserted.second) {
                if (new_level) side_levels.erase(target);
                return false;
            }
        } catch (...) {
            if (new_level) side_levels.erase(target);
            throw;
        }
        target->second = retained_at_target + new_remaining.get();
        if (!same_price) {
            old_level->second -= old.remaining.get();
            if (old_level->second == 0) side_levels.erase(old_level);
        }
        orders_.erase(old_id);
        return true;
    }
    // Borrowed until the next mutation. No mutable order/aggregate access is
    // exposed to adapters, so identities and level quantities stay coherent.
    const MarketDataOrder* find_order(uint64_t order_id) const noexcept {
        const auto found = orders_.find(order_id);
        return found == orders_.end() ? nullptr : &found->second;
    }
    std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return Price(bids_.rbegin()->first);
    }
    std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return Price(asks_.begin()->first);
    }
    // Cold verification path: reconstruct aggregates independently from orders.
    // It may allocate/throw, and is not a per-event hot-path latency promise.
    bool check_invariants() const {
        std::map<int64_t, uint64_t> expected_bids;
        std::map<int64_t, uint64_t> expected_asks;
        for (const auto& entry : orders_) {
            const auto& order = entry.second;
            if (entry.first == 0 || order.price.get() <= 0 || order.remaining.get() == 0 ||
                (order.side != Side::BUY && order.side != Side::SELL)) return false;
            auto& expected = order.side == Side::BUY ? expected_bids : expected_asks;
            auto& aggregate = expected[order.price.get()];
            if (order.remaining.get() > std::numeric_limits<uint64_t>::max() - aggregate) return false;
            aggregate += order.remaining.get();
        }
        return bids_ == expected_bids && asks_ == expected_asks;
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
