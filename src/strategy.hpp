#ifndef STRATEGY_HPP
#define STRATEGY_HPP

#include "trading_engine.hpp"
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <vector>

class StrategyContext {
    TradingEngine& engine_;

public:
    explicit StrategyContext(TradingEngine& engine) noexcept : engine_(engine) {}

    MarketProcessResult submit(Timestamp timestamp, const Symbol& symbol,
                               const Command& command) {
        return engine_.submit(timestamp, symbol, command);
    }
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual void on_market_data(Timestamp timestamp, const Symbol& symbol,
                                const MarketDataEvent& market_data,
                                StrategyContext& context) = 0;
};

// StrategyHost does not own strategies. Registered strategies and this host
// must remain alive while the dispatcher can publish market-data events.
class StrategyHost {
    StrategyContext context_;
    std::vector<std::reference_wrapper<Strategy>> strategies_;
    EventDispatcher::Subscription subscription_;

public:
    explicit StrategyHost(TradingEngine& engine) noexcept : context_(engine) {}

    void add_strategy(Strategy& strategy) {
        strategies_.push_back(strategy);
    }

    void attach(EventDispatcher& dispatcher) {
        subscription_ = dispatcher.subscribe(SystemEventType::MARKET_DATA,
            [this](const SystemEvent& event) {
                const auto& market_data = std::get<MarketDataEvent>(event.payload);
                for (Strategy& strategy : strategies_) {
                    strategy.on_market_data(event.timestamp, event.symbol,
                                            market_data, context_);
                }
            });
    }

    size_t strategy_count() const noexcept { return strategies_.size(); }
};

#endif
