#include "risk_checked_trading_engine.hpp"
#include <iostream>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

int main() {
    try {
        MarketManager markets(16);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        RiskLimits limits;
        limits.max_order_quantity = 10;
        limits.max_price_deviation_bps = 100;
        limits.max_open_orders = 2;
        limits.max_symbol_notional = 250;
        limits.max_absolute_position = 20;
        limits.max_total_exposure = 1000;
        PreTradeRisk risk(limits);
        risk.set_reference_price(Symbol("AAPL"), from_double(100));
        RiskCheckedTradingEngine checked(risk, engine);

        const Command too_large = NewOrderCommand(CommandSequence(1), OrderId(1),
            Side::BUY, from_double(100), Quantity(11));
        const auto rejected = checked.submit(Timestamp(1), Symbol("AAPL"), too_large);
        TEST_ASSERT(!rejected.risk.accepted);
        TEST_ASSERT(rejected.risk.reason == RiskRejectReason::QUANTITY_LIMIT);
        TEST_ASSERT(!rejected.matching.has_value());
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->last_applied_command_sequence().get() == 0);

        const Command valid = NewOrderCommand(CommandSequence(1), OrderId(2),
            Side::BUY, from_double(100), Quantity(2));
        const auto accepted = checked.submit(Timestamp(2), Symbol("AAPL"), valid);
        TEST_ASSERT(accepted.risk.accepted && accepted.matching->routed());

        const Command deviated = NewOrderCommand(CommandSequence(2), OrderId(3),
            Side::BUY, from_double(103), Quantity(1));
        TEST_ASSERT(checked.submit(Timestamp(3), Symbol("AAPL"), deviated).risk.reason ==
                    RiskRejectReason::PRICE_DEVIATION);

        risk.set_kill_switch(true);
        const Command killed = CancelOrderCommand(CommandSequence(2), OrderId(2));
        TEST_ASSERT(checked.submit(Timestamp(4), Symbol("AAPL"), killed).risk.reason ==
                    RiskRejectReason::KILL_SWITCH_ENABLED);
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
        std::cout << "Risk engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Risk engine tests failed: " << error.what() << '\n';
        return 1;
    }
}
