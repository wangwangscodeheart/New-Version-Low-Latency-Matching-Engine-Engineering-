#ifndef TRADING_ENGINE_HPP
#define TRADING_ENGINE_HPP

#include "event_dispatcher.hpp"
#include "market_manager.hpp"
#include <chrono>
#include <cstddef>
#include <variant>

// Application-layer coordinator. It keeps OrderBook single-writer semantics,
// publishes the input command, processes it exactly once, then publishes only
// the EngineEvents produced by that command in their original event-index order.
class TradingEngine {
    MarketManager& markets_;
    EventDispatcher& dispatcher_;

public:
    TradingEngine(MarketManager& markets, EventDispatcher& dispatcher) noexcept
        : markets_(markets), dispatcher_(dispatcher) {}

    MarketProcessResult submit(Timestamp timestamp, const Symbol& symbol,
                               const Command& command) {
        dispatcher_.publish(SystemEvent::order(timestamp, symbol, command));

        const auto start = std::chrono::steady_clock::now();
        const MarketProcessResult result = markets_.process(symbol, command);
        const auto finish = std::chrono::steady_clock::now();
        const uint64_t latency_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        if (!result.routed()) {
            dispatcher_.publish(SystemEvent::processing_latency(
                timestamp, symbol,
                ProcessingLatencyEvent{latency_ns, CommandOutcome::UNKNOWN_SYMBOL,
                    get_command_sequence(command), get_order_id(command)}));
            return result;
        }

        const OrderBook* book = markets_.find_book(symbol);
        const auto& events = book->engine_events();
        const size_t begin = result.process_result.event_begin;
        const size_t end = begin + result.process_result.event_count;
        for (size_t i = begin; i < end; ++i) {
            dispatcher_.publish(SystemEvent::engine(timestamp, symbol, events[i]));
        }
        const CommandOutcome outcome = result.process_result.status == ProcessStatus::APPLIED
            ? CommandOutcome::APPLIED
            : result.process_result.status == ProcessStatus::REJECTED
            ? CommandOutcome::BUSINESS_REJECTED
            : CommandOutcome::SEQUENCE_REJECTED;
        dispatcher_.publish(SystemEvent::processing_latency(
            timestamp, symbol,
            ProcessingLatencyEvent{latency_ns, outcome,
                get_command_sequence(command), get_order_id(command)}));
        return result;
    }

    void publish_market_data(Timestamp timestamp, const Symbol& symbol,
                             const MarketDataEvent& market_data) {
        dispatcher_.publish(
            SystemEvent::market_data(timestamp, symbol, market_data));
    }

    MarketManager& market_manager() noexcept { return markets_; }
    const MarketManager& market_manager() const noexcept { return markets_; }
};

#endif
