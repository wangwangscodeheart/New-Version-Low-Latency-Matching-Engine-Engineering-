#ifndef RISK_ENGINE_HPP
#define RISK_ENGINE_HPP

#include "commands.hpp"
#include "market_manager.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_map>

enum class RiskRejectReason : uint8_t {
    NONE = 0,
    QUANTITY_LIMIT,
    PRICE_DEVIATION,
    POSITION_LIMIT,
    NOTIONAL_LIMIT,
    TOO_MANY_OPEN_ORDERS,
    TOTAL_EXPOSURE_LIMIT,
    KILL_SWITCH_ENABLED,
    UNKNOWN_SYMBOL
};

struct RiskDecision {
    bool accepted;
    RiskRejectReason reason;
    static RiskDecision accept() noexcept { return {true, RiskRejectReason::NONE}; }
    static RiskDecision reject(RiskRejectReason reason) noexcept { return {false, reason}; }
};

struct RiskLimits {
    uint64_t max_order_quantity = std::numeric_limits<uint64_t>::max();
    uint32_t max_price_deviation_bps = std::numeric_limits<uint32_t>::max();
    size_t max_open_orders = std::numeric_limits<size_t>::max();
    long double max_symbol_notional = std::numeric_limits<long double>::max();
    int64_t max_absolute_position = std::numeric_limits<int64_t>::max();
    long double max_total_exposure = std::numeric_limits<long double>::max();
};

class PreTradeRisk {
    RiskLimits limits_;
    bool kill_switch_ = false;
    std::unordered_map<Symbol, Price, SymbolHash> reference_prices_;
    std::unordered_map<Symbol, int64_t, SymbolHash> positions_;

    static long double notional(Price price, uint64_t quantity) noexcept {
        return std::fabs(static_cast<long double>(price.get())) *
               static_cast<long double>(quantity) /
               static_cast<long double>(PRICE_SCALE);
    }

public:
    explicit PreTradeRisk(RiskLimits limits = {}) : limits_(limits) {}
    void set_kill_switch(bool enabled) noexcept { kill_switch_ = enabled; }
    bool kill_switch_enabled() const noexcept { return kill_switch_; }
    void set_reference_price(const Symbol& symbol, Price price) {
        reference_prices_.insert_or_assign(symbol, price);
    }
    void set_position(const Symbol& symbol, int64_t position) {
        positions_.insert_or_assign(symbol, position);
    }

    RiskDecision check(const Symbol& symbol, const Command& command,
                       const MarketManager& markets) const {
        if (kill_switch_) return RiskDecision::reject(RiskRejectReason::KILL_SWITCH_ENABLED);
        const OrderBook* target = markets.find_book(symbol);
        if (!target) return RiskDecision::reject(RiskRejectReason::UNKNOWN_SYMBOL);
        const auto* order = std::get_if<NewOrderCommand>(&command);
        if (!order) return RiskDecision::accept(); // Cancels reduce risk.
        if (order->quantity.get() > limits_.max_order_quantity) {
            return RiskDecision::reject(RiskRejectReason::QUANTITY_LIMIT);
        }

        size_t open_orders = 0;
        long double total_exposure = 0;
        for (const Symbol& registered : markets.symbols()) {
            const OrderBook* book = markets.find_book(registered);
            open_orders += book->active_order_count();
            const auto position = positions_.find(registered);
            const auto reference = reference_prices_.find(registered);
            if (position != positions_.end() && reference != reference_prices_.end()) {
                total_exposure += notional(reference->second,
                    static_cast<uint64_t>(std::llabs(position->second)));
            }
        }
        if (open_orders >= limits_.max_open_orders) {
            return RiskDecision::reject(RiskRejectReason::TOO_MANY_OPEN_ORDERS);
        }

        const auto reference = reference_prices_.find(symbol);
        if (reference != reference_prices_.end() && reference->second.get() > 0) {
            const long double difference = std::fabs(
                static_cast<long double>(order->price.get() - reference->second.get()));
            const long double bps = difference * 10000.0L /
                                    static_cast<long double>(reference->second.get());
            if (bps > limits_.max_price_deviation_bps) {
                return RiskDecision::reject(RiskRejectReason::PRICE_DEVIATION);
            }
        }

        long double symbol_notional = notional(order->price, order->quantity.get());
        const OrderBookState state = target->capture_state();
        const auto accumulate = [&](const auto& levels) {
            for (const BookLevelState& level : levels) {
                for (const BookOrderState& resting : level.orders) {
                    symbol_notional += notional(resting.price, resting.remaining_quantity.get());
                }
            }
        };
        accumulate(state.bid_levels);
        accumulate(state.ask_levels);
        if (symbol_notional > limits_.max_symbol_notional) {
            return RiskDecision::reject(RiskRejectReason::NOTIONAL_LIMIT);
        }

        const int64_t current_position = positions_.count(symbol) ? positions_.at(symbol) : 0;
        const long double projected = static_cast<long double>(current_position) +
            (order->side == Side::BUY ? 1.0L : -1.0L) * order->quantity.get();
        if (std::fabs(projected) > static_cast<long double>(limits_.max_absolute_position)) {
            return RiskDecision::reject(RiskRejectReason::POSITION_LIMIT);
        }
        total_exposure += notional(order->price, order->quantity.get());
        if (total_exposure > limits_.max_total_exposure) {
            return RiskDecision::reject(RiskRejectReason::TOTAL_EXPOSURE_LIMIT);
        }
        return RiskDecision::accept();
    }
};

#endif
