#include "risk_checked_trading_engine.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>
#include <string>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {
constexpr int64_t signed_maximum = std::numeric_limits<int64_t>::max();
constexpr int64_t signed_minimum = std::numeric_limits<int64_t>::min();
constexpr uint64_t unsigned_maximum = std::numeric_limits<uint64_t>::max();

RiskDecision check_order(const PreTradeRisk& risk, const MarketManager& markets,
                         Side side, uint64_t quantity, int64_t price = 1'000'000) {
    return risk.check(Symbol("AAPL"), NewOrderCommand(CommandSequence(1), OrderId(1),
                      side, Price(price), Quantity(quantity)), markets);
}
void expect_decision(RiskDecision actual, RiskRejectReason reason) {
    if (actual.reason != reason || actual.accepted != (reason == RiskRejectReason::NONE)) {
        throw std::runtime_error("Risk decision mismatch: expected=" +
            std::to_string(static_cast<unsigned>(reason)) + " actual=" +
            std::to_string(static_cast<unsigned>(actual.reason)));
    }
}
void notify_rest(PreTradeRisk& risk, InstrumentId id, uint64_t order_id,
                 Side side, uint64_t quantity) {
    risk.on_event(SystemEvent::engine(Timestamp(0), Symbol("AAPL"),
        OrderRestedEvent(CommandSequence(order_id), EventIndex(0), OrderId(order_id),
            side, Price(1'000'000), Quantity(quantity), Quantity(quantity),
            PrioritySequence(order_id)), id));
}
void notify_cancel(PreTradeRisk& risk, InstrumentId id, uint64_t order_id,
                   Side side, uint64_t quantity) {
    risk.on_event(SystemEvent::engine(Timestamp(0), Symbol("AAPL"),
        OrderCancelledEvent(CommandSequence(order_id), EventIndex(0), OrderId(order_id),
            side, Price(1'000'000), Quantity(quantity), PrioritySequence(order_id)), id));
}

// This regression is safe to run against the old code: no MIN abs or signed
// subtraction overflow. On MSVC, long double has insufficient integer precision.
void test_exact_position_limit_without_floating_rounding() {
    MarketManager markets(4);
    PreTradeRisk risk;
    risk.bind(markets);
    const Symbol symbol("AAPL");
    risk.set_position(symbol, std::numeric_limits<int64_t>::max() - 1);
    const auto result = risk.check(symbol,
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        Price(1'000'000), Quantity(2)), markets);
    std::cout << "position MAX-1 plus BUY 2: accepted=" << result.accepted << '\n';
    TEST_ASSERT(!result.accepted && result.reason == RiskRejectReason::POSITION_LIMIT);
}

void test_full_width_position_boundaries() {
    struct Case { int64_t position, limit; Side side; uint64_t quantity; bool accepted; };
    const uint64_t half_range = uint64_t{1} << 63;
    const Case cases[] = {
        {signed_maximum, signed_maximum, Side::BUY, 1, false},
        {signed_maximum - 1, signed_maximum, Side::BUY, 1, true},
        {signed_minimum, signed_maximum, Side::BUY, 0, false},
        {signed_minimum, signed_maximum, Side::BUY, 1, true},
        {signed_minimum, signed_maximum, Side::BUY, unsigned_maximum, true},
        {signed_minimum, signed_maximum, Side::SELL, 1, false},
        {signed_maximum, signed_maximum, Side::SELL, unsigned_maximum, false},
        {signed_maximum, signed_maximum, Side::SELL, unsigned_maximum - 1, true},
        {0, signed_maximum, Side::BUY, static_cast<uint64_t>(signed_maximum), true},
        {0, signed_maximum, Side::BUY, half_range, false},
        {0, signed_maximum, Side::SELL, static_cast<uint64_t>(signed_maximum), true},
        {0, signed_maximum, Side::SELL, half_range, false},
        {signed_minimum, 0, Side::BUY, half_range, true},
        {signed_minimum, 0, Side::BUY, half_range - 1, false},
        {signed_minimum, 0, Side::BUY, half_range + 1, false},
        {10, 4, Side::SELL, 6, true}, {10, 4, Side::SELL, 5, false},
        {10, 4, Side::SELL, 14, true}, {10, 4, Side::SELL, 15, false},
        {-10, 4, Side::BUY, 6, true}, {-10, 4, Side::BUY, 5, false},
        {-10, 4, Side::BUY, 14, true}, {-10, 4, Side::BUY, 15, false},
        {0, -1, Side::BUY, 1, false}
    };
    MarketManager markets(4);
    for (const auto& value : cases) {
        RiskLimits limits;
        limits.max_absolute_position = value.limit;
        PreTradeRisk risk(limits);
        risk.set_position(Symbol("AAPL"), value.position);
        risk.bind(markets);
        expect_decision(check_order(risk, markets, value.side, value.quantity),
            value.accepted ? RiskRejectReason::NONE : RiskRejectReason::POSITION_LIMIT);
    }
    std::cout << "Full-width position boundaries passed: 24 cases\n";
}

void test_exact_price_deviation_boundaries() {
    struct Case { int64_t reference, price; uint32_t bps; bool accepted; };
    const Case cases[] = {
        {signed_maximum, signed_maximum, 0, true},
        {signed_maximum, signed_maximum - 1, 0, false},
        {signed_maximum, signed_minimum, 20000, false},
        {signed_maximum, signed_minimum, 20001, true},
        {signed_maximum, signed_minimum, std::numeric_limits<uint32_t>::max(), true},
        {1, signed_minimum, std::numeric_limits<uint32_t>::max(), false},
        {10001, 10002, 1, true}, {10001, 10003, 1, false},
        {10000, 10100, 100, true}, {10000, 10101, 100, false},
        {10000, 9900, 100, true}, {10000, 9899, 100, false},
        {1, -1, 20000, true}, {1, -1, 19999, false},
        {signed_maximum, 0, 10000, true}
    };
    MarketManager markets(4);
    for (const auto& value : cases) {
        RiskLimits limits;
        limits.max_price_deviation_bps = value.bps;
        PreTradeRisk risk(limits);
        risk.set_reference_price(Symbol("AAPL"), Price(value.reference));
        risk.bind(markets);
        // Direct arithmetic check only: accepting here does not make a negative
        // price a valid matching command. The core validates prices separately.
        expect_decision(check_order(risk, markets, Side::BUY, 1, value.price),
            value.accepted ? RiskRejectReason::NONE : RiskRejectReason::PRICE_DEVIATION);
    }
    size_t checks = 0;
    for (int64_t reference = 1; reference <= 25; ++reference) {
        for (uint32_t bps : {0U, 1U, 500U, 9999U, 10000U, 20000U}) {
            RiskLimits limits;
            limits.max_price_deviation_bps = bps;
            PreTradeRisk risk(limits);
            risk.set_reference_price(Symbol("AAPL"), Price(reference));
            risk.bind(markets);
            for (int64_t price = -30; price <= 30; ++price) {
                const int64_t difference = price >= reference ? price - reference : reference - price;
                const bool accepted = difference * 10000 <= reference * static_cast<int64_t>(bps);
                expect_decision(check_order(risk, markets, Side::BUY, 1, price),
                    accepted ? RiskRejectReason::NONE : RiskRejectReason::PRICE_DEVIATION);
                ++checks;
            }
        }
    }
    std::cout << "Exact price deviation passed: 15 extremes + " << checks << " small oracle cases\n";
}

void test_minimum_position_exposure_before_and_after_bind() {
    MarketManager markets(4);
    for (bool configure_before_bind : {true, false}) {
        RiskLimits limits;
        limits.max_total_exposure = 1;
        PreTradeRisk risk(limits);
        if (!configure_before_bind) risk.bind(markets);
        risk.set_reference_price(Symbol("AAPL"), Price(PRICE_SCALE));
        risk.set_position(Symbol("AAPL"), signed_minimum);
        if (configure_before_bind) risk.bind(markets);
        expect_decision(check_order(risk, markets, Side::BUY, 1, PRICE_SCALE),
                        RiskRejectReason::TOTAL_EXPOSURE_LIMIT);
    }
    std::cout << "MIN position exposure passed: pre/post bind\n";
}

void test_pending_position_small_oracle_and_full_width() {
    MarketManager markets(4);
    const InstrumentId id = markets.resolve(Symbol("AAPL"));
    size_t checks = 0;
    for (int64_t limit = 0; limit <= 8; ++limit) {
        RiskLimits limits;
        limits.max_absolute_position = limit;
        PreTradeRisk risk(limits);
        risk.bind(markets);
        for (uint64_t pending = 0; pending <= 8; ++pending) {
            // Deliberate synthetic observer inputs isolate risk arithmetic.
            notify_rest(risk, id, 1, Side::BUY, pending);
            notify_rest(risk, id, 2, Side::SELL, pending);
            for (int64_t position = -8; position <= 8; ++position) {
                risk.set_position(Symbol("AAPL"), position);
                for (uint64_t quantity = 0; quantity <= 8; ++quantity) {
                    for (Side side : {Side::BUY, Side::SELL}) {
                        const int64_t change = static_cast<int64_t>(pending + quantity);
                        const int64_t projected = position + (side == Side::BUY ? change : -change);
                        const bool accepted = projected >= -limit && projected <= limit;
                        expect_decision(check_order(risk, markets, side, quantity),
                            accepted ? RiskRejectReason::NONE : RiskRejectReason::POSITION_LIMIT);
                        ++checks;
                    }
                }
            }
            notify_cancel(risk, id, 1, Side::BUY, pending);
            notify_cancel(risk, id, 2, Side::SELL, pending);
            TEST_ASSERT(risk.state(id)->open_orders == 0);
        }
    }
    PreTradeRisk risk;
    risk.bind(markets);
    risk.set_position(Symbol("AAPL"), signed_minimum);
    notify_rest(risk, id, 1, Side::BUY, unsigned_maximum - 1);
    expect_decision(check_order(risk, markets, Side::BUY, 1), RiskRejectReason::NONE);
    expect_decision(check_order(risk, markets, Side::BUY, 2), RiskRejectReason::POSITION_LIMIT);
    notify_cancel(risk, id, 1, Side::BUY, unsigned_maximum - 1);
    risk.set_position(Symbol("AAPL"), signed_maximum);
    notify_rest(risk, id, 2, Side::SELL, unsigned_maximum - 2);
    expect_decision(check_order(risk, markets, Side::SELL, 1), RiskRejectReason::NONE);
    expect_decision(check_order(risk, markets, Side::SELL, 2), RiskRejectReason::POSITION_LIMIT);
    std::cout << "Pending position passed: " << checks << " small oracle + 4 full-width cases\n";
}

void seed_two_levels(MarketManager& markets, Side side, uint64_t first_quantity) {
    NullEventSink sink;
    for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
        const auto result = markets.process(Symbol("AAPL"), NewOrderCommand(
            CommandSequence(sequence), OrderId(sequence), side,
            Price(1'000'000 + static_cast<int64_t>(sequence) * 100),
            Quantity(sequence == 1 ? first_quantity : 1)), sink);
        TEST_ASSERT(result.routed() && result.process_result.applied());
    }
}

void test_aggregate_import_overflow_fails_closed_and_rebinds() {
    for (Side side : {Side::BUY, Side::SELL}) {
        MarketManager markets(4);
        const InstrumentId id = markets.resolve(Symbol("AAPL"));
        seed_two_levels(markets, side, unsigned_maximum);
        PreTradeRisk risk;
        bool overflow = false;
        try { risk.bind(markets); } catch (const std::overflow_error&) { overflow = true; }
        TEST_ASSERT(overflow && !risk.state_available());
        expect_decision(check_order(risk, markets, Side::BUY, 1), RiskRejectReason::STATE_UNAVAILABLE);
        TEST_ASSERT(risk.check(Symbol("AAPL"),
            CancelOrderCommand(CommandSequence(3), OrderId(1)), markets).accepted);
        // Setters and callbacks cannot index partially imported state or clear
        // the latch. Only a complete import of a repaired source restores it.
        risk.set_position(Symbol("AAPL"), signed_minimum);
        risk.set_reference_price(Symbol("AAPL"), Price(PRICE_SCALE));
        notify_rest(risk, id, 3, side, 1);
        TEST_ASSERT(!risk.state_available());
        NullEventSink sink;
        TEST_ASSERT(markets.process(Symbol("AAPL"),
            CancelOrderCommand(CommandSequence(3), OrderId(1)), sink).process_result.applied());
        risk.bind(markets);
        TEST_ASSERT(risk.state_available() && risk.state(id)->open_orders == 1);
        TEST_ASSERT((side == Side::BUY ? risk.state(id)->open_buy_quantity
                                      : risk.state(id)->open_sell_quantity) == 1);

        MarketManager exact_markets(4);
        seed_two_levels(exact_markets, side, unsigned_maximum - 1);
        risk.bind(exact_markets);
        TEST_ASSERT(risk.state_available() && risk.state(id)->open_orders == 2);
        TEST_ASSERT((side == Side::BUY ? risk.state(id)->open_buy_quantity
                                      : risk.state(id)->open_sell_quantity) == unsigned_maximum);
        notify_cancel(risk, id, 2, side, 1);
        TEST_ASSERT((side == Side::BUY ? risk.state(id)->open_buy_quantity
                                      : risk.state(id)->open_sell_quantity) == unsigned_maximum - 1);
    }
    std::cout << "Aggregate import passed: BUY/SELL overflow + exact maximum + rebind\n";
}

void test_aggregate_callback_failure_stops_engine() {
    MarketManager markets(4);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    PreTradeRisk risk;
    RiskCheckedTradingEngine checked(risk, engine);
    const Symbol symbol("AAPL");
    // Deliberately bypass the optional pre-trade wrapper to exercise observer
    // capacity failure on legal core orders at separate price levels.
    TEST_ASSERT(engine.submit(Timestamp(1), symbol, NewOrderCommand(CommandSequence(1),
        OrderId(1), Side::BUY, Price(1'000'000), Quantity(unsigned_maximum))).process_result.applied());
    bool overflow = false;
    try {
        engine.submit(Timestamp(2), symbol, NewOrderCommand(CommandSequence(2), OrderId(2),
            Side::BUY, Price(1'000'100), Quantity(1)));
    } catch (const std::overflow_error&) { overflow = true; }
    TEST_ASSERT(overflow && engine.failed() && !risk.state_available());
    TEST_ASSERT(risk.state(markets.resolve(symbol))->open_orders == 1);
    TEST_ASSERT(risk.state(markets.resolve(symbol))->open_buy_quantity == unsigned_maximum);
    TEST_ASSERT(markets.find_book(symbol)->active_order_count() == 2);
    TEST_ASSERT(markets.find_book(symbol)->event_output_state() == EventOutputState::FAILED);
    const auto blocked = checked.submit(Timestamp(3), symbol, NewOrderCommand(CommandSequence(3),
        OrderId(3), Side::BUY, Price(1'000'200), Quantity(1)));
    TEST_ASSERT(blocked.risk.reason == RiskRejectReason::STATE_UNAVAILABLE && !blocked.matching);
    const auto cancelled = checked.submit(Timestamp(4), symbol,
        CancelOrderCommand(CommandSequence(3), OrderId(1)));
    TEST_ASSERT(cancelled.risk.accepted && cancelled.matching &&
                cancelled.matching->routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
    std::cout << "Aggregate callback failure passed: observer unavailable + engine FAILED\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        test_exact_position_limit_without_floating_rounding();
        if (argc == 2 && std::string(argv[1]) == "--safe-boundary-only") return 0;
        if (argc != 1) throw std::runtime_error("Unknown risk test option");
        test_full_width_position_boundaries();
        test_exact_price_deviation_boundaries();
        test_minimum_position_exposure_before_and_after_bind();
        test_pending_position_small_oracle_and_full_width();
        test_aggregate_import_overflow_fails_closed_and_rebinds();
        test_aggregate_callback_failure_stops_engine();
        MarketManager markets(16);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        RiskLimits limits;
        limits.max_order_quantity = 10;
        limits.max_price_deviation_bps = 100;
        limits.max_open_orders = 100;
        limits.max_symbol_notional = 1000;
        limits.max_absolute_position = 4;
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
        const auto cancelled = checked.submit(Timestamp(4), Symbol("AAPL"), killed);
        TEST_ASSERT(cancelled.risk.accepted && cancelled.matching->routed());
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
        TEST_ASSERT(risk.state(markets.resolve(Symbol("AAPL")))->open_orders == 0);

        const Command blocked = NewOrderCommand(CommandSequence(3), OrderId(4),
            Side::BUY, from_double(100), Quantity(1));
        TEST_ASSERT(checked.submit(Timestamp(5), Symbol("AAPL"), blocked).risk.reason ==
                    RiskRejectReason::KILL_SWITCH_ENABLED);

        risk.set_kill_switch(false);
        risk.set_position(Symbol("AAPL"), 0);
        const Command first_open = NewOrderCommand(CommandSequence(3), OrderId(5),
            Side::BUY, from_double(100), Quantity(2));
        TEST_ASSERT(checked.submit(Timestamp(6), Symbol("AAPL"), first_open).risk.accepted);
        const Command second_open = NewOrderCommand(CommandSequence(4), OrderId(6),
            Side::BUY, from_double(100), Quantity(2));
        TEST_ASSERT(checked.submit(Timestamp(7), Symbol("AAPL"), second_open).risk.accepted);
        const Command exceeds_if_all_fill = NewOrderCommand(CommandSequence(5), OrderId(7),
            Side::BUY, from_double(100), Quantity(1));
        TEST_ASSERT(checked.submit(Timestamp(8), Symbol("AAPL"), exceeds_if_all_fill).risk.reason ==
                    RiskRejectReason::POSITION_LIMIT);
        TEST_ASSERT(risk.state(markets.resolve(Symbol("AAPL")))->open_buy_quantity == 4);
        std::cout << "Risk engine tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Risk engine tests failed: " << error.what() << '\n';
        return 1;
    }
}
