#ifndef EXAMPLE_STRATEGY_HPP
#define EXAMPLE_STRATEGY_HPP

#include "strategy.hpp"
#include <unordered_map>

// Demonstration only: submit one buy order per symbol when the best ask is at
// or below a configured threshold. Sequence/order-id allocation is injected so
// a real gateway can remain the single authority for command identity.
class BuyBelowAskStrategy : public Strategy {
public:
    using SequenceProvider = std::function<CommandSequence(const Symbol&)>;
    using OrderIdProvider = std::function<OrderId()>;

private:
    Price threshold_;
    Quantity quantity_;
    SequenceProvider next_sequence_;
    OrderIdProvider next_order_id_;
    std::unordered_map<Symbol, bool, SymbolHash> submitted_;

public:
    BuyBelowAskStrategy(Price threshold, Quantity quantity,
                        SequenceProvider sequence_provider,
                        OrderIdProvider order_id_provider)
        : threshold_(threshold), quantity_(quantity),
          next_sequence_(std::move(sequence_provider)),
          next_order_id_(std::move(order_id_provider)) {
        if (!next_sequence_ || !next_order_id_) {
            throw std::invalid_argument("Strategy providers cannot be empty");
        }
    }

    void on_market_data(Timestamp timestamp, const Symbol& symbol,
                        const MarketDataEvent& market_data,
                        StrategyContext& context) override {
        if (market_data.ask > threshold_ || submitted_[symbol]) return;
        const Command command = NewOrderCommand(next_sequence_(symbol),
            next_order_id_(), Side::BUY, market_data.ask, quantity_);
        const MarketProcessResult result = context.submit(timestamp, symbol, command);
        if (result.routed() && result.process_result.applied()) submitted_[symbol] = true;
    }
};

#endif
