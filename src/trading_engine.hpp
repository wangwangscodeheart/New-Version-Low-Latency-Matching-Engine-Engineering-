#ifndef TRADING_ENGINE_HPP
#define TRADING_ENGINE_HPP

#include "event_dispatcher.hpp"
#include "market_manager.hpp"
#include "submission_gate.hpp"
#include <chrono>
#include <cstddef>
#include <variant>

// Application-layer coordinator. It keeps OrderBook single-writer semantics,
// publishes the input command, processes it exactly once, then publishes only
// the EngineEvents produced by that command in their original event-index order.
class TradingEngine {
    MarketManager& markets_;
    EventDispatcher& dispatcher_;
    const SubmissionGate* submission_gate_ = nullptr;

public:
    TradingEngine(MarketManager& markets, EventDispatcher& dispatcher,
                  const SubmissionGate* submission_gate = nullptr) noexcept
        : markets_(markets), dispatcher_(dispatcher),
          submission_gate_(submission_gate) {}

    MarketProcessResult submit(Timestamp timestamp, const Symbol& symbol,
                               const Command& command) {
        return submit_impl(timestamp, symbol, markets_.resolve(symbol), command);
    }

    MarketProcessResult submit(Timestamp timestamp, InstrumentId instrument_id,
                               const Command& command) {
        const Symbol* symbol = markets_.symbol(instrument_id);
        if (!symbol) {
            return MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return submit_impl(timestamp, *symbol, instrument_id, command);
    }

private:
    MarketProcessResult submit_impl(Timestamp timestamp, const Symbol& symbol,
                                    InstrumentId instrument_id,
                                    const Command& command) {
        if (submission_gate_ && !submission_gate_->available()) {
            dispatcher_.publish(SystemEvent::processing_latency(
                timestamp, symbol,
                ProcessingLatencyEvent{0, CommandOutcome::SYSTEM_UNAVAILABLE,
                    get_command_sequence(command), get_order_id(command)}, instrument_id));
            return MarketProcessResult{RoutingStatus::SYSTEM_UNAVAILABLE,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        dispatcher_.publish(SystemEvent::order(timestamp, symbol, command, instrument_id));

        class DispatcherSink {
            EventDispatcher& dispatcher_;
            Timestamp timestamp_;
            const Symbol& symbol_;
            InstrumentId instrument_id_;
        public:
            DispatcherSink(EventDispatcher& dispatcher, Timestamp timestamp,
                           const Symbol& symbol, InstrumentId instrument_id) noexcept
                : dispatcher_(dispatcher), timestamp_(timestamp), symbol_(symbol),
                  instrument_id_(instrument_id) {}
            bool push(const EngineEvent& event) {
                dispatcher_.publish(SystemEvent::engine(
                    timestamp_, symbol_, event, instrument_id_));
                return true;
            }
        } sink(dispatcher_, timestamp, symbol, instrument_id);

        const auto start = std::chrono::steady_clock::now();
        const MarketProcessResult result = instrument_id.valid()
            ? markets_.process(instrument_id, command, sink)
            : MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        const auto finish = std::chrono::steady_clock::now();
        const uint64_t latency_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        if (!result.routed()) {
            dispatcher_.publish(SystemEvent::processing_latency(
                timestamp, symbol,
                ProcessingLatencyEvent{latency_ns, CommandOutcome::UNKNOWN_SYMBOL,
                    get_command_sequence(command), get_order_id(command)}, instrument_id));
            return result;
        }

        const CommandOutcome outcome = result.process_result.status == ProcessStatus::APPLIED
            ? CommandOutcome::APPLIED
            : result.process_result.status == ProcessStatus::REJECTED
            ? CommandOutcome::BUSINESS_REJECTED
            : CommandOutcome::SEQUENCE_REJECTED;
        dispatcher_.publish(SystemEvent::processing_latency(
            timestamp, symbol,
            ProcessingLatencyEvent{latency_ns, outcome,
                get_command_sequence(command), get_order_id(command)}, instrument_id));
        return result;
    }

public:

    void publish_market_data(Timestamp timestamp, const Symbol& symbol,
                             const MarketDataEvent& market_data) {
        dispatcher_.publish(
            SystemEvent::market_data(timestamp, symbol, market_data));
    }

    MarketManager& market_manager() noexcept { return markets_; }
    const MarketManager& market_manager() const noexcept { return markets_; }
};

#endif
