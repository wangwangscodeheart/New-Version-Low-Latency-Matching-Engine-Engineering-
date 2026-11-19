// M0 diagnostic only: reproduces baseline defects, not a passing-correctness test.
// No production source changes. See ../baseline_test_report.md for build commands.
#include "event_sink.hpp"
#include "monitor.hpp"
#include "orderbook.hpp"
#include "reference_engine.hpp"
#include "trading_engine.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool condition) {
    if (!condition) throw std::runtime_error("Baseline observation changed");
}

int main() {
    try {
        {
            OrderBook book(2);
            const auto first = book.process(NewOrderCommand(CommandSequence(1),
                OrderId(1), Side::BUY, Price(100),
                Quantity(std::numeric_limits<uint64_t>::max())));
            const auto second = book.process(NewOrderCommand(CommandSequence(2),
                OrderId(2), Side::BUY, Price(100), Quantity(1)));
            const auto state = book.capture_state();
            require(first.applied() && second.applied());
            require(state.bid_levels.at(0).total_volume.get() == 0);
            require(!book.check_invariants());
            std::cout << "R01 volume_overflow: both_applied=1 total_volume=0 invariants=0\n";
        }
        {
            OrderBook book(1);
            ReferenceEngine reference(1);
            VectorEventSink actual;
            std::vector<EngineEvent> expected;
            const Command resting = NewOrderCommand(CommandSequence(1), OrderId(1),
                Side::SELL, Price(100), Quantity(1));
            book.process(resting, actual);
            reference.process(resting, expected);
            actual.clear();
            const Command crossing = NewOrderCommand(CommandSequence(2), OrderId(2),
                Side::BUY, Price(100), Quantity(1), TimeInForce::POST_ONLY);
            book.process(crossing, actual);
            reference.process(crossing, expected);
            const auto core_reason = std::get<OrderRejectedEvent>(actual.events().at(0)).reason;
            const auto ref_reason = std::get<OrderRejectedEvent>(expected.at(0)).reason;
            require(core_reason == RejectReason::POST_ONLY_WOULD_TRADE);
            require(ref_reason == RejectReason::POOL_EXHAUSTED);
            std::cout << "R02 post_only_full: core=" << to_string(core_reason)
                      << " reference=" << to_string(ref_reason) << '\n';
        }
        {
            OrderBook book(4);
            book.process(NewOrderCommand(CommandSequence(1), OrderId(1),
                Side::SELL, Price(100), Quantity(1)));
            book.process(NewOrderCommand(CommandSequence(2), OrderId(2),
                Side::SELL, Price(200), Quantity(1)));
            FixedEventBuffer<1> sink;
            const auto result = book.process(NewOrderCommand(CommandSequence(3),
                OrderId(3), Side::BUY, Price(200), Quantity(2)), sink);
            require(result.applied() && !result.event_output_complete);
            require(result.event_count == 2 && sink.size() == 1);
            require(book.active_order_count() == 0 && book.engine_events().empty());
            std::cout << "R03 sink_overflow: applied=1 generated=2 delivered=1 retained=0 active=0\n";
        }
        {
            MarketManager markets(4);
            EventDispatcher dispatcher;
            Monitor monitor;
            monitor.attach(dispatcher, &markets);
            dispatcher.publish(SystemEvent::data_quality(Timestamp(1), Symbol("AAPL"),
                DataQualityEvent{DataQualitySeverity::WARNING,
                    DataQualityCode::STALE_SYMBOL, "probe"}));
            const auto metrics = monitor.snapshot();
            require(metrics.global.data_warnings == 1);
            require(metrics.by_symbol.at("AAPL").data_warnings == 0);
            std::cout << "R04 monitor_quality: global_warnings=1 AAPL_warnings=0\n";
        }
        {
            class Gate final : public SubmissionGate {
            public:
                bool healthy = true;
                bool available() const noexcept override { return healthy; }
            } gate;
            MarketManager markets(4);
            EventDispatcher dispatcher;
            auto subscription = dispatcher.subscribe(SystemEventType::ORDER,
                [&](const SystemEvent&) { gate.healthy = false; });
            dispatcher.freeze();
            TradingEngine engine(markets, dispatcher, &gate);
            const auto first = engine.submit(Timestamp(1), Symbol("AAPL"),
                NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                    Price(100), Quantity(1)));
            const auto next = engine.submit(Timestamp(2), Symbol("AAPL"),
                CancelOrderCommand(CommandSequence(2), OrderId(1)));
            require(first.process_result.applied());
            require(next.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
            require(markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
            std::cout << "R05 gate_mid_command: first_applied=1 next_unavailable=1 active=1\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
