#include "async_journal.hpp"
#include "command_journal.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {

Command order(uint64_t sequence, uint64_t id, Side side = Side::BUY,
              uint64_t quantity = 1, int64_t price = 1'000'000) {
    return NewOrderCommand(CommandSequence(sequence), OrderId(id), side, Price(price), Quantity(quantity));
}

class FaultJournal final : public CommandJournal {
public:
    enum class Fault { NONE, BEGIN, TRADE, COMMIT };
    Fault fault = Fault::NONE;
    bool active = false;
    bool healthy = true;
    size_t begins = 0, trades = 0, commits = 0, aborts = 0, bound = 0;
    bool available() const noexcept override { return healthy; }
    bool begin_command(Timestamp, const Symbol&, const Command&, size_t max_records) override {
        ++begins;
        bound = max_records;
        if (fault == Fault::BEGIN) { healthy = false; return false; }
        active = true;
        return true;
    }
    bool append_trade(const TradeEvent&) override {
        ++trades;
        if (fault == Fault::TRADE) { healthy = false; return false; }
        return true;
    }
    bool commit_command(CommandOutcome) override {
        if (fault == Fault::COMMIT) { healthy = false; return false; }
        ++commits;
        active = false;
        return true;
    }
    void abort_command() noexcept override {
        if (active) { ++aborts; active = false; healthy = false; }
    }
};

void test_reservation_failure_is_before_any_publication_or_book_change() {
    MarketManager markets(8);
    EventDispatcher dispatcher;
    FaultJournal journal;
    journal.fault = FaultJournal::Fault::BEGIN;
    TradingEngine engine(markets, dispatcher, journal);
    size_t events = 0;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent&) { ++events; });
    const auto before = markets.find_book(Symbol("AAPL"))->capture_state();
    const auto result = engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1));
    TEST_ASSERT(result.routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(result.process_result.status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(events == 0 && journal.begins == 1 && journal.commits == 0);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->capture_state() == before);
}

void test_commit_failure_does_not_claim_the_command_was_unapplied() {
    MarketManager markets(8);
    EventDispatcher dispatcher;
    FaultJournal journal;
    journal.fault = FaultJournal::Fault::COMMIT;
    TradingEngine engine(markets, dispatcher, journal);
    size_t outcomes = 0;
    auto subscription = dispatcher.subscribe(SystemEventType::PROCESSING_LATENCY,
        [&](const SystemEvent&) { ++outcomes; });
    const auto result = engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1));
    TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
    TEST_ASSERT(engine.failed() && journal.aborts == 1 && journal.commits == 0);
    TEST_ASSERT(outcomes == 0);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 1);
    TEST_ASSERT(engine.submit(Timestamp(2), Symbol("TSLA"), order(1, 2)).routing_status ==
        RoutingStatus::SYSTEM_UNAVAILABLE);
}

void test_trade_append_failure_retains_the_core_event_and_stops() {
    MarketManager markets(8);
    EventDispatcher dispatcher;
    FaultJournal journal;
    TradingEngine engine(markets, dispatcher, journal);
    TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1, Side::SELL)).process_result.applied());
    journal.fault = FaultJournal::Fault::TRADE;
    size_t published_trades = 0;
    auto subscription = dispatcher.subscribe(SystemEventType::TRADE,
        [&](const SystemEvent&) { ++published_trades; });
    const auto result = engine.submit(Timestamp(2), Symbol("AAPL"), order(2, 2));
    TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
    const auto* book = markets.find_book(Symbol("AAPL"));
    TEST_ASSERT(book->active_order_count() == 0 && book->pending_event_count() == 1);
    TEST_ASSERT(journal.bound == 3 && journal.commits == 1 && journal.aborts == 1);
    TEST_ASSERT(engine.failed() && published_trades == 0);
}

void test_observer_failure_aborts_only_uncommitted_groups() {
    for (SystemEventType point : {SystemEventType::ORDER, SystemEventType::PROCESSING_LATENCY}) {
        MarketManager markets(8);
        EventDispatcher dispatcher;
        FaultJournal journal;
        TradingEngine engine(markets, dispatcher, journal);
        auto subscription = dispatcher.subscribe(point, [](const SystemEvent&) {
            throw std::runtime_error("injected observer failure");
        });
        bool threw = false;
        try { engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1)); }
        catch (const std::runtime_error&) { threw = true; }
        TEST_ASSERT(threw && engine.failed());
        TEST_ASSERT(journal.commits == (point == SystemEventType::ORDER ? 0u : 1u));
        TEST_ASSERT(journal.aborts == (point == SystemEventType::ORDER ? 1u : 0u));
        TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() ==
            (point == SystemEventType::ORDER ? 0u : 1u));
    }
}

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* name) : path(std::filesystem::temp_directory_path() / name) {}
    ~TempFile() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};

void test_real_writer_rejects_too_small_budget_without_mutation() {
    TempFile file("m1_command_budget.journal.csv");
    MarketManager markets(8);
    EventDispatcher dispatcher;
    AsyncJournalWriter journal(file.path.string(), 1);
    TradingEngine engine(markets, dispatcher, journal);
    const auto before = markets.find_book(Symbol("AAPL"))->capture_state();
    TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1)).routing_status ==
        RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->capture_state() == before);
    journal.stop();
    TEST_ASSERT(journal.accepted_count() == 0 && journal.written_count() == 0);
    TEST_ASSERT(journal.health() == JournalHealth::QUEUE_OVERFLOW);
    TEST_ASSERT(Journal::load(file.path.string()).empty());
}

void test_canonical_roundtrip_and_replay_preflight() {
    TempFile file("m1_command_roundtrip.journal.csv");
    MarketManager markets(8);
    EventDispatcher dispatcher;
    AsyncJournalWriter journal(file.path.string(), 128);
    TradingEngine engine(markets, dispatcher, journal);
    engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1, Side::SELL, 2));
    engine.submit(Timestamp(2), Symbol("TSLA"), order(1, 2));
    engine.submit(Timestamp(3), Symbol("AAPL"), order(2, 3));
    engine.submit(Timestamp(4), Symbol("AAPL"), CancelOrderCommand(CommandSequence(3), OrderId(1)));
    engine.submit(Timestamp(5), Symbol("NVDA"), order(1, 4, Side::BUY, 1, 0));
    engine.submit(Timestamp(6), Symbol("MSFT"), order(1, 5));
    engine.submit(Timestamp(7), Symbol("AAPL"), CancelOrderCommand(CommandSequence(3), OrderId(1)));
    journal.stop();
    TEST_ASSERT(journal.healthy() && journal.accepted_count() == 15 && journal.written_count() == 15);
    auto records = Journal::load(file.path.string());
    TEST_ASSERT(records.size() == 15);
    for (const auto& record : records) TEST_ASSERT(record.format_version == 2);
    Journal::validate_committed_records(records);
    MarketManager restored(8);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(restored, replay_dispatcher);
    TEST_ASSERT(JournalReplayEngine::replay(records, replay_engine, replay_dispatcher, &markets).success());

    // A syntactically valid vector containing only an input is NOT a commit.
    records.resize(1);
    MarketManager untouched(8);
    EventDispatcher empty_dispatcher;
    TradingEngine untouched_engine(untouched, empty_dispatcher);
    bool rejected = false;
    try { JournalReplayEngine::replay(records, untouched_engine, empty_dispatcher); }
    catch (const std::runtime_error&) { rejected = true; }
    TEST_ASSERT(rejected && untouched.find_book(Symbol("AAPL"))->active_order_count() == 0);
    TEST_ASSERT(empty_dispatcher.subscriber_count(SystemEventType::TRADE) == 0);
}

JournalRecord input(uint64_t sequence, uint64_t command) {
    JournalRecord record{sequence, Timestamp(command), JournalRecordType::ORDER_INSERT,
        Symbol("AAPL"), CommandSequence(command), OrderId(command), Side::BUY,
        Price(1'000'000), Quantity(1), OrderId(0), OrderId(0)};
    record.format_version = 2;
    return record;
}

JournalRecord outcome(uint64_t sequence, uint64_t command) {
    auto record = input(sequence, command);
    record.type = JournalRecordType::COMMAND_RESULT;
    record.price = Price(0);
    record.quantity = Quantity(0);
    return record;
}

void write_prefix(std::ostream& out) {
    write_command_journal_header(out);
    write_journal_record(out, input(1, 1));
    write_journal_record(out, outcome(2, 1));
}

void test_incomplete_new_format_discards_whole_final_command() {
    TempFile file("m1_command_tail.journal.csv");
    for (unsigned mode = 0; mode < 3; ++mode) {
        {
            std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
            write_prefix(out);
            auto second = input(3, 2);
            second.side = Side::SELL;
            write_journal_record(out, second);
            JournalRecord trade{4, Timestamp(2), JournalRecordType::TRADE, Symbol("AAPL"),
                CommandSequence(2), OrderId(0), Side::BUY, Price(1'000'000), Quantity(1),
                OrderId(1), OrderId(2)};
            trade.format_version = 2;
            write_journal_record(out, trade);
            if (mode == 1) out << "5,2,COMMAND_RESULT,AAPL,2";
            if (mode == 2) {
                std::ostringstream line;
                write_journal_record(line, outcome(5, 2));
                std::string text = line.str();
                text.pop_back(); // No final newline: not a complete written row.
                out << text;
            }
        }
        bool rejected = false;
        try { (void)Journal::load(file.path.string()); }
        catch (const std::runtime_error&) { rejected = true; }
        TEST_ASSERT(rejected);
        const auto recovered = Journal::load(file.path.string(), true);
        TEST_ASSERT(recovered.size() == 2);
        TEST_ASSERT(recovered.back().type == JournalRecordType::COMMAND_RESULT);
    }
}

void test_new_format_corruption_is_not_a_tolerated_tail() {
    TempFile file("m1_command_corrupt.journal.csv");
    for (unsigned mode = 0; mode < 4; ++mode) {
        {
            std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
            write_prefix(out);
            write_journal_record(out, input(3, 2));
            if (mode == 0) out << "broken,middle,row\n";
            auto closing = outcome(4, 2);
            if (mode == 1) closing.order_id = OrderId(99);
            if (mode == 2) closing.sequence = 5;
            if (mode == 3) closing.symbol = Symbol("TSLA");
            write_journal_record(out, closing);
        }
        bool rejected = false;
        try { (void)Journal::load(file.path.string(), true); }
        catch (const std::runtime_error&) { rejected = true; }
        TEST_ASSERT(rejected);
    }
}

void test_queue_budget_includes_the_writer_owned_buffer() {
    JournalBatchQueue queue(4);
    std::vector<JournalRecord> records;
    records.reserve(2);
    records.push_back(input(1, 1));
    records.push_back(outcome(2, 1));
    const size_t charge = records.capacity();
    TEST_ASSERT(queue.try_reserve(charge));
    TEST_ASSERT(queue.try_push(std::move(records), charge));
    JournalBatchQueue::Batch writing;
    TEST_ASSERT(queue.wait_pop(writing));
    TEST_ASSERT(queue.used_slots() == charge);
    TEST_ASSERT(!queue.try_reserve(queue.capacity()));
    queue.finish(writing);
    TEST_ASSERT(queue.used_slots() == 0);
    TEST_ASSERT(queue.try_reserve(4));
    queue.release(4);
    queue.close();
    TEST_ASSERT(!queue.try_reserve(1));
    TEST_ASSERT(!queue.wait_pop(writing));
}

void test_direct_v2_vectors_validate_before_replay() {
    const std::vector<JournalRecord> good{input(1, 1), outcome(2, 1)};
    for (unsigned mode = 0; mode < 5; ++mode) {
        auto records = good;
        if (mode == 0) records[0].side = static_cast<Side>(99);
        if (mode == 1) records[0].time_in_force = static_cast<TimeInForce>(99);
        if (mode == 2) records[0].format_version = 1;
        if (mode == 3) records[1].outcome = static_cast<CommandOutcome>(99);
        if (mode == 4) { records[0].format_version = 3; records[1].format_version = 3; }
        MarketManager markets(4);
        EventDispatcher dispatcher;
        TradingEngine engine(markets, dispatcher);
        bool rejected = false;
        try { JournalReplayEngine::replay(records, engine, dispatcher); }
        catch (const std::runtime_error&) { rejected = true; }
        TEST_ASSERT(rejected && markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    }
}

void test_observer_compatibility_failure_is_not_silent_or_partly_queued() {
    TempFile file("m1_command_observer_failure.journal.csv");
    MarketManager markets(4);
    markets.process(Symbol("AAPL"), order(1, 1, Side::SELL));
    EventDispatcher dispatcher;
    AsyncJournalWriter journal(file.path.string(), 2);
    journal.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);
    bool threw = false;
    try { engine.submit(Timestamp(2), Symbol("AAPL"), order(2, 2)); }
    catch (const std::runtime_error&) { threw = true; }
    TEST_ASSERT(threw && engine.failed());
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    journal.stop();
    TEST_ASSERT(!journal.healthy() && journal.accepted_count() == 0);
    TEST_ASSERT(journal.buffered_record_slots() == 0);
    TEST_ASSERT(Journal::load(file.path.string()).empty());
}

void test_mixed_binding_is_rejected_before_matching() {
    TempFile file("m1_command_mixed_binding.journal.csv");
    MarketManager markets(4);
    EventDispatcher dispatcher;
    AsyncJournalWriter journal(file.path.string(), 16);
    journal.attach(dispatcher);
    TradingEngine engine(markets, dispatcher, journal);
    TEST_ASSERT(engine.submit(Timestamp(1), Symbol("AAPL"), order(1, 1)).routing_status ==
        RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    journal.stop();
    TEST_ASSERT(!journal.healthy() && Journal::load(file.path.string()).empty());
}

} // namespace

int main() {
    try {
        test_reservation_failure_is_before_any_publication_or_book_change();
        test_commit_failure_does_not_claim_the_command_was_unapplied();
        test_trade_append_failure_retains_the_core_event_and_stops();
        test_observer_failure_aborts_only_uncommitted_groups();
        test_real_writer_rejects_too_small_budget_without_mutation();
        test_canonical_roundtrip_and_replay_preflight();
        test_incomplete_new_format_discards_whole_final_command();
        test_new_format_corruption_is_not_a_tolerated_tail();
        test_queue_budget_includes_the_writer_owned_buffer();
        test_direct_v2_vectors_validate_before_replay();
        test_observer_compatibility_failure_is_not_silent_or_partly_queued();
        test_mixed_binding_is_rejected_before_matching();
        std::cout << "Command journal tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Command journal tests failed: " << error.what() << '\n';
        return 1;
    }
}
