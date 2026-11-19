#include "async_journal.hpp"
#include "public_feed_publisher.hpp"
#include "trading_engine.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while (false)

namespace {
Command order(uint64_t sequence, uint64_t id, Side side = Side::BUY,
              uint64_t quantity = 1, int64_t price = 1'000'000,
              TimeInForce tif = TimeInForce::GTC) {
    return NewOrderCommand(CommandSequence(sequence), OrderId(id), side,
                           Price(price), Quantity(quantity), tif);
}
Command cancel(uint64_t sequence, uint64_t id) {
    return CancelOrderCommand(CommandSequence(sequence), OrderId(id));
}
class MemoryJournal final : public CommandJournal {
public:
    size_t commits = 0;
    bool active = false;
    bool available() const noexcept override { return true; }
    bool begin_command(Timestamp, const Symbol&, const Command&, size_t) override {
        TEST_ASSERT(!active); active = true; return true;
    }
    bool append_trade(const TradeEvent&) override { TEST_ASSERT(active); return true; }
    bool commit_command(CommandOutcome) override {
        TEST_ASSERT(active); active = false; ++commits; return true;
    }
    void abort_command() noexcept override { active = false; }
};
class RecordingSink final : public FeedByteSink {
    GatewayFeedSink adapter_;
public:
    std::vector<FeedMessage> messages;
    std::vector<size_t> batches;
    size_t invalidations = 0;
    explicit RecordingSink(MarketDataGateway& gateway) : adapter_(gateway) {}
    bool consume(const uint8_t* bytes, size_t size) override {
        if (!adapter_.consume(bytes, size)) return false;
        FeedDecoder decoder;
        decoder.consume(bytes, size, [this](const FeedMessage& message) { messages.push_back(message); });
        decoder.finish();
        batches.push_back(size);
        return true;
    }
    void invalidate() noexcept override { ++invalidations; adapter_.invalidate(); }
};
struct Loop {
    MarketManager markets{16};
    EventDispatcher dispatcher;
    MemoryJournal journal;
    MarketDataGateway gateway{3};
    RecordingSink sink{gateway};
    PublicFeedPublisher publisher;
    TradingEngine engine;
    explicit Loop(size_t budget = 4096)
        : publisher(markets, sink, budget), engine(markets, dispatcher, journal, publisher) {}
    MarketProcessResult submit(const Command& command, uint32_t instrument = 0) {
        return engine.submit(Timestamp(journal.commits + 1), InstrumentId(instrument), command);
    }
};
void assert_all_stale(const MarketDataGateway& gateway) {
    for (size_t i = 0; i < gateway.instrument_count(); ++i)
        TEST_ASSERT(gateway.state(InstrumentId(static_cast<uint32_t>(i))) == FeedState::STALE);
}
void compare_depth(const MarketManager& source, const MarketDataGateway& gateway) {
    for (size_t i = 0; i < source.symbol_count(); ++i) {
        const InstrumentId id(static_cast<uint32_t>(i));
        const auto state = source.find_book(id)->capture_state();
        const MarketDataBook* book = gateway.book(id);
        TEST_ASSERT(book && book->order_count() == state.active_order_count);
        for (const auto& level : state.bid_levels)
            TEST_ASSERT(book->bid_quantity(level.price) == level.total_volume.get());
        for (const auto& level : state.ask_levels)
            TEST_ASSERT(book->ask_quantity(level.price) == level.total_volume.get());
        // Compare every visible order without confusing private IDs/public
        // references or asserting a FIFO representation in MarketDataBook.
        using VisibleOrder = std::tuple<Side, int64_t, uint64_t>;
        std::vector<VisibleOrder> expected, actual;
        for (const auto* levels : {&state.bid_levels, &state.ask_levels})
            for (const auto& level : *levels)
                for (const auto& order : level.orders)
                    expected.emplace_back(order.side, order.price.get(), order.remaining_quantity.get());
        for (const auto& order : book->capture_state().orders)
            actual.emplace_back(order.side, order.price.get(), order.remaining.get());
        std::sort(expected.begin(), expected.end());
        std::sort(actual.begin(), actual.end());
        TEST_ASSERT(expected == actual);
        TEST_ASSERT(gateway.state(id) == FeedState::HEALTHY);
    }
}
struct TemporaryJournal {
    std::filesystem::path root;
    std::filesystem::path directory;
    std::filesystem::path path;
    bool owns_directory = false;
    TemporaryJournal() : root(std::filesystem::canonical(std::filesystem::temp_directory_path())) {
        std::random_device random;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (size_t attempt = 0; attempt < 128; ++attempt) {
            auto candidate = root / ("matching-engine-public-feed-" + std::to_string(stamp) +
                "-" + std::to_string(random()) + "-" + std::to_string(attempt));
            auto candidate_file = candidate / "journal.csv";
            std::error_code error;
            // Atomic exclusive creation: never reuse or truncate a preexisting
            // file/directory, including parallel Release/ASan test sessions.
            if (std::filesystem::create_directory(candidate, error)) {
                directory = std::move(candidate);
                path = std::move(candidate_file);
                owns_directory = true;
                return;
            }
            if (error && error != std::errc::file_exists)
                throw std::filesystem::filesystem_error("create private journal test directory", candidate, error);
        }
        throw std::runtime_error("Cannot exclusively create private journal test directory");
    }
    TemporaryJournal(const TemporaryJournal&) = delete;
    TemporaryJournal& operator=(const TemporaryJournal&) = delete;
    ~TemporaryJournal() {
        std::error_code error;
        if (!owns_directory || !directory.is_absolute() || directory.parent_path() != root ||
            path.parent_path() != directory || directory.filename().string().find("matching-engine-public-feed-") != 0)
            return;
        const auto status = std::filesystem::symlink_status(directory, error);
        if (error || !std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) return;
        const auto resolved = std::filesystem::canonical(directory, error);
        if (error || resolved != directory || resolved.parent_path() != root) return;
        // Only the exact absolute child exclusively created above is removed.
        std::filesystem::remove_all(directory, error);
    }
};

void test_real_engine_journal_and_multi_instrument_projection() {
    TemporaryJournal file;
    MarketManager markets(16);
    EventDispatcher dispatcher;
    AsyncJournalWriter journal(file.path.string(), 512);
    MarketDataGateway gateway(3);
    RecordingSink sink(gateway);
    PublicFeedPublisher publisher(markets, sink, 4096);
    TradingEngine engine(markets, dispatcher, journal, publisher);
    uint64_t timestamp = 0;
    auto submit = [&](uint32_t instrument, const Command& command, ProcessStatus expected = ProcessStatus::APPLIED) {
        const auto result = engine.submit(Timestamp(++timestamp), InstrumentId(instrument), command);
        TEST_ASSERT(result.routed() && result.process_result.status == expected &&
                    result.process_result.event_output_complete);
    };
    submit(0, order(10, 900, Side::SELL, 10));
    submit(1, order(1, 900, Side::BUY, 8, 900'000));
    TEST_ASSERT(publisher.pending_batches() == 2 && publisher.pending_bytes() == 98);
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 0);
    TEST_ASSERT(publisher.drain());
    TEST_ASSERT(gateway.book(InstrumentId(0))->ask_quantity(Price(1'000'000)) == 10);
    TEST_ASSERT(gateway.book(InstrumentId(1))->bid_quantity(Price(900'000)) == 8);
    submit(0, order(11, 800, Side::BUY, 4)); // Partial passive fill.
    TEST_ASSERT(publisher.pending_bytes() == 56 && publisher.drain());
    TEST_ASSERT(gateway.book(InstrumentId(0))->ask_quantity(Price(1'000'000)) == 6);
    submit(0, order(12, 700, Side::BUY, 8)); // Full passive fill + aggressor rest.
    TEST_ASSERT(publisher.pending_batches() == 1 && publisher.pending_bytes() == 105);
    TEST_ASSERT(publisher.drain());
    TEST_ASSERT(gateway.book(InstrumentId(0))->bid_quantity(Price(1'000'000)) == 2);
    compare_depth(markets, gateway);
    submit(0, order(13, 600, Side::BUY, 5, 1'000'000, TimeInForce::IOC));
    submit(0, order(14, 500, Side::SELL, 1, 1'000'000, TimeInForce::POST_ONLY), ProcessStatus::REJECTED);
    submit(0, order(15, 0), ProcessStatus::REJECTED);
    submit(0, order(14, 800), ProcessStatus::SEQUENCE_REJECTED);
    TEST_ASSERT(publisher.pending_bytes() == 0 && publisher.pending_batches() == 0);
    submit(0, cancel(16, 700));
    submit(0, order(17, 900, Side::SELL, 3, 1'010'000)); // Reused PRIVATE ID, fresh public ref.
    submit(0, order(18, 400, Side::BUY, 5, 1'020'000, TimeInForce::IOC));
    submit(0, order(19, 300, Side::BUY, 2, 990'000, TimeInForce::POST_ONLY));
    TEST_ASSERT(publisher.drain());
    compare_depth(markets, gateway);
    const auto visible = gateway.book(InstrumentId(0))->capture_state();
    TEST_ASSERT(visible.orders.size() == 1 && visible.orders[0].order_id == 4 &&
                visible.orders[0].order_id != 300);
    submit(0, cancel(20, 300));
    submit(0, cancel(21, 12345), ProcessStatus::REJECTED);
    submit(1, cancel(2, 900));
    TEST_ASSERT(publisher.drain() && publisher.pending_bytes() == 0);
    compare_depth(markets, gateway);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 9 &&
                gateway.last_sequence(InstrumentId(1)) == 2 &&
                gateway.last_sequence(InstrumentId(2)) == 0);
    TEST_ASSERT(sink.messages.size() == 11 && sink.invalidations == 0);
    const auto& add0 = std::get<FeedAddOrder>(sink.messages[0].payload);
    const auto& add1 = std::get<FeedAddOrder>(sink.messages[1].payload);
    TEST_ASSERT(add0.order_id == 1 && add1.order_id == 1); // Per-instrument namespace.
    TEST_ASSERT(std::get<FeedTrade>(sink.messages[2].payload).passive_order_id == 1);
    TEST_ASSERT(std::get<FeedTrade>(sink.messages[3].payload).passive_order_id == 1);
    TEST_ASSERT(std::get<FeedAddOrder>(sink.messages[4].payload).order_id == 2);
    TEST_ASSERT(std::get<FeedCancelOrder>(sink.messages[5].payload).order_id == 2);
    TEST_ASSERT(std::get<FeedAddOrder>(sink.messages[6].payload).order_id == 3);
    TEST_ASSERT(std::get<FeedTrade>(sink.messages[7].payload).passive_order_id == 3);
    uint64_t expected_sequences[3] = {0, 0, 0};
    for (const auto& message : sink.messages) {
        TEST_ASSERT(message.header.sequence == ++expected_sequences[message.header.instrument_id.get()]);
        if (const auto* trade = std::get_if<FeedTrade>(&message.payload))
            TEST_ASSERT(trade->trade_id == message.header.sequence);
        TEST_ASSERT(message.header.exchange_timestamp.get() != 0);
    }
    TEST_ASSERT(sink.batches[3] == 105); // Whole command, two frames, one consume.
    journal.stop();
    TEST_ASSERT(journal.healthy() && journal.accepted_count() == journal.written_count());
    TEST_ASSERT(!Journal::load(file.path.string()).empty());
}

void test_exact_budget_and_whole_command_overflow() {
    {
        Loop loop(98);
        TEST_ASSERT(loop.submit(order(1, 999)).process_result.applied());
        TEST_ASSERT(loop.submit(order(2, 888)).process_result.applied());
        TEST_ASSERT(loop.publisher.pending_bytes() == 98 && loop.publisher.pending_batches() == 2);
        TEST_ASSERT(loop.publisher.drain() && loop.publisher.pending_bytes() == 0);
        TEST_ASSERT(loop.submit(cancel(3, 999)).process_result.applied());
        TEST_ASSERT(loop.publisher.pending_bytes() == 32 && loop.publisher.drain());
    }
    {
        Loop loop(97);
        TEST_ASSERT(loop.submit(order(1, 999)).process_result.applied());
        const auto result = loop.submit(order(2, 888));
        TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
        TEST_ASSERT(loop.journal.commits == 2); // Matching/journal acceptance cannot be undone.
        TEST_ASSERT(loop.markets.find_book(InstrumentId(0))->active_order_count() == 2);
        TEST_ASSERT(!loop.publisher.available() && !loop.publisher.drain());
        TEST_ASSERT(loop.publisher.pending_bytes() == 0 && loop.publisher.pending_batches() == 0);
        TEST_ASSERT(loop.sink.messages.empty() && loop.sink.invalidations == 1);
        assert_all_stale(loop.gateway);
        const auto commits = loop.journal.commits;
        TEST_ASSERT(loop.submit(order(3, 777)).routing_status == RoutingStatus::SYSTEM_UNAVAILABLE);
        TEST_ASSERT(loop.journal.commits == commits);
    }
    {
        Loop loop(104);
        TEST_ASSERT(loop.submit(order(1, 999, Side::SELL, 3)).process_result.applied());
        TEST_ASSERT(loop.publisher.drain());
        const auto result = loop.submit(order(2, 888, Side::BUY, 5));
        TEST_ASSERT(result.process_result.applied() && !result.process_result.event_output_complete);
        TEST_ASSERT(loop.publisher.pending_bytes() == 0 && loop.publisher.pending_batches() == 0);
        TEST_ASSERT(loop.sink.messages.size() == 1); // Neither Trade nor Add of failed 105-byte batch escaped.
        assert_all_stale(loop.gateway);
    }
    {
        Loop loop(105);
        TEST_ASSERT(loop.submit(order(1, 999, Side::SELL, 3)).process_result.applied());
        TEST_ASSERT(loop.publisher.drain());
        TEST_ASSERT(loop.submit(order(2, 888, Side::BUY, 5)).process_result.applied());
        TEST_ASSERT(loop.publisher.pending_bytes() == 105 && loop.publisher.pending_batches() == 1);
        TEST_ASSERT(loop.publisher.drain() && loop.sink.batches.back() == 105);
    }
}

FeedMessage feed_add(uint64_t sequence, uint64_t reference = 1, uint32_t instrument = 0) {
    return {{FeedMessageType::ADD_ORDER, 25, InstrumentId(instrument), sequence, Timestamp(1)},
            FeedAddOrder{reference, Price(1'000'000), Quantity(1), Side::BUY}};
}
void test_adapter_first_sequence_gaps_and_malformed_batches_are_terminal() {
    for (int fault = 0; fault < 5; ++fault) {
        MarketDataGateway gateway(2);
        GatewayFeedSink adapter(gateway);
        auto frame = FeedEncoder::encode(feed_add(fault == 0 ? 2 : 1));
        if (fault == 1) frame.bytes[1] = 99; // Unknown type.
        if (fault == 2) frame.bytes[3] = 24; // Invalid body length.
        if (fault == 3) frame.bytes[48] = 2; // Invalid side.
        if (fault == 4) --frame.length; // Incomplete whole batch.
        TEST_ASSERT(!adapter.consume(frame.data(), frame.size()));
        TEST_ASSERT(!adapter.available() && adapter.pending_bytes() == 0);
        assert_all_stale(gateway);
        const auto good = FeedEncoder::encode(feed_add(1));
        TEST_ASSERT(!adapter.consume(good.data(), good.size()));
    }
    {
        MarketDataGateway gateway(2);
        GatewayFeedSink adapter(gateway);
        const auto first = FeedEncoder::encode(feed_add(1));
        const auto gap = FeedEncoder::encode(feed_add(3, 2));
        std::vector<uint8_t> bytes(first.data(), first.data() + first.size());
        bytes.insert(bytes.end(), gap.data(), gap.data() + gap.size());
        TEST_ASSERT(!adapter.consume(bytes.data(), bytes.size()));
        TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 1); // Prefix may have applied.
        TEST_ASSERT(!adapter.available() && adapter.pending_bytes() == 0);
        assert_all_stale(gateway);
    }
    for (int fault = 0; fault < 3; ++fault) {
        MarketDataGateway gateway(2);
        GatewayFeedSink adapter(gateway);
        const auto first = FeedEncoder::encode(feed_add(1));
        TEST_ASSERT(adapter.consume(first.data(), first.size()));
        FeedMessage bad = fault == 0 ? feed_add(1, 2) :
            fault == 1 ? feed_add(2, 1) : feed_add(2, 2, 3);
        const auto frame = FeedEncoder::encode(bad);
        TEST_ASSERT(!adapter.consume(frame.data(), frame.size())); // Duplicate, book reject, invalid instrument.
        assert_all_stale(gateway);
    }
}

class FaultSink final : public FeedByteSink {
public:
    std::function<bool(const uint8_t*, size_t)> action;
    size_t calls = 0, invalidations = 0;
    bool consume(const uint8_t* bytes, size_t size) override {
        ++calls;
        return action ? action(bytes, size) : false;
    }
    void invalidate() noexcept override { ++invalidations; }
};
void test_sink_false_exception_inflight_charge_and_no_retry() {
    for (bool throws : {false, true}) {
        MarketManager markets(16);
        EventDispatcher dispatcher;
        MemoryJournal journal;
        FaultSink sink;
        PublicFeedPublisher publisher(markets, sink, 98);
        TradingEngine engine(markets, dispatcher, journal, publisher);
        TEST_ASSERT(engine.submit(Timestamp(1), InstrumentId(0), order(1, 999)).process_result.applied());
        TEST_ASSERT(engine.submit(Timestamp(2), InstrumentId(0), order(2, 888)).process_result.applied());
        sink.action = [&](const uint8_t* bytes, size_t size) -> bool {
            TEST_ASSERT(size == 49 && bytes != nullptr);
            TEST_ASSERT(publisher.pending_bytes() == 98 && publisher.pending_batches() == 2);
            TEST_ASSERT(!publisher.available());
            if (throws) throw std::runtime_error("consumer failure after unknown prefix");
            return false;
        };
        TEST_ASSERT(!publisher.drain());
        TEST_ASSERT(!publisher.drain() && sink.calls == 1 && sink.invalidations == 1);
        TEST_ASSERT(publisher.pending_bytes() == 0 && publisher.pending_batches() == 0);
    }
    for (bool reenter_publish : {false, true}) {
        MarketManager markets(16);
        EventDispatcher dispatcher;
        MemoryJournal journal;
        FaultSink sink;
        PublicFeedPublisher publisher(markets, sink, 49);
        TradingEngine engine(markets, dispatcher, journal, publisher);
        TEST_ASSERT(engine.submit(Timestamp(1), InstrumentId(0), order(1, 999)).process_result.applied());
        sink.action = [&](const uint8_t* bytes, size_t size) {
            const uint8_t first = bytes[0];
            TEST_ASSERT(publisher.pending_bytes() == size && publisher.pending_batches() == 1);
            if (reenter_publish) {
                TEST_ASSERT(!publisher.publish(Timestamp(2), InstrumentId(0), cancel(2, 999),
                    CommandOutcome::APPLIED, {}));
            } else {
                TEST_ASSERT(!publisher.drain());
            }
            TEST_ASSERT(publisher.pending_bytes() == size && publisher.pending_batches() == 1);
            TEST_ASSERT(bytes[0] == first); // In-flight allocation survives invalidation until return.
            return true;
        };
        TEST_ASSERT(!publisher.drain() && sink.calls == 1 && sink.invalidations == 1);
        TEST_ASSERT(publisher.pending_bytes() == 0 && publisher.pending_batches() == 0);
    }
}

void test_session_prerequisites_and_context_fail_closed() {
    for (int fault = 0; fault < 3; ++fault) {
        MarketDataGateway gateway(2);
        if (fault == 0) TEST_ASSERT(gateway.on_message(feed_add(1)));
        if (fault == 1) {
            auto checkpoint = gateway.checkpoint(); checkpoint.sequences[0] = 7;
            gateway.restore_checkpoint(checkpoint);
        }
        if (fault == 2) gateway.invalidate(InstrumentId(1));
        bool rejected = false;
        try { GatewayFeedSink invalid(gateway); }
        catch (const std::exception&) { rejected = true; }
        TEST_ASSERT(rejected); assert_all_stale(gateway);
    }
    for (bool disabled_events : {false, true}) {
        MarketManager markets(EmptyMarketManagerTag{});
        InstrumentRuntimeConfig config; config.order_capacity = 8; config.record_events = !disabled_events;
        markets.register_symbol(Symbol("ONLY"), config);
        if (!disabled_events) markets.process(Symbol("ONLY"), order(1, 999));
        FaultSink sink;
        bool rejected = false;
        try { PublicFeedPublisher invalid(markets, sink, 1024); }
        catch (const std::exception&) { rejected = true; }
        TEST_ASSERT(rejected && sink.invalidations == 1);
    }
    {
        Loop loop;
        TEST_ASSERT(loop.submit(order(1, 999)).process_result.applied());
        // Mismatched event index is terminal; queued older output is discarded.
        const Command command = order(2, 888);
        const auto result = loop.markets.process(Symbol("AAPL"), command);
        TEST_ASSERT(result.process_result.applied());
        std::vector<EngineEvent> invalid{OrderRestedEvent(CommandSequence(2), EventIndex(1),
            OrderId(888), Side::BUY, Price(1'000'000), Quantity(1), Quantity(1), PrioritySequence(2))};
        TEST_ASSERT(!loop.publisher.publish(Timestamp(2), InstrumentId(0), command,
                                           CommandOutcome::APPLIED, invalid));
        TEST_ASSERT(loop.publisher.pending_bytes() == 0 && loop.sink.messages.empty());
        assert_all_stale(loop.gateway);
    }
    {
        // A source may have historical rejected commands while remaining empty;
        // public references/sequences still start freshly at one.
        MarketManager markets(16);
        TEST_ASSERT(markets.process(Symbol("AAPL"), order(100, 0)).process_result.status == ProcessStatus::REJECTED);
        EventDispatcher dispatcher;
        MemoryJournal journal;
        MarketDataGateway gateway(3);
        RecordingSink sink(gateway);
        PublicFeedPublisher publisher(markets, sink, 49);
        TradingEngine engine(markets, dispatcher, journal, publisher);
        TEST_ASSERT(engine.submit(Timestamp(1), InstrumentId(0), order(101, 900)).process_result.applied());
        TEST_ASSERT(publisher.drain() && sink.messages[0].header.sequence == 1);
        TEST_ASSERT(std::get<FeedAddOrder>(sink.messages[0].payload).order_id == 1);
    }
}
} // namespace

int main() {
    try {
        test_real_engine_journal_and_multi_instrument_projection();
        test_exact_budget_and_whole_command_overflow();
        test_adapter_first_sequence_gaps_and_malformed_batches_are_terminal();
        test_sink_false_exception_inflight_charge_and_no_retry();
        test_session_prerequisites_and_context_fail_closed();
        std::cout << "Public feed publisher tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Public feed publisher tests failed: " << error.what() << '\n';
        return 1;
    }
}
