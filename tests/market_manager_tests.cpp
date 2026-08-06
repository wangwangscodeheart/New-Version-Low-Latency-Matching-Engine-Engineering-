#include "market_manager.hpp"
#include <iostream>
#include <stdexcept>
#include <variant>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

void test_default_symbols() {
    MarketManager manager(64);
    TEST_ASSERT(manager.symbol_count() == 3);
    TEST_ASSERT(manager.contains(Symbol("AAPL")));
    TEST_ASSERT(manager.contains(Symbol("TSLA")));
    TEST_ASSERT(manager.contains(Symbol("NVDA")));
}

void test_symbol_isolation_and_routing() {
    MarketManager manager(64);

    const auto aapl_sell = manager.process(
        Symbol("AAPL"), NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::SELL, from_double(100.00), Quantity(10)));
    const auto tsla_buy = manager.process(
        Symbol("TSLA"), NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, from_double(200.00), Quantity(20)));

    TEST_ASSERT(aapl_sell.routed() && aapl_sell.process_result.applied());
    TEST_ASSERT(tsla_buy.routed() && tsla_buy.process_result.applied());
    TEST_ASSERT(manager.find_book(Symbol("AAPL"))->best_ask() == from_double(100.00));
    TEST_ASSERT(manager.find_book(Symbol("TSLA"))->best_bid() == from_double(200.00));
    TEST_ASSERT(!manager.find_book(Symbol("NVDA"))->best_bid().has_value());

    const auto aapl_buy = manager.process(
        Symbol("AAPL"), NewOrderCommand(CommandSequence(2), OrderId(2),
        Side::BUY, from_double(100.00), Quantity(10)));
    TEST_ASSERT(aapl_buy.process_result.applied());
    TEST_ASSERT(!manager.find_book(Symbol("AAPL"))->best_ask().has_value());
    TEST_ASSERT(manager.find_book(Symbol("TSLA"))->best_bid() == from_double(200.00));
    TEST_ASSERT(std::holds_alternative<TradeEvent>(
        manager.find_book(Symbol("AAPL"))->engine_events().back()));
}

void test_unknown_symbol_and_per_instrument_rules() {
    MarketManager manager(64);
    const auto unknown = manager.process(
        Symbol("MSFT"), NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, from_double(100.00), Quantity(10)));
    TEST_ASSERT(!unknown.routed());

    InstrumentRuntimeConfig config;
    config.order_capacity = 32;
    config.trading_rules.lot_size = 10;
    TEST_ASSERT(manager.register_symbol(Symbol("AMD"), config));
    TEST_ASSERT(!manager.register_symbol(Symbol("AMD"), config));

    const auto rejected = manager.process(
        Symbol("AMD"), NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::BUY, from_double(100.00), Quantity(11)));
    TEST_ASSERT(rejected.routed());
    TEST_ASSERT(rejected.process_result.status == ProcessStatus::REJECTED);
    const auto& event = manager.find_book(Symbol("AMD"))->engine_events().back();
    TEST_ASSERT(std::get<OrderRejectedEvent>(event).reason ==
                RejectReason::INVALID_LOT_SIZE);
}

void test_pre_resolved_instrument_routing() {
    MarketManager manager(64);
    const InstrumentId aapl = manager.resolve(Symbol("AAPL"));
    const InstrumentId tsla = manager.resolve(Symbol("TSLA"));
    TEST_ASSERT(aapl.valid() && tsla.valid() && aapl != tsla);
    TEST_ASSERT(manager.symbol(aapl) && manager.symbol(aapl)->value() == "AAPL");
    NullEventSink sink;
    const auto result = manager.process(aapl,
        NewOrderCommand(CommandSequence(1), OrderId(99), Side::BUY,
                        from_double(90), Quantity(1)), sink);
    TEST_ASSERT(result.routed() && result.process_result.applied());
    TEST_ASSERT(manager.find_book(aapl)->best_bid() == from_double(90));
    TEST_ASSERT(!manager.resolve(Symbol("UNKNOWN")).valid());
}

int main() {
    try {
        test_default_symbols();
        test_symbol_isolation_and_routing();
        test_unknown_symbol_and_per_instrument_rules();
        test_pre_resolved_instrument_routing();
        std::cout << "MarketManager tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MarketManager tests failed: " << error.what() << '\n';
        return 1;
    }
}
