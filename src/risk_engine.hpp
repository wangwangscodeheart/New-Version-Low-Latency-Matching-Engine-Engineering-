#ifndef RISK_ENGINE_HPP
#define RISK_ENGINE_HPP

#include "commands.hpp"
#include "market_manager.hpp"
#include "system_events.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <vector>

enum class RiskRejectReason : uint8_t {
    NONE = 0, QUANTITY_LIMIT, PRICE_DEVIATION, POSITION_LIMIT,
    NOTIONAL_LIMIT, TOO_MANY_OPEN_ORDERS, TOTAL_EXPOSURE_LIMIT,
    KILL_SWITCH_ENABLED, UNKNOWN_SYMBOL
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

struct InstrumentRiskState {
    int64_t position = 0;
    size_t open_orders = 0;
    uint64_t open_buy_quantity = 0;
    uint64_t open_sell_quantity = 0;
    long double pending_notional = 0;
    Price reference_price{0};
};

class PreTradeRisk {
    struct RestingOrder { Side side; Price price; uint64_t remaining; };

    RiskLimits limits_;
    bool kill_switch_ = false;
    const MarketManager* markets_ = nullptr;
    std::vector<InstrumentRiskState> states_;
    std::vector<std::unordered_map<uint64_t, RestingOrder>> orders_;
    std::unordered_map<Symbol, Price, SymbolHash> configured_references_;
    std::unordered_map<Symbol, int64_t, SymbolHash> configured_positions_;
    size_t total_open_orders_ = 0;
    long double total_pending_notional_ = 0;
    long double total_position_exposure_ = 0;

    static long double notional(Price price, uint64_t quantity) noexcept {
        return std::fabs(static_cast<long double>(price.get())) *
               static_cast<long double>(quantity) /
               static_cast<long double>(PRICE_SCALE);
    }

    void recalculate_position_exposure() noexcept {
        total_position_exposure_ = 0;
        for (const InstrumentRiskState& state : states_) {
            total_position_exposure_ += notional(
                state.reference_price,
                static_cast<uint64_t>(std::llabs(state.position)));
        }
    }

    void add_resting(InstrumentId id, OrderId order_id, Side side,
                     Price price, Quantity remaining) {
        if (!id.valid() || id.get() >= states_.size() || remaining.get() == 0) return;
        auto& by_id = orders_[id.get()];
        if (by_id.find(order_id.get()) != by_id.end()) return;
        by_id.emplace(order_id.get(), RestingOrder{side, price, remaining.get()});
        InstrumentRiskState& state = states_[id.get()];
        ++state.open_orders;
        ++total_open_orders_;
        if (side == Side::BUY) state.open_buy_quantity += remaining.get();
        else state.open_sell_quantity += remaining.get();
        const long double value = notional(price, remaining.get());
        state.pending_notional += value;
        total_pending_notional_ += value;
    }

    void reduce_resting(InstrumentId id, OrderId order_id, uint64_t quantity) noexcept {
        if (!id.valid() || id.get() >= states_.size()) return;
        auto& by_id = orders_[id.get()];
        const auto found = by_id.find(order_id.get());
        if (found == by_id.end()) return;
        const uint64_t reduction = std::min(quantity, found->second.remaining);
        InstrumentRiskState& state = states_[id.get()];
        if (found->second.side == Side::BUY) state.open_buy_quantity -= reduction;
        else state.open_sell_quantity -= reduction;
        const long double value = notional(found->second.price, reduction);
        state.pending_notional -= value;
        total_pending_notional_ -= value;
        found->second.remaining -= reduction;
        if (found->second.remaining == 0) {
            --state.open_orders;
            --total_open_orders_;
            by_id.erase(found);
        }
    }

public:
    explicit PreTradeRisk(RiskLimits limits = {}) : limits_(limits) {}

    void bind(const MarketManager& markets) {
        markets_ = &markets;
        states_.assign(markets.symbol_count(), InstrumentRiskState{});
        orders_.clear();
        orders_.resize(markets.symbol_count());
        total_open_orders_ = 0;
        total_pending_notional_ = 0;
        for (size_t i = 0; i < markets.symbol_count(); ++i) {
            const InstrumentId id(static_cast<uint32_t>(i));
            const Symbol* symbol = markets.symbol(id);
            const auto reference = configured_references_.find(*symbol);
            if (reference != configured_references_.end()) states_[i].reference_price = reference->second;
            const auto position = configured_positions_.find(*symbol);
            if (position != configured_positions_.end()) states_[i].position = position->second;
            const OrderBookState state = markets.find_book(id)->capture_state();
            const auto import = [this, id](const auto& levels) {
                for (const BookLevelState& level : levels) {
                    for (const BookOrderState& order : level.orders) {
                        add_resting(id, order.order_id, order.side,
                                    order.price, order.remaining_quantity);
                    }
                }
            };
            import(state.bid_levels);
            import(state.ask_levels);
        }
        recalculate_position_exposure();
    }

    void set_kill_switch(bool enabled) noexcept { kill_switch_ = enabled; }
    bool kill_switch_enabled() const noexcept { return kill_switch_; }
    void set_reference_price(const Symbol& symbol, Price price) {
        configured_references_.insert_or_assign(symbol, price);
        if (markets_) {
            const InstrumentId id = markets_->resolve(symbol);
            if (id.valid()) states_[id.get()].reference_price = price;
            recalculate_position_exposure();
        }
    }
    void set_position(const Symbol& symbol, int64_t position) {
        configured_positions_.insert_or_assign(symbol, position);
        if (markets_) {
            const InstrumentId id = markets_->resolve(symbol);
            if (id.valid()) states_[id.get()].position = position;
            recalculate_position_exposure();
        }
    }

    void on_event(const SystemEvent& event) {
        const auto* engine_event = std::get_if<EngineEvent>(&event.payload);
        if (!engine_event) return;
        std::visit([this, id = event.instrument_id](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, OrderRestedEvent>) {
                add_resting(id, value.order_id, value.side,
                            value.price, value.remaining_quantity);
            } else if constexpr (std::is_same_v<T, OrderCancelledEvent>) {
                reduce_resting(id, value.order_id, value.cancelled_quantity.get());
            } else if constexpr (std::is_same_v<T, TradeEvent>) {
                reduce_resting(id, value.passive_order_id, value.quantity.get());
            }
        }, *engine_event);
    }

    RiskDecision check(const Symbol& symbol, const Command& command,
                       const MarketManager& markets) const {
        const InstrumentId id = markets.resolve(symbol);
        if (!id.valid() || id.get() >= states_.size()) {
            return RiskDecision::reject(RiskRejectReason::UNKNOWN_SYMBOL);
        }
        if (std::holds_alternative<CancelOrderCommand>(command)) {
            return RiskDecision::accept();
        }
        if (kill_switch_) return RiskDecision::reject(RiskRejectReason::KILL_SWITCH_ENABLED);
        const NewOrderCommand& order = std::get<NewOrderCommand>(command);
        const InstrumentRiskState& state = states_[id.get()];
        if (order.quantity.get() > limits_.max_order_quantity) {
            return RiskDecision::reject(RiskRejectReason::QUANTITY_LIMIT);
        }
        if (total_open_orders_ >= limits_.max_open_orders) {
            return RiskDecision::reject(RiskRejectReason::TOO_MANY_OPEN_ORDERS);
        }
        if (state.reference_price.get() > 0) {
            const long double difference = std::fabs(static_cast<long double>(
                order.price.get() - state.reference_price.get()));
            const long double bps = difference * 10000.0L /
                                    static_cast<long double>(state.reference_price.get());
            if (bps > limits_.max_price_deviation_bps) {
                return RiskDecision::reject(RiskRejectReason::PRICE_DEVIATION);
            }
        }
        const long double proposed_notional = notional(order.price, order.quantity.get());
        if (state.pending_notional + proposed_notional > limits_.max_symbol_notional) {
            return RiskDecision::reject(RiskRejectReason::NOTIONAL_LIMIT);
        }
        const long double projected = static_cast<long double>(state.position) +
            (order.side == Side::BUY ? 1.0L : -1.0L) * order.quantity.get();
        if (std::fabs(projected) > static_cast<long double>(limits_.max_absolute_position)) {
            return RiskDecision::reject(RiskRejectReason::POSITION_LIMIT);
        }
        if (total_position_exposure_ + total_pending_notional_ + proposed_notional >
            limits_.max_total_exposure) {
            return RiskDecision::reject(RiskRejectReason::TOTAL_EXPOSURE_LIMIT);
        }
        return RiskDecision::accept();
    }

    const InstrumentRiskState* state(InstrumentId id) const noexcept {
        return id.valid() && id.get() < states_.size() ? &states_[id.get()] : nullptr;
    }
};

#endif
