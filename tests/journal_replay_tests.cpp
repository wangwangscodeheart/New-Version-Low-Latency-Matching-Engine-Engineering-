#include "audit_log.hpp"
#include "journal.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <streambuf>
#include <string>
#include <limits>
#include <algorithm>
#include <chrono>
#include <iterator>

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

class AuditFailingBuffer : public std::streambuf {
    std::string bytes_;
    std::streamsize write(const char* data, std::streamsize size) {
        const size_t requested = static_cast<size_t>(size);
        const size_t take = std::min(requested, budget);
        bytes_.append(data, take);
        budget -= take;
        if (take != requested && throw_write) throw std::runtime_error("Injected audit write failure");
        return static_cast<std::streamsize>(take);
    }
public:
    size_t budget = 0;
    bool throw_write = false, fail_flush = false, throw_flush = false;
    unsigned sync_calls = 0;
    const std::string& str() const noexcept { return bytes_; }
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override { return write(data, size); }
    int_type overflow(int_type value = traits_type::eof()) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return write(&byte, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override {
        ++sync_calls;
        if (throw_flush) throw std::runtime_error("Injected audit flush failure");
        return fail_flush ? -1 : 0;
    }
};

class TemporaryAuditDirectory {
    std::filesystem::path base_, path_;
public:
    TemporaryAuditDirectory() : base_(std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base_ / ("dme_audit_export_" + std::to_string(stamp) + "_" + std::to_string(attempt));
            if (candidate.parent_path() != base_) throw std::runtime_error("Unsafe audit test path");
            if (std::filesystem::create_directory(candidate)) { path_ = candidate; return; }
        }
        throw std::runtime_error("Cannot reserve audit test directory");
    }
    ~TemporaryAuditDirectory() {
        try {
            if (!path_.empty() && path_.parent_path() == base_ &&
                path_.filename().string().rfind("dme_audit_export_", 0) == 0) {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } catch (...) {}
    }
    TemporaryAuditDirectory(const TemporaryAuditDirectory&) = delete;
    TemporaryAuditDirectory& operator=(const TemporaryAuditDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
};

const std::string audit_header = "timestamp,action,symbol,order_id,price,quantity,remaining,status\n";
const std::string audit_golden = audit_header +
    "100,CREATE,AAPL,1,1000000,10,10,CREATED\n"
    "100,SUBMIT,AAPL,1,1000000,10,10,SUBMITTED\n"
    "101,CREATE,TSLA,1,2000000,20,20,CREATED\n"
    "101,SUBMIT,TSLA,1,2000000,20,20,SUBMITTED\n"
    "102,CREATE,AAPL,2,1000000,4,4,CREATED\n"
    "102,SUBMIT,AAPL,2,1000000,4,4,SUBMITTED\n"
    "102,TRADE,AAPL,1,1000000,4,6,PARTIALLY_FILLED\n"
    "102,TRADE,AAPL,2,1000000,4,0,FILLED\n"
    "103,CANCEL,AAPL,1,1000000,6,0,CANCELLED\n";

std::string audit_text(const AuditLog& audit) {
    std::ostringstream output;
    audit.save(output);
    return output.str();
}

void test_audit_export_reports_failed_write() {
    AuditLog audit;
    AuditFailingBuffer buffer;
    std::ostream output(&buffer);
    bool threw = false;
    try { audit.save(output); } catch (const std::exception&) { threw = true; }
    std::cout << "audit export: stream_failed=" << output.fail() << " error_reported=" << threw << '\n';
    TEST_ASSERT(output.fail() && threw);
}

void run_scenario(TradingEngine& engine) {
    engine.submit(Timestamp(100), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(101), Symbol("TSLA"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(200), Quantity(20)));
    engine.submit(Timestamp(102), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(4)));
    engine.submit(Timestamp(103), Symbol("AAPL"),
        CancelOrderCommand(CommandSequence(3), OrderId(1)));
}

struct AuditScenario {
    MarketManager markets{8};
    EventDispatcher dispatcher;
    AuditLog audit;
    TradingEngine engine{markets, dispatcher};
    AuditScenario() { audit.attach(dispatcher); run_scenario(engine); }
};

void assert_audit_can_continue_after_failed_export(AuditScenario& scenario) {
    TEST_ASSERT(audit_text(scenario.audit) == audit_golden && scenario.audit.records().size() == 9);
    TEST_ASSERT(scenario.markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    TEST_ASSERT(scenario.markets.find_book(Symbol("TSLA"))->active_order_count() == 1);
    TEST_ASSERT(!scenario.engine.failed());
    scenario.engine.submit(Timestamp(104), Symbol("TSLA"), CancelOrderCommand(CommandSequence(2), OrderId(1)));
    const auto& last = scenario.audit.records().back();
    TEST_ASSERT(scenario.audit.records().size() == 10 && last.action == AuditAction::CANCEL);
    TEST_ASSERT(last.symbol == Symbol("TSLA") && last.quantity == Quantity(20) && last.remaining_quantity == Quantity(0));
    TEST_ASSERT(scenario.markets.find_book(Symbol("TSLA"))->check_invariants());
}

void test_audit_csv_golden_owned_and_borrowed_streams() {
    AuditScenario scenario;
    TEST_ASSERT(audit_text(scenario.audit) == audit_golden);
    TEST_ASSERT(audit_text(AuditLog{}) == audit_header);
    TemporaryAuditDirectory directory;
    const auto path = directory.path() / "audit.csv";
    scenario.audit.save(path.string());
    // Preserve existing text mode: CRLF on Windows reads back as LF here.
    std::ifstream file(path);
    TEST_ASSERT(file.good());
    const std::string restored((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    TEST_ASSERT(restored == audit_golden);
    file.close();
    std::ostringstream borrowed;
    scenario.audit.save(borrowed);
    borrowed << "caller owns stream\n";
    TEST_ASSERT(borrowed.good() && borrowed.str() == audit_golden + "caller owns stream\n");
    std::cout << "Audit CSV golden passed: 9 lifecycle records, owned/borrowed, empty header\n";
}

void test_audit_short_and_throwing_writes_preserve_memory_state() {
    const size_t after_first_row = audit_golden.find('\n', audit_header.size()) + 1;
    for (bool exception_mask : {false, true}) {
        for (bool throwing_buffer : {false, true}) {
            for (size_t budget : {size_t{0}, audit_header.size() - 1, after_first_row + 4}) {
                AuditScenario scenario;
                AuditFailingBuffer buffer;
                buffer.budget = budget;
                buffer.throw_write = throwing_buffer;
                std::ostream output(&buffer);
                if (exception_mask) output.exceptions(std::ios::badbit | std::ios::failbit);
                const auto previous_mask = output.exceptions();
                bool threw = false;
                try { scenario.audit.save(output); } catch (const std::exception&) { threw = true; }
                TEST_ASSERT(threw && output.fail() && output.exceptions() == previous_mask);
                TEST_ASSERT(buffer.str() == audit_golden.substr(0, budget));
                assert_audit_can_continue_after_failed_export(scenario);
            }
        }
    }
    std::cout << "Audit write failures passed: 12 cases; failed sink retained; healthy re-export/continued lifecycle\n";
}

void test_audit_flush_failure_and_exception_are_not_silent() {
    for (bool exception_mask : {false, true}) {
        for (bool throwing_buffer : {false, true}) {
            AuditScenario scenario;
            AuditFailingBuffer buffer;
            buffer.budget = std::numeric_limits<size_t>::max();
            buffer.fail_flush = !throwing_buffer;
            buffer.throw_flush = throwing_buffer;
            std::ostream output(&buffer);
            if (exception_mask) output.exceptions(std::ios::badbit | std::ios::failbit);
            bool threw = false;
            try { scenario.audit.save(output); } catch (const std::exception&) { threw = true; }
            TEST_ASSERT(threw && output.fail() && buffer.sync_calls == 1 && buffer.str() == audit_golden);
            assert_audit_can_continue_after_failed_export(scenario);
        }
    }
    std::cout << "Audit flush failures passed: 4 cases; complete bytes do not imply successful export\n";
}

void test_audit_filename_open_failure_is_visible_and_nonmutating() {
    TemporaryAuditDirectory directory;
    AuditScenario scenario;
    for (const auto& path : {directory.path(), directory.path() / "missing" / "audit.csv"}) {
        bool threw = false;
        try { scenario.audit.save(path.string()); } catch (const std::exception&) { threw = true; }
        TEST_ASSERT(threw && audit_text(scenario.audit) == audit_golden);
    }
    TEST_ASSERT(std::filesystem::is_empty(directory.path()));
    scenario.audit.save((directory.path() / "good.csv").string());
    assert_audit_can_continue_after_failed_export(scenario);
    std::cout << "Audit filename failures passed: directory/missing parent, subsequent export succeeds\n";
}

void test_journal_roundtrip_and_replay() {
    MarketManager source_markets(64);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source(source_markets, source_dispatcher);
    run_scenario(source);

    TEST_ASSERT(journal.records().size() == 9);
    TEST_ASSERT(journal.records()[0].type == JournalRecordType::ORDER_INSERT);
    TEST_ASSERT(journal.records()[1].type == JournalRecordType::COMMAND_RESULT);
    TEST_ASSERT(journal.records()[5].type == JournalRecordType::TRADE);
    TEST_ASSERT(journal.records()[7].type == JournalRecordType::CANCEL);

    const auto path = std::filesystem::temp_directory_path() / "matching_v2.journal.csv";
    journal.save(path.string());
    const std::vector<JournalRecord> loaded = Journal::load(path.string());
    std::filesystem::remove(path);
    TEST_ASSERT(loaded.size() == journal.records().size());

    MarketManager replay_markets(64);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(replay_markets, replay_dispatcher);
    const JournalReplayResult result = JournalReplayEngine::replay(
        loaded, replay_engine, replay_dispatcher, &source_markets);
    TEST_ASSERT(result.success());
    TEST_ASSERT(result.replayed_commands == 4);
    TEST_ASSERT(result.expected_outcomes == 4);
    TEST_ASSERT(result.actual_outcomes == 4);
    TEST_ASSERT(result.symbols.size() == 3);
    TEST_ASSERT(source_markets.find_book(Symbol("AAPL"))->capture_state() ==
                replay_markets.find_book(Symbol("AAPL"))->capture_state());
    TEST_ASSERT(source_markets.find_book(Symbol("TSLA"))->capture_state() ==
                replay_markets.find_book(Symbol("TSLA"))->capture_state());
    TEST_ASSERT(source_markets.find_book(Symbol("NVDA"))->capture_state() ==
                replay_markets.find_book(Symbol("NVDA"))->capture_state());
}

void test_replay_reports_first_outcome_mismatch() {
    MarketManager source_markets(32);
    EventDispatcher source_dispatcher;
    Journal journal;
    journal.attach(source_dispatcher);
    TradingEngine source(source_markets, source_dispatcher);
    source.submit(Timestamp(1), Symbol("MSFT"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                        from_double(100), Quantity(1)));

    std::vector<JournalRecord> corrupt = journal.records();
    TEST_ASSERT(corrupt.size() == 2);
    TEST_ASSERT(corrupt[1].type == JournalRecordType::COMMAND_RESULT);
    const uint64_t expected_mismatch = corrupt[1].sequence;
    corrupt[1].outcome = CommandOutcome::APPLIED;

    MarketManager replay_markets(32);
    EventDispatcher replay_dispatcher;
    TradingEngine replay_engine(replay_markets, replay_dispatcher);
    const JournalReplayResult result = JournalReplayEngine::replay(
        corrupt, replay_engine, replay_dispatcher, &source_markets);
    TEST_ASSERT(!result.success());
    TEST_ASSERT(!result.outcomes_equal);
    TEST_ASSERT(result.unknown_symbols == 1);
    TEST_ASSERT(result.rejected_commands == 1);
    TEST_ASSERT(result.first_mismatched_journal_sequence == expected_mismatch);
}

void test_audit_lifecycle() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AuditLog audit;
    audit.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);
    run_scenario(engine);

    const auto& records = audit.records();
    TEST_ASSERT(records.size() == 9);
    TEST_ASSERT(records[0].action == AuditAction::CREATE);
    TEST_ASSERT(records[1].action == AuditAction::SUBMIT);

    bool passive_partial = false;
    bool aggressive_filled = false;
    bool cancelled = false;
    for (const AuditRecord& record : records) {
        if (record.action == AuditAction::TRADE && record.order_id == OrderId(1) &&
            record.symbol == Symbol("AAPL")) {
            passive_partial = record.status == AuditStatus::PARTIALLY_FILLED &&
                              record.remaining_quantity == Quantity(6);
        }
        if (record.action == AuditAction::TRADE && record.order_id == OrderId(2)) {
            aggressive_filled = record.status == AuditStatus::FILLED;
        }
        if (record.action == AuditAction::CANCEL) {
            cancelled = record.order_id == OrderId(1) &&
                        record.quantity == Quantity(6);
        }
    }
    TEST_ASSERT(passive_partial);
    TEST_ASSERT(aggressive_filled);
    TEST_ASSERT(cancelled);
}

void test_audit_rejection_lifecycle() {
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AuditLog audit;
    audit.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);

    engine.submit(Timestamp(1), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(50), Side::BUY,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(2), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(50), Side::BUY,
                        from_double(100), Quantity(5)));
    engine.submit(Timestamp(3), Symbol("MSFT"),
        NewOrderCommand(CommandSequence(1), OrderId(60), Side::BUY,
                        from_double(100), Quantity(5)));
    engine.submit(Timestamp(4), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(70), Side::BUY,
                        from_double(150), Quantity(5)));
    engine.submit(Timestamp(5), Symbol("NVDA"),
        NewOrderCommand(CommandSequence(1), OrderId(71), Side::BUY,
                        from_double(150), Quantity(5)));
    engine.submit(Timestamp(6), Symbol("AAPL"),
        CancelOrderCommand(CommandSequence(3), OrderId(50)));

    size_t rejected = 0;
    bool original_cancelled = false;
    for (const AuditRecord& record : audit.records()) {
        if (record.action == AuditAction::REJECT &&
            record.status == AuditStatus::REJECTED) {
            ++rejected;
        }
        if (record.action == AuditAction::CANCEL &&
            record.order_id == OrderId(50) && record.quantity == Quantity(10)) {
            original_cancelled = true;
        }
    }
    TEST_ASSERT(rejected == 3);
    TEST_ASSERT(original_cancelled);
}

void test_corrupt_journal_rejected() {
    const auto path = std::filesystem::temp_directory_path() / "bad_v2.journal.csv";
    {
        std::ofstream file(path);
        file << "sequence,timestamp,type,symbol,command_sequence,order_id,side,price,quantity,passive_id,aggressive_id\n";
        file << "2,100,ORDER_INSERT,AAPL,1,1,BUY,1000000,10,0,0\n";
        file << "1,101,CANCEL,AAPL,2,1,BUY,0,0,0,0\n";
    }
    bool rejected = false;
    try {
        (void)Journal::load(path.string());
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    std::filesystem::remove(path);
    TEST_ASSERT(rejected);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--audit-export-failure-only") {
            test_audit_export_reports_failed_write();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown journal/audit test option");
        test_journal_roundtrip_and_replay();
        test_replay_reports_first_outcome_mismatch();
        test_audit_lifecycle();
        test_audit_rejection_lifecycle();
        test_corrupt_journal_rejected();
        test_audit_export_reports_failed_write();
        test_audit_csv_golden_owned_and_borrowed_streams();
        test_audit_short_and_throwing_writes_preserve_memory_state();
        test_audit_flush_failure_and_exception_are_not_silent();
        test_audit_filename_open_failure_is_visible_and_nonmutating();
        std::cout << "Journal, audit and replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Journal, audit and replay tests failed: " << error.what() << '\n';
        return 1;
    }
}
