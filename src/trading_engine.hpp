#ifndef TRADING_ENGINE_HPP
#define TRADING_ENGINE_HPP

#include "event_dispatcher.hpp"
#include "command_journal.hpp"
#include "committed_command_sink.hpp"
#include "market_manager.hpp"
#include "submission_gate.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

// Application-layer coordinator. It keeps OrderBook single-writer semantics,
// publishes the input command, processes it exactly once, then publishes only
// the EngineEvents produced by that command in their original event-index order.
class TradingEngine {
    MarketManager& markets_;
    EventDispatcher& dispatcher_;
    const SubmissionGate* submission_gate_ = nullptr;
    CommandJournal* command_journal_ = nullptr;
    CommittedCommandSink* committed_sink_ = nullptr;
    std::vector<const OrderBook*> publication_books_;
    std::vector<EngineEvent> command_events_;
    size_t command_event_limit_ = 0;
    bool submitting_ = false;
    bool failed_ = false;

    class SubmissionScope {
        bool& submitting_;
    public:
        explicit SubmissionScope(bool& submitting) noexcept : submitting_(submitting) {
            submitting_ = true;
        }
        ~SubmissionScope() { submitting_ = false; }
        SubmissionScope(const SubmissionScope&) = delete;
        SubmissionScope& operator=(const SubmissionScope&) = delete;
    };

    static MarketProcessResult unavailable() noexcept {
        return MarketProcessResult{RoutingStatus::SYSTEM_UNAVAILABLE,
            ProcessResult{ProcessStatus::OUTPUT_BLOCKED, 0, 0, false}};
    }

    void fail_output() noexcept {
        failed_ = true;
        if (committed_sink_) committed_sink_->invalidate();
    }

    bool publication_ready() noexcept {
        if (!committed_sink_) return true;
        if (!committed_sink_->available() || markets_.symbol_count() != publication_books_.size()) {
            fail_output();
            return false;
        }
        for (size_t i = 0; i < publication_books_.size(); ++i) {
            const auto* book = markets_.find_book(InstrumentId(static_cast<uint32_t>(i)));
            if (book != publication_books_[i] || !book->event_recording_enabled()) {
                fail_output();
                return false;
            }
        }
        return true;
    }

    bool publish_committed(Timestamp timestamp, InstrumentId id,
                           const Command& command, CommandOutcome outcome) {
        if (!committed_sink_) return true;
        if (!publication_ready() || !committed_sink_->publish(
                timestamp, id, command, outcome, command_events_)) {
            fail_output();
            return false;
        }
        return true;
    }

public:
    TradingEngine(MarketManager& markets, EventDispatcher& dispatcher,
                  const SubmissionGate* submission_gate = nullptr) noexcept
        : markets_(markets), dispatcher_(dispatcher),
          submission_gate_(submission_gate) {}

    // Explicit recovery-critical integration. The journal must outlive this
    // coordinator and must not also subscribe in observer/attach mode.
    TradingEngine(MarketManager& markets, EventDispatcher& dispatcher,
                  CommandJournal& journal) noexcept
        : markets_(markets), dispatcher_(dispatcher), submission_gate_(&journal),
          command_journal_(&journal) {}

    // Opt-in publication session: fresh empty books, frozen registry and one
    // command writer. Existing constructors retain their original behavior.
    TradingEngine(MarketManager& markets, EventDispatcher& dispatcher,
                  CommandJournal& journal, CommittedCommandSink& sink)
        : markets_(markets), dispatcher_(dispatcher), submission_gate_(&journal),
          command_journal_(&journal), committed_sink_(&sink) {
        if (!sink.available() || markets.symbol_count() == 0)
            throw std::invalid_argument("Publication requires an available sink and instruments");
        publication_books_.reserve(markets.symbol_count());
        for (size_t i = 0; i < markets.symbol_count(); ++i) {
            const auto* book = markets.find_book(InstrumentId(static_cast<uint32_t>(i)));
            if (!book->event_recording_enabled() || !book->ready_for_commands() ||
                    book->active_order_count() != 0)
                throw std::invalid_argument("Publication requires empty event-enabled ready books");
            publication_books_.push_back(book);
            command_event_limit_ = std::max(command_event_limit_, std::max(book->capacity(), size_t{1}));
        }
        command_events_.reserve(command_event_limit_);
    }

    MarketProcessResult submit(Timestamp timestamp, const Symbol& symbol,
                               const Command& command) {
        return submit_impl(timestamp, symbol, markets_.resolve(symbol), command);
    }

    MarketProcessResult submit(Timestamp timestamp, InstrumentId instrument_id,
                               const Command& command) {
        const Symbol* symbol = markets_.symbol(instrument_id);
        if (!symbol) {
            if (failed_ || submitting_ || !publication_ready() ||
                    (submission_gate_ && !submission_gate_->available())) return unavailable();
            return MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return submit_impl(timestamp, *symbol, instrument_id, command);
    }

private:
    static size_t journal_record_bound(const OrderBook* book, const Command& command) noexcept {
        const auto* order = std::get_if<NewOrderCommand>(&command);
        if (!book || !order || order->time_in_force == TimeInForce::POST_ONLY) return 2;
        const auto opposite = order->side == Side::BUY ? book->best_ask() : book->best_bid();
        const bool crosses = opposite && (order->side == Side::BUY
            ? order->price >= *opposite : order->price <= *opposite);
        // Input + at most one trade per passive order + final result.
        // This conservative bound can reject before business validation; it
        // never scans the match path or mutates state to discover capacity.
        if (!crosses) return 2;
        const size_t count = book->active_order_count();
        return count > std::numeric_limits<size_t>::max() - 2
            ? std::numeric_limits<size_t>::max() : count + 2;
    }

    MarketProcessResult submit_impl(Timestamp timestamp, const Symbol& symbol,
                                    InstrumentId instrument_id,
                                    const Command& command) {
        const OrderBook* book = markets_.find_book(instrument_id);
        if (failed_ || submitting_ || !publication_ready() ||
                (submission_gate_ && !submission_gate_->available()) ||
                (book && !book->ready_for_commands())) {
            // Admission failure emits nothing, including no outcome callback:
            // retrying a callback here could itself recurse or duplicate output.
            return unavailable();
        }
        SubmissionScope submission(submitting_);
        try {
            command_events_.clear();
            if (command_journal_ && !command_journal_->begin_command(
                    timestamp, symbol, command, journal_record_bound(book, command))) {
                return unavailable();
            }
            return submit_active(timestamp, symbol, instrument_id, command);
        } catch (...) {
            // A Dispatcher callback may have reached only some subscribers.
            // Do not retry it, publish a synthetic success, or admit more input.
            fail_output();
            if (command_journal_) command_journal_->abort_command();
            throw;
        }
    }

    MarketProcessResult submit_active(Timestamp timestamp, const Symbol& symbol,
                                      InstrumentId instrument_id,
                                      const Command& command) {
        dispatcher_.publish(SystemEvent::order(timestamp, symbol, command, instrument_id));
        if (!publication_ready()) {
            if (command_journal_) command_journal_->abort_command();
            return unavailable();
        }

        class DispatcherSink {
            EventDispatcher& dispatcher_;
            Timestamp timestamp_;
            const Symbol& symbol_;
            InstrumentId instrument_id_;
            CommandJournal* journal_;
            std::vector<EngineEvent>* staged_;
            size_t limit_;
        public:
            DispatcherSink(EventDispatcher& dispatcher, Timestamp timestamp,
                           const Symbol& symbol, InstrumentId instrument_id,
                           CommandJournal* journal, std::vector<EngineEvent>* staged,
                           size_t limit) noexcept
                : dispatcher_(dispatcher), timestamp_(timestamp), symbol_(symbol),
                  instrument_id_(instrument_id), journal_(journal), staged_(staged), limit_(limit) {}
            bool push(const EngineEvent& event) {
                if (journal_) {
                    if (const auto* trade = std::get_if<TradeEvent>(&event)) {
                        if (!journal_->append_trade(*trade)) return false;
                    }
                }
                if (staged_) {
                    if (staged_->size() == limit_) return false;
                    staged_->push_back(event);
                }
                dispatcher_.publish(SystemEvent::engine(
                    timestamp_, symbol_, event, instrument_id_));
                return true;
            }
        } sink(dispatcher_, timestamp, symbol, instrument_id, command_journal_,
               committed_sink_ ? &command_events_ : nullptr, command_event_limit_);

        const auto start = std::chrono::steady_clock::now();
        const MarketProcessResult result = instrument_id.valid()
            ? markets_.process(instrument_id, command, sink)
            : MarketProcessResult{RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        const auto finish = std::chrono::steady_clock::now();
        const uint64_t latency_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
        if (result.process_result.status == ProcessStatus::OUTPUT_BLOCKED) {
            // Defensive: control-plane mutation during ORDER callbacks is not
            // supported. Once input was published, fail closed rather than
            // label this as a sequence rejection and continue.
            fail_output();
            if (command_journal_) command_journal_->abort_command();
            return unavailable();
        }
        if (!result.process_result.event_output_complete) {
            fail_output();
            if (command_journal_) command_journal_->abort_command();
            return result; // Applied state is not mislabeled as unadmitted.
        }
        if (!result.routed()) {
            if (command_journal_ && !command_journal_->commit_command(CommandOutcome::UNKNOWN_SYMBOL)) {
                fail_output();
                command_journal_->abort_command();
                auto incomplete = result;
                incomplete.process_result.event_output_complete = false;
                return incomplete;
            }
            if (!publish_committed(timestamp, instrument_id, command, CommandOutcome::UNKNOWN_SYMBOL)) {
                auto incomplete = result;
                incomplete.process_result.event_output_complete = false;
                return incomplete;
            }
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
        if (command_journal_ && !command_journal_->commit_command(outcome)) {
            fail_output();
            command_journal_->abort_command();
            auto incomplete = result;
            incomplete.process_result.event_output_complete = false;
            return incomplete;
        }
        if (!publish_committed(timestamp, instrument_id, command, outcome)) {
            auto incomplete = result;
            incomplete.process_result.event_output_complete = false;
            return incomplete;
        }
        dispatcher_.publish(SystemEvent::processing_latency(
            timestamp, symbol,
            ProcessingLatencyEvent{latency_ns, outcome,
                get_command_sequence(command), get_order_id(command)}, instrument_id));
        return result;
    }

public:

    void publish_market_data(Timestamp timestamp, const Symbol& symbol,
                             const MarketDataEvent& market_data) {
        if (failed_) throw std::logic_error("TradingEngine output is failed");
        try {
            dispatcher_.publish(
                SystemEvent::market_data(timestamp, symbol, market_data));
        } catch (...) {
            fail_output();
            throw;
        }
    }

    bool failed() const noexcept { return failed_; }

    MarketManager& market_manager() noexcept { return markets_; }
    const MarketManager& market_manager() const noexcept { return markets_; }
    EventDispatcher& event_dispatcher() noexcept { return dispatcher_; }
};

#endif
