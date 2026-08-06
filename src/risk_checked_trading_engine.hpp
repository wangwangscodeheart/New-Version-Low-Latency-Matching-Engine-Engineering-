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
public:
    RiskCheckedTradingEngine(PreTradeRisk& risk, TradingEngine& engine) noexcept
        : risk_(risk), engine_(engine) {}

    RiskSubmitResult submit(Timestamp timestamp, const Symbol& symbol,
                            const Command& command) {
        const RiskDecision decision = risk_.check(
            symbol, command, engine_.market_manager());
        if (!decision.accepted) return RiskSubmitResult{decision, std::nullopt};
        return RiskSubmitResult{decision, engine_.submit(timestamp, symbol, command)};
    }
};

#endif
