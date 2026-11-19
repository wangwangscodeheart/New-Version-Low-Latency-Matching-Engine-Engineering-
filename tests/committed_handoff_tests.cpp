#include "committed_command_sink.hpp"
#include "trading_engine.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {

using Trace = std::vector<std::string>;

Command order(uint64_t sequence, uint64_t id, Side side = Side::BUY,
              uint64_t quantity = 1, int64_t price = 1'000'000) {
    return NewOrderCommand(CommandSequence(sequence), OrderId(id), side,
        Price(price), Quantity(quantity));
}

class FaultJournal final : public CommandJournal {
    Trace& trace_;
public:
    enum class Fault { NONE, BEGIN_FALSE, APPEND_FALSE, COMMIT_FALSE, COMMIT_THROW };
    Fault fault = Fault::NONE;
    bool healthy = true;
    bool active = false;
    size_t begins = 0, appends = 0, commit_attempts = 0, commits = 0, aborts = 0;
    size_t bound = 0;
    std::vector<CommandOutcome> outcomes;

    explicit FaultJournal(Trace& trace) : trace_(trace) {}
    bool available() const noexcept override { return healthy; }
    bool begin_command(Timestamp, const Symbol&, const Command&, size_t max_records) override {
        trace_.push_back("journal.begin");
        ++begins;
        bound = max_records;
        if (fault == Fault::BEGIN_FALSE) { healthy = false; return false; }
        TEST_ASSERT(!active);
        active = true;
        return true;
    }
    bool append_trade(const TradeEvent&) override {
        trace_.push_back("journal.append");
        ++appends;
        TEST_ASSERT(active);
        if (fault == Fault::APPEND_FALSE) { healthy = false; return false; }
        return true;
    }
    bool commit_command(CommandOutcome outcome) override {
        trace_.push_back("journal.commit");
        ++commit_attempts;
        TEST_ASSERT(active);
        outcomes.push_back(outcome);
        if (fault == Fault::COMMIT_THROW) throw std::runtime_error("injected commit failure");
        if (fault == Fault::COMMIT_FALSE) { healthy = false; return false; }
        ++commits;
        active = false;
        return true;
    }
    void abort_command() noexcept override {
        if (active) {
            trace_.push_back("journal.abort");
            ++aborts;
            active = false;
            healthy = false;
        }
    }
};

struct Batch {
    Timestamp timestamp;
    InstrumentId instrument;
    Command command;
    CommandOutcome outcome;
    std::vector<EngineEvent> events;
};

class FaultSink final : public CommittedCommandSink {
    Trace& trace_;
public:
    enum class Fault { NONE, RETURN_FALSE, THROW };
    Fault fault = Fault::NONE;
    bool healthy = true;
    size_t calls = 0, accepted = 0, invalidations = 0;
    std::vector<Batch> batches;

    explicit FaultSink(Trace& trace) : trace_(trace) {}
    bool available() const noexcept override { return healthy; }
    bool publish(Timestamp timestamp, InstrumentId instrument, const Command& command,
                 CommandOutcome outcome, const std::vector<EngineEvent>& events) override {
        trace_.push_back("public.publish");
        ++calls;
        batches.push_back(Batch{timestamp, instrument, command, outcome, events});
        if (fault == Fault::THROW) throw std::runtime_error("injected handoff failure");
        if (fault == Fault::RETURN_FALSE) return false;
        ++accepted;
        return true;
    }
    void invalidate() noexcept override {
        trace_.push_back("public.invalidate");
        ++invalidations;
        healthy = false;
    }
};

struct Fixture {
    Trace trace;
    MarketManager markets;
    EventDispatcher private_dispatcher;
    FaultJournal journal;
    FaultSink sink;
    TradingEngine engine;
    std::vector<EngineEvent> raw_events;
    size_t latencies = 0;
    EventDispatcher::Subscription observation;

    explicit Fixture(size_t capacity = 4)
        : markets(capacity), journal(trace), sink(trace),
          engine(markets, private_dispatcher, journal, sink),
          observation(private_dispatcher.subscribe_all([this](const SystemEvent& event) {
              switch (event.event_type) {
                  case SystemEventType::ORDER: trace.push_back("raw.order"); break;
                  case SystemEventType::TRADE: trace.push_back("raw.trade"); break;
                  case SystemEventType::ORDER_RESTED: trace.push_back("raw.rested"); break;
                  case SystemEventType::CANCEL: trace.push_back("raw.cancel"); break;
                  case SystemEventType::ORDER_REJECTED: trace.push_back("raw.rejected"); break;
                  case SystemEventType::PROCESSING_LATENCY:
                      trace.push_back("raw.latency"); ++latencies; break;
                  default: break;
              }
              if (const auto* value = std::get_if<EngineEvent>(&event.payload))
                  raw_events.push_back(*value);
          })) {}

    MarketProcessResult submit(const Command& command, uint64_t timestamp = 10) {
        return engine.submit(Timestamp(timestamp), Symbol("AAPL"), command);
    }

    void clear_observations() {
        trace.clear();
        raw_events.clear();
        latencies = 0;
        journal.begins = journal.appends = journal.commit_attempts = journal.commits = journal.aborts = 0;
        journal.outcomes.clear();
        sink.calls = sink.accepted = sink.invalidations = 0;
        sink.batches.clear();
    }

    void expect_terminal() {
        TEST_ASSERT(engine.failed());
        TEST_ASSERT(sink.invalidations != 0 && !sink.available());
        const auto before = markets.find_book(Symbol("AAPL"))->capture_state();
        const auto other_before = markets.find_book(Symbol("TSLA"))->capture_state();
        const auto before_trace = trace;
        const size_t before_begins = journal.begins;
        const size_t before_calls = sink.calls;
        // Recovery of the external sink alone must not restart a failed engine.
        sink.healthy = true;
        const auto result = engine.submit(Timestamp(99), markets.resolve(Symbol("TSLA")), order(1, 99));
        TEST_ASSERT(result.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
        TEST_ASSERT(result.process_result.status == ProcessStatus::OUTPUT_BLOCKED);
        TEST_ASSERT(journal.begins == before_begins && sink.calls == before_calls);
        TEST_ASSERT(trace == before_trace);
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->capture_state() == before);
        TEST_ASSERT(markets.find_book(Symbol("TSLA"))->capture_state() == other_before);
    }
};

void test_commit_precedes_publication_of_the_complete_maximum_batch() {
    Fixture f(3);
    TEST_ASSERT(f.submit(order(1, 11, Side::SELL)).process_result.applied());
    TEST_ASSERT(f.submit(order(2, 12, Side::SELL)).process_result.applied());
    f.clear_observations();

    const auto result = f.submit(order(3, 13, Side::BUY, 3), 1234);
    TEST_ASSERT(result.process_result.applied() && result.process_result.event_output_complete);
    const Trace expected{"journal.begin", "raw.order", "journal.append", "raw.trade",
        "journal.append", "raw.trade", "raw.rested", "journal.commit", "public.publish", "raw.latency"};
    TEST_ASSERT(f.trace == expected);
    TEST_ASSERT(f.journal.commits == 1 && f.journal.appends == 2 && f.sink.accepted == 1);
    TEST_ASSERT(f.journal.outcomes.front() == CommandOutcome::APPLIED);
    TEST_ASSERT(f.sink.batches.size() == 1);
    const auto& batch = f.sink.batches.front();
    TEST_ASSERT(batch.timestamp == Timestamp(1234));
    TEST_ASSERT(batch.instrument == f.markets.resolve(Symbol("AAPL")));
    TEST_ASSERT(get_command_sequence(batch.command) == CommandSequence(3));
    TEST_ASSERT(get_order_id(batch.command) == OrderId(13));
    TEST_ASSERT(batch.outcome == CommandOutcome::APPLIED);
    TEST_ASSERT(batch.events == f.raw_events && batch.events.size() == 3);
    TEST_ASSERT(std::get<TradeEvent>(batch.events[0]).passive_order_id == OrderId(11));
    TEST_ASSERT(std::get<TradeEvent>(batch.events[1]).passive_order_id == OrderId(12));
    TEST_ASSERT(std::get<OrderRestedEvent>(batch.events[2]).remaining_quantity == Quantity(1));
    for (size_t i = 0; i < batch.events.size(); ++i) {
        TEST_ASSERT(get_command_sequence(batch.events[i]) == CommandSequence(3));
        TEST_ASSERT(get_event_index(batch.events[i]) == EventIndex(static_cast<uint32_t>(i)));
    }
    TEST_ASSERT(f.latencies == 1 && !f.engine.failed());

    // A command with no core output cannot accidentally reuse the previous batch.
    f.clear_observations();
    const auto duplicate = f.submit(order(3, 13));
    TEST_ASSERT(duplicate.process_result.status == ProcessStatus::SEQUENCE_REJECTED);
    TEST_ASSERT(f.sink.batches.size() == 1 && f.sink.batches.front().events.empty());
    TEST_ASSERT(f.sink.batches.front().outcome == CommandOutcome::SEQUENCE_REJECTED);
}

void test_zero_capacity_still_hands_off_the_business_rejection() {
    Fixture f(0);
    const auto result = f.submit(order(1, 1));
    TEST_ASSERT(result.process_result.status == ProcessStatus::REJECTED);
    TEST_ASSERT(result.process_result.event_output_complete && !f.engine.failed());
    TEST_ASSERT(f.sink.batches.size() == 1);
    TEST_ASSERT(f.sink.batches.front().outcome == CommandOutcome::BUSINESS_REJECTED);
    TEST_ASSERT(f.sink.batches.front().events == f.raw_events);
    TEST_ASSERT(f.sink.batches.front().events.size() == 1);
    TEST_ASSERT(std::get<OrderRejectedEvent>(f.sink.batches.front().events.front()).reason == RejectReason::POOL_EXHAUSTED);
}

void test_failed_begin_has_no_matching_or_handoff_and_is_retryable() {
    Fixture f;
    f.journal.fault = FaultJournal::Fault::BEGIN_FALSE;
    const auto before = f.markets.find_book(Symbol("AAPL"))->capture_state();
    const auto result = f.submit(order(1, 1));
    TEST_ASSERT(result.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(f.journal.begins == 1 && f.journal.commit_attempts == 0);
    TEST_ASSERT(f.raw_events.empty() && f.latencies == 0 && f.sink.calls == 0);
    TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->capture_state() == before);
    TEST_ASSERT(!f.engine.failed() && f.sink.invalidations == 0);
    f.journal.fault = FaultJournal::Fault::NONE;
    f.journal.healthy = true;
    TEST_ASSERT(f.submit(order(1, 1)).process_result.applied());
    TEST_ASSERT(f.journal.begins == 2 && f.journal.commits == 1 && f.sink.accepted == 1);
}

void test_commit_false_preserves_applied_state_without_public_handoff() {
    Fixture f;
    f.journal.fault = FaultJournal::Fault::COMMIT_FALSE;
    const auto result = f.submit(order(1, 1));
    TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
    TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
    TEST_ASSERT(f.raw_events.size() == 1 && f.journal.commit_attempts == 1);
    TEST_ASSERT(f.journal.commits == 0 && f.journal.aborts == 1);
    TEST_ASSERT(f.sink.calls == 0 && f.latencies == 0);
    f.expect_terminal();
}

void test_append_false_never_reaches_commit_or_public_handoff() {
    Fixture f;
    TEST_ASSERT(f.submit(order(1, 1, Side::SELL)).process_result.applied());
    f.clear_observations();
    f.journal.fault = FaultJournal::Fault::APPEND_FALSE;
    const auto result = f.submit(order(2, 2));
    TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
    TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    TEST_ASSERT(f.journal.appends == 1 && f.journal.commit_attempts == 0 && f.journal.aborts == 1);
    TEST_ASSERT(f.raw_events.empty() && f.sink.calls == 0 && f.latencies == 0);
    f.expect_terminal();
}

void test_throwing_commit_is_terminal_without_public_handoff() {
    Fixture f;
    f.journal.fault = FaultJournal::Fault::COMMIT_THROW;
    bool threw = false;
    try { (void)f.submit(order(1, 1)); }
    catch (const std::runtime_error&) { threw = true; }
    TEST_ASSERT(threw && f.markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
    TEST_ASSERT(f.journal.commit_attempts == 1 && f.journal.commits == 0 && f.journal.aborts == 1);
    TEST_ASSERT(f.sink.calls == 0 && f.latencies == 0);
    f.expect_terminal();
}

void test_raw_callback_exceptions_never_reach_public_handoff() {
    for (const auto point : {SystemEventType::ORDER, SystemEventType::ORDER_RESTED,
                             SystemEventType::TRADE}) {
        Fixture f;
        if (point == SystemEventType::TRADE) {
            TEST_ASSERT(f.submit(order(1, 1, Side::SELL)).process_result.applied());
            f.clear_observations();
        }
        auto failing = f.private_dispatcher.subscribe(point, [](const SystemEvent&) {
            throw std::runtime_error("injected private callback failure");
        });
        f.private_dispatcher.freeze();
        bool threw = false;
        try { (void)f.submit(point == SystemEventType::TRADE ? order(2, 2) : order(1, 1)); }
        catch (const std::runtime_error&) { threw = true; }
        TEST_ASSERT(threw && f.journal.begins == 1 && f.journal.aborts == 1);
        TEST_ASSERT(f.journal.commit_attempts == 0 && f.sink.calls == 0 && f.latencies == 0);
        TEST_ASSERT(f.journal.appends == (point == SystemEventType::TRADE ? 1u : 0u));
        TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->active_order_count() ==
            (point == SystemEventType::ORDER_RESTED ? 1u : 0u));
        f.expect_terminal();
    }
}

void test_sink_failures_happen_only_after_commit_and_have_no_success_latency() {
    for (const auto fault : {FaultSink::Fault::RETURN_FALSE, FaultSink::Fault::THROW}) {
        Fixture f;
        f.sink.fault = fault;
        bool threw = false;
        try {
            const auto result = f.submit(order(1, 1));
            TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
        } catch (const std::runtime_error&) { threw = true; }
        TEST_ASSERT(threw == (fault == FaultSink::Fault::THROW));
        TEST_ASSERT(f.journal.commits == 1 && f.journal.aborts == 0 && !f.journal.active);
        TEST_ASSERT(f.sink.calls == 1 && f.sink.accepted == 0 && f.sink.batches.size() == 1);
        TEST_ASSERT(f.sink.batches.front().outcome == CommandOutcome::APPLIED);
        TEST_ASSERT(f.sink.batches.front().events == f.raw_events && f.raw_events.size() == 1);
        const Trace expected{"journal.begin", "raw.order", "raw.rested", "journal.commit",
            "public.publish", "public.invalidate"};
        TEST_ASSERT(f.trace == expected && f.latencies == 0);
        TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
        f.expect_terminal();
    }
}

void test_unavailable_sink_is_rejected_before_journal_begin() {
    Fixture f;
    f.sink.healthy = false;
    const auto before = f.markets.find_book(Symbol("AAPL"))->capture_state();
    const auto result = f.submit(order(1, 1));
    TEST_ASSERT(result.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(f.journal.begins == 0 && f.sink.calls == 0 && f.raw_events.empty() && f.latencies == 0);
    TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->capture_state() == before);
    f.expect_terminal();
}

void test_new_constructor_requires_recording_empty_ready_books() {
    InstrumentRuntimeConfig config;
    config.order_capacity = 4;
    config.record_events = false;
    // Both existing construction paths retain support for non-recording books.
    for (bool explicit_journal : {false, true}) {
        MarketManager markets(EmptyMarketManagerTag{});
        TEST_ASSERT(markets.register_symbol(Symbol("AAPL"), config));
        EventDispatcher dispatcher;
        Trace trace;
        FaultJournal journal(trace);
        if (explicit_journal) {
            TradingEngine engine(markets, dispatcher, journal);
            TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1)).process_result.applied());
        } else {
            TradingEngine engine(markets, dispatcher);
            TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1)).process_result.applied());
        }
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        MarketManager markets(EmptyMarketManagerTag{});
        config.record_events = mode != 0;
        TEST_ASSERT(markets.register_symbol(Symbol("AAPL"), config));
        if (mode == 1)
            TEST_ASSERT(markets.process(Symbol("AAPL"), order(1, 1)).process_result.applied());
        if (mode == 2) {
            FixedEventBuffer<0> full;
            const auto result = markets.process(Symbol("AAPL"), order(1, 1, Side::BUY, 1, 0), full);
            TEST_ASSERT(!result.process_result.event_output_complete);
            TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
            TEST_ASSERT(!markets.find_book(Symbol("AAPL"))->ready_for_commands());
        }
        EventDispatcher dispatcher;
        Trace trace;
        FaultJournal journal(trace);
        FaultSink sink(trace);
        bool rejected = false;
        try { TradingEngine engine(markets, dispatcher, journal, sink); }
        catch (const std::logic_error&) { rejected = true; }
        TEST_ASSERT(rejected && journal.begins == 0 && sink.calls == 0);
    }
}

void test_registry_growth_and_book_replacement_are_rejected_before_admission() {
    for (bool replace : {false, true}) {
        Fixture f;
        if (replace)
            f.markets.install_restored_book(Symbol("AAPL"), std::make_unique<OrderBook>(4));
        else {
            InstrumentRuntimeConfig config;
            config.order_capacity = 4;
            TEST_ASSERT(f.markets.register_symbol(Symbol("AMD"), config));
        }
        const auto before = f.markets.find_book(Symbol("AAPL"))->capture_state();
        const auto result = f.submit(order(1, 1));
        TEST_ASSERT(result.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
        TEST_ASSERT(f.journal.begins == 0 && f.sink.calls == 0 && f.raw_events.empty() && f.latencies == 0);
        TEST_ASSERT(f.markets.find_book(Symbol("AAPL"))->capture_state() == before);
        f.expect_terminal();
    }
}

} // namespace

int main() {
    try {
        test_commit_precedes_publication_of_the_complete_maximum_batch();
        test_zero_capacity_still_hands_off_the_business_rejection();
        test_failed_begin_has_no_matching_or_handoff_and_is_retryable();
        test_commit_false_preserves_applied_state_without_public_handoff();
        test_append_false_never_reaches_commit_or_public_handoff();
        test_throwing_commit_is_terminal_without_public_handoff();
        test_raw_callback_exceptions_never_reach_public_handoff();
        test_sink_failures_happen_only_after_commit_and_have_no_success_latency();
        test_unavailable_sink_is_rejected_before_journal_begin();
        test_new_constructor_requires_recording_empty_ready_books();
        test_registry_growth_and_book_replacement_are_rejected_before_admission();
        std::cout << "Committed handoff tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Committed handoff tests failed: " << error.what() << '\n';
        return 1;
    }
}
