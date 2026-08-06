#ifndef RISK_CHECKED_TRADING_ENGINE_HPP
#define RISK_CHECKED_TRADING_ENGINE_HPP

#include "risk_engine.hpp"
#include "trading_engine.hpp"
#include <optional>

struct RiskSubmitResult {
    RiskDecision risk;
    std::optional<MarketProcessResult> matching;
};

class RiskCheckedTradingEngine {
    PreTradeRisk& risk_;
    TradingEngine& engine_;
    EventDispatcher::Subscription risk_events_;
public:
    RiskCheckedTradingEngine(PreTradeRisk& risk, TradingEngine& engine)
        : risk_(risk), engine_(engine) {
        risk_.bind(engine_.market_manager());
        risk_events_ = engine_.event_dispatcher().subscribe_all(
            [this](const SystemEvent& event) { risk_.on_event(event); });
    }

    RiskSubmitResult submit(Timestamp timestamp, const Symbol& symbol,
                            const Command& command) {
        const RiskDecision decision = risk_.check(
            symbol, command, engine_.market_manager());
        if (!decision.accepted) return RiskSubmitResult{decision, std::nullopt};
        return RiskSubmitResult{decision, engine_.submit(timestamp, symbol, command)};
    }
};

#endif
