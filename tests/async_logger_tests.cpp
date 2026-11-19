#include "async_logger.hpp"
#include "trading_engine.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <sstream>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <limits>
#include <algorithm>
#include "audit_log.hpp"
#include "command_journal.hpp"

#define TEST_ASSERT(condition)                                                \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error("Assertion failed: " #condition);       \
        }                                                                     \
    } while (false)

class FailingBuffer : public std::streambuf {
    std::string bytes_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false, released_ = false;

    std::streamsize write(const char* data, std::streamsize size) {
        if (gate.exchange(false)) {
            std::unique_lock<std::mutex> lock(mutex_);
            entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return released_; });
        }
        const auto requested = static_cast<size_t>(size);
        const size_t available = fail_writes.load() ? 0 : budget.load();
        const size_t take = std::min(requested, available);
        bytes_.append(data, take);
        budget.fetch_sub(take);
        if (take != requested && throw_writes.load()) throw std::runtime_error("Injected write failure");
        return static_cast<std::streamsize>(take);
    }
public:
    std::atomic<bool> fail_writes{false};
    std::atomic<bool> throw_writes{false}, fail_flush{false}, throw_flush{false}, gate{false};
    std::atomic<size_t> budget{std::numeric_limits<size_t>::max()};
    std::atomic<unsigned> flush_calls{0};
    const std::string& str() const noexcept { return bytes_; } // Only inspect after stop or while gated.
    bool wait_for_gate() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(3), [this] { return entered_; });
    }
    void release() {
        { std::lock_guard<std::mutex> lock(mutex_); released_ = true; }
        changed_.notify_all();
    }
protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        return write(data, size);
    }
    int_type overflow(int_type value = traits_type::eof()) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return write(&byte, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override {
        flush_calls.fetch_add(1);
        if (throw_flush.load()) throw std::runtime_error("Injected flush exception");
        return fail_flush.load() ? -1 : 0;
    }
};

struct ReleaseGate {
    FailingBuffer& buffer;
    ~ReleaseGate() { buffer.release(); } // Unblock before logger destruction if an assertion fails.
};

class TemporaryLogDirectory {
    std::filesystem::path base_, path_;
public:
    TemporaryLogDirectory() : base_(std::filesystem::weakly_canonical(std::filesystem::temp_directory_path())) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto candidate = base_ / ("dme_async_logger_" + std::to_string(stamp) + "_" + std::to_string(attempt));
            if (candidate.parent_path() != base_) throw std::runtime_error("Unsafe logger test directory");
            if (std::filesystem::create_directory(candidate)) { path_ = candidate; return; }
        }
        throw std::runtime_error("Cannot reserve logger test directory");
    }
    ~TemporaryLogDirectory() {
        try {
            if (!path_.empty() && path_.parent_path() == base_ &&
                path_.filename().string().rfind("dme_async_logger_", 0) == 0) {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } catch (...) {}
    }
    TemporaryLogDirectory(const TemporaryLogDirectory&) = delete;
    TemporaryLogDirectory& operator=(const TemporaryLogDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }
};

SystemEvent market_event(uint64_t timestamp) {
    return SystemEvent::market_data(Timestamp(timestamp), Symbol("AAPL"),
        MarketDataEvent{Price(100), Quantity(1), Price(99), Price(101)});
}

const std::string log_header = "timestamp,event_type,symbol,payload\n";
std::string market_row(uint64_t timestamp) {
    return std::to_string(timestamp) + ",MARKET_DATA,AAPL,price=100;volume=1;bid=99;ask=101\n";
}

void test_failed_stream_is_not_counted_as_written() {
    FailingBuffer buffer;
    std::ostream output(&buffer);
    AsyncLogger logger(output, 4);
    buffer.fail_writes.store(true);
    TEST_ASSERT(logger.enqueue(SystemEvent::market_data(Timestamp(1), Symbol("AAPL"),
        MarketDataEvent{Price(100), Quantity(1), Price(99), Price(101)})));
    logger.stop();
    std::cout << "failed stream: accepted=" << logger.accepted_count()
              << " written=" << logger.written_count() << '\n';
    TEST_ASSERT(logger.accepted_count() == 1 && logger.written_count() == 0);
    TEST_ASSERT(logger.failed_count() == 1 && !logger.healthy() &&
                logger.health() == LoggerHealth::WRITE_FAILURE);
}

void test_short_writes_and_thrown_writes_keep_only_successful_prefix() {
    for (bool throwing : {false, true}) {
        FailingBuffer buffer;
        std::ostream output(&buffer);
        if (throwing) output.exceptions(std::ios::badbit | std::ios::failbit);
        AsyncLogger logger(output, 4);
        ReleaseGate release{buffer};
        buffer.budget.store(market_row(1).size() + 4); // One full row, then an incomplete row.
        buffer.throw_writes.store(throwing);
        buffer.gate.store(true);
        TEST_ASSERT(logger.enqueue(market_event(1)));
        TEST_ASSERT(buffer.wait_for_gate());
        TEST_ASSERT(logger.enqueue(market_event(2)) && logger.enqueue(market_event(3)));
        buffer.release();
        logger.stop();
        TEST_ASSERT(logger.accepted_count() == 3 && logger.written_count() == 1 && logger.failed_count() == 2);
        TEST_ASSERT(logger.dropped_count() == 0 && !logger.healthy() && !logger.available());
        TEST_ASSERT(logger.health() == LoggerHealth::WRITE_FAILURE);
        TEST_ASSERT(buffer.str().rfind(log_header + market_row(1), 0) == 0);
        TEST_ASSERT(buffer.str().size() == log_header.size() + market_row(1).size() + 4);
        TEST_ASSERT(buffer.str().find("3,MARKET_DATA") == std::string::npos);
        TEST_ASSERT(!logger.enqueue(market_event(4)) && logger.dropped_count() == 1);
        logger.stop();
        TEST_ASSERT(logger.health() == LoggerHealth::WRITE_FAILURE && logger.failed_count() == 2);
    }
    std::cout << "Short/thrown write failures: successful prefix=1, failed tail=2\n";
}

void test_queue_full_is_visible_but_does_not_poison_stream_health() {
    FailingBuffer buffer;
    std::ostream output(&buffer);
    AsyncLogger logger(output, 1);
    ReleaseGate release{buffer};
    buffer.gate.store(true);
    TEST_ASSERT(logger.enqueue(market_event(1)) && buffer.wait_for_gate());
    TEST_ASSERT(logger.enqueue(market_event(2)));
    TEST_ASSERT(!logger.enqueue(market_event(3)));
    TEST_ASSERT(buffer.str() == log_header); // Worker is gated; enqueue did not touch the sink.
    TEST_ASSERT(logger.healthy() && logger.available() && logger.dropped_count() == 1);
    buffer.release();
    logger.stop();
    TEST_ASSERT(logger.accepted_count() == 2 && logger.written_count() == 2 && logger.failed_count() == 0);
    TEST_ASSERT(logger.healthy() && logger.health() == LoggerHealth::STOPPED);
    TEST_ASSERT(buffer.str() == log_header + market_row(1) + market_row(2));
    std::cout << "Bounded queue saturation: dropped=1, admitted FIFO retained, producer no sink I/O\n";
}

void test_flush_failures_are_sticky_even_when_rows_were_written() {
    for (bool throwing : {false, true}) {
        FailingBuffer buffer;
        std::ostream output(&buffer);
        if (throwing) output.exceptions(std::ios::badbit | std::ios::failbit);
        AsyncLogger logger(output, 4);
        buffer.fail_flush.store(!throwing);
        buffer.throw_flush.store(throwing);
        TEST_ASSERT(logger.enqueue(market_event(1)));
        logger.stop();
        TEST_ASSERT(logger.accepted_count() == 1 && logger.written_count() == 1 && logger.failed_count() == 0);
        TEST_ASSERT(!logger.healthy() && logger.health() == LoggerHealth::WRITE_FAILURE);
        TEST_ASSERT(buffer.flush_calls.load() == 2 && buffer.str() == log_header + market_row(1));
        logger.stop();
        TEST_ASSERT(logger.health() == LoggerHealth::WRITE_FAILURE);
    }
    std::cout << "Flush return/exception failures: written=accepted=1 but health failed\n";
}

void test_header_failures_prevent_worker_start() {
    for (bool throwing : {false, true}) {
        for (bool flush_failure : {false, true}) {
            FailingBuffer buffer;
            std::ostream output(&buffer);
            if (throwing) output.exceptions(std::ios::badbit | std::ios::failbit);
            buffer.fail_writes.store(!flush_failure);
            buffer.throw_writes.store(throwing && !flush_failure);
            buffer.fail_flush.store(flush_failure && !throwing);
            buffer.throw_flush.store(flush_failure && throwing);
            bool rejected = false;
            try { AsyncLogger logger(output, 4); } catch (const std::exception&) { rejected = true; }
            TEST_ASSERT(rejected);
        }
    }
    std::ostringstream output;
    bool rejected = false;
    try { AsyncLogger invalid(output, 0); } catch (const std::invalid_argument&) { rejected = true; }
    TEST_ASSERT(rejected && output.str().empty());
    std::cout << "Header write/flush failures and zero queue rejected before worker start\n";
}

void test_borrowed_stream_drain_stop_and_lifetime() {
    std::ostringstream output;
    {
        AsyncLogger logger(output, 4);
        TEST_ASSERT(logger.enqueue(market_event(1)) && logger.enqueue(market_event(2)));
        logger.stop();
        logger.stop();
        TEST_ASSERT(logger.healthy() && logger.health() == LoggerHealth::STOPPED);
        TEST_ASSERT(logger.accepted_count() == 2 && logger.written_count() == 2 && logger.failed_count() == 0);
        TEST_ASSERT(!logger.enqueue(market_event(3)) && logger.dropped_count() == 1);
    }
    TEST_ASSERT(output.str() == log_header + market_row(1) + market_row(2));
    output << "caller still owns stream\n";
    TEST_ASSERT(output.good());
    {
        AsyncLogger logger(output, 4);
        TEST_ASSERT(logger.enqueue(market_event(4))); // Destructor alone drains too.
    }
    TEST_ASSERT(output.str().find(market_row(4)) != std::string::npos);
}

class RecordingJournal final : public CommandJournal {
public:
    size_t begins = 0, commits = 0, trades = 0, aborts = 0;
    bool available() const noexcept override { return true; }
    bool begin_command(Timestamp, const Symbol&, const Command&, size_t) override { ++begins; return true; }
    bool append_trade(const TradeEvent&) override { ++trades; return true; }
    bool commit_command(CommandOutcome) override { ++commits; return true; }
    void abort_command() noexcept override { ++aborts; }
};

void test_optional_logger_failure_does_not_replace_journal_or_audit_policy() {
    FailingBuffer buffer;
    std::ostream output(&buffer);
    AsyncLogger logger(output, 32);
    MarketManager markets(8);
    EventDispatcher dispatcher;
    AuditLog audit;
    RecordingJournal journal;
    logger.attach(dispatcher);
    audit.attach(dispatcher);
    TradingEngine engine(markets, dispatcher, journal);
    buffer.fail_writes.store(true);
    engine.submit(Timestamp(1), Symbol("AAPL"), NewOrderCommand(CommandSequence(1), OrderId(1),
        Side::SELL, Price(1'000'000), Quantity(2)));
    engine.submit(Timestamp(2), Symbol("AAPL"), NewOrderCommand(CommandSequence(2), OrderId(2),
        Side::BUY, Price(1'000'000), Quantity(1)));
    engine.submit(Timestamp(3), Symbol("AAPL"), CancelOrderCommand(CommandSequence(3), OrderId(1)));
    logger.stop();
    TEST_ASSERT(!logger.healthy() && logger.failed_count() == logger.accepted_count());
    TEST_ASSERT(!engine.failed() && markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    TEST_ASSERT(journal.begins == 3 && journal.commits == 3 && journal.trades == 1 && journal.aborts == 0);
    TEST_ASSERT(audit.records().size() == 7);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->check_invariants());
    std::cout << "Diagnostic logger failure isolated from canonical Journal and Audit lifecycle\n";
}

void test_bounded_queue_contract() {
    BoundedQueue<int> queue(2);
    TEST_ASSERT(queue.try_push(1));
    TEST_ASSERT(queue.try_push(2));
    TEST_ASSERT(!queue.try_push(3));
    int value = 0;
    TEST_ASSERT(queue.wait_pop(value) && value == 1);
    TEST_ASSERT(queue.wait_pop(value) && value == 2);
    queue.close();
    TEST_ASSERT(!queue.wait_pop(value));
    TEST_ASSERT(!queue.try_push(4));
}

void test_async_event_logging_and_drain() {
    TemporaryLogDirectory directory;
    const auto path = directory.path() / "events.log";
    MarketManager markets(64);
    EventDispatcher dispatcher;
    AsyncLogger logger(path.string(), 64);
    logger.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);

    engine.submit(Timestamp(100), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                        from_double(100), Quantity(10)));
    engine.submit(Timestamp(101), Symbol("AAPL"),
        NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                        from_double(100), Quantity(10)));
    engine.publish_market_data(Timestamp(102), Symbol("NVDA"),
        MarketDataEvent{from_double(150), Quantity(500),
                        from_double(149.99), from_double(150.01)});
    logger.stop();

    TEST_ASSERT(logger.accepted_count() == 7);
    TEST_ASSERT(logger.written_count() == logger.accepted_count());
    TEST_ASSERT(logger.dropped_count() == 0);
    TEST_ASSERT(logger.healthy() && logger.health() == LoggerHealth::STOPPED && logger.failed_count() == 0);

    std::ifstream file(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) lines.push_back(line);
    file.close();
    std::filesystem::remove(path);
    TEST_ASSERT(lines.size() == 8);
    TEST_ASSERT(lines[1].find("100,ORDER,AAPL,NEW_ORDER") == 0);
    TEST_ASSERT(lines[2].find("100,ORDER_RESTED,AAPL,") == 0);
    TEST_ASSERT(lines[3].find("100,PROCESSING_LATENCY,AAPL,") == 0);
    TEST_ASSERT(lines[5].find("101,TRADE,AAPL,TRADE") == 0);
    TEST_ASSERT(lines[7].find("102,MARKET_DATA,NVDA,") == 0);
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--failed-stream-only") {
            test_failed_stream_is_not_counted_as_written();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown async logger test option");
        test_bounded_queue_contract();
        test_async_event_logging_and_drain();
        test_failed_stream_is_not_counted_as_written();
        test_short_writes_and_thrown_writes_keep_only_successful_prefix();
        test_queue_full_is_visible_but_does_not_poison_stream_health();
        test_flush_failures_are_sticky_even_when_rows_were_written();
        test_header_failures_prevent_worker_start();
        test_borrowed_stream_drain_stop_and_lifetime();
        test_optional_logger_failure_does_not_replace_journal_or_audit_policy();
        std::cout << "Async logger tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Async logger tests failed: " << error.what() << '\n';
        return 1;
    }
}
