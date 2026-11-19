// Executable-local allocation replacements: never linked into demos/benchmarks.
#include "command_journal.hpp"
#include "async_logger.hpp"
#include "feed_encoder.hpp"
#include "market_data_gateway.hpp"
#include "event_sink.hpp"
#include "price_level_store.hpp"
#include "trading_engine.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <sstream>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_fault {
thread_local size_t remaining = 0;
thread_local bool armed = false;
thread_local bool triggered = false;
thread_local size_t attempts = 0;

void checkpoint() {
    if (!armed) return;
    ++attempts;
    if (remaining == 0) {
        armed = false; // Exception handling/diagnostics must be able to allocate.
        triggered = true;
        throw std::bad_alloc();
    }
    --remaining;
}
void* allocate(size_t size) {
    checkpoint();
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void* allocate_aligned(size_t size, size_t alignment) {
    checkpoint();
    size = size == 0 ? 1 : size;
#ifdef _WIN32
    if (void* result = _aligned_malloc(size, alignment)) return result;
#else
    if (size > std::numeric_limits<size_t>::max() - (alignment - 1)) throw std::bad_alloc();
    const size_t rounded = (size + alignment - 1) / alignment * alignment;
    if (void* result = std::aligned_alloc(alignment, rounded)) return result;
#endif
    throw std::bad_alloc();
}
void free_aligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}
struct Observation { bool threw; bool hit; size_t allocations; };
template<typename Function>
Observation inject(size_t successful_allocations, Function&& function) {
    remaining = successful_allocations;
    triggered = false;
    attempts = 0;
    armed = true;
    bool threw = false;
    try { function(); }
    catch (const std::bad_alloc&) { threw = true; }
    catch (...) { armed = false; throw; }
    armed = false; // Disable before assertions, streams, snapshots, or copies.
    return {threw, triggered, attempts};
}
} // namespace allocation_fault

void* operator new(size_t size) { return allocation_fault::allocate(size); }
void* operator new[](size_t size) { return allocation_fault::allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, size_t) noexcept { std::free(value); }
void operator delete[](void* value, size_t) noexcept { std::free(value); }
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try { return allocation_fault::allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try { return allocation_fault::allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void* operator new(size_t size, std::align_val_t alignment) {
    return allocation_fault::allocate_aligned(size, static_cast<size_t>(alignment));
}
void* operator new[](size_t size, std::align_val_t alignment) {
    return allocation_fault::allocate_aligned(size, static_cast<size_t>(alignment));
}
void operator delete(void* value, std::align_val_t) noexcept { allocation_fault::free_aligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { allocation_fault::free_aligned(value); }
void operator delete(void* value, size_t, std::align_val_t) noexcept { allocation_fault::free_aligned(value); }
void operator delete[](void* value, size_t, std::align_val_t) noexcept { allocation_fault::free_aligned(value); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_fault::allocate_aligned(size, static_cast<size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_fault::allocate_aligned(size, static_cast<size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_fault::free_aligned(value);
}
void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_fault::free_aligned(value);
}

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {
Command order(uint64_t sequence, Side side, int64_t price, uint64_t volume,
              TimeInForce tif = TimeInForce::GTC) {
    return NewOrderCommand(CommandSequence(sequence), OrderId(sequence), side, Price(price), Quantity(volume), tif);
}
void assert_failed_and_blocked(OrderBook& book, uint64_t next_sequence) {
    TEST_ASSERT(book.event_output_state() == EventOutputState::FAILED && !book.ready_for_commands());
    FixedEventBuffer<4> sink;
    const auto before = book.capture_state();
    const Command next = order(next_sequence, Side::BUY, 1'000'000, 1);
    TEST_ASSERT(book.process(next).status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(book.process(next, sink).status == ProcessStatus::OUTPUT_BLOCKED);
    TEST_ASSERT(!book.retry_pending_events(sink) && sink.size() == 0);
    TEST_ASSERT(book.capture_state() == before);
    bool blocked_snapshot = false;
    try { (void)book.create_snapshot(); } catch (const std::logic_error&) { blocked_snapshot = true; }
    TEST_ASSERT(blocked_snapshot);
}

void test_store_returns_acquired_level_when_map_allocation_fails() {
    LimitLevelPool pool(2);
    PriceLevelStore<std::greater<int64_t>> store(pool);
    const auto failed = allocation_fault::inject(0, [&] { store.get_or_create(1'000'000); });
    TEST_ASSERT(failed.threw && failed.hit && failed.allocations == 1);
    TEST_ASSERT(store.empty() && pool.available() == 2 && !store.find(1'000'000));
    auto first = store.get_or_create(1'000'000).first;
    const auto existing = allocation_fault::inject(0, [&] { (void)store.get_or_create(1'000'000); });
    TEST_ASSERT(!existing.threw && !existing.hit && existing.allocations == 0);
    const auto second = allocation_fault::inject(0, [&] { store.get_or_create(1'010'000); });
    TEST_ASSERT(second.threw && second.hit && store.size() == 1);
    TEST_ASSERT(store.find(1'000'000) == first && pool.available() == 1);
    TEST_ASSERT(store.erase(1'000'000) == 1 && pool.available() == 2);
}

void test_new_level_failure_cleans_unlinked_order_and_fails_closed() {
    for (Side side : {Side::BUY, Side::SELL}) {
        for (bool record : {true, false}) {
            OrderBook book(4, record);
            FixedEventBuffer<4> sink;
            const Command command = order(1, side, 1'000'000, 1);
            const auto failure = allocation_fault::inject(0, [&] { book.process(command, sink); });
            TEST_ASSERT(failure.threw && failure.hit && failure.allocations == 1);
            TEST_ASSERT(sink.size() == 0 && book.engine_events().empty());
            std::cout << "new_level side=" << to_string(side) << " record=" << record
                      << " indexed=" << book.active_order_count()
                      << " free_levels=" << book.available_level_slots() << '\n';
            TEST_ASSERT(book.active_order_count() == 0 && book.available_level_slots() == 4);
            TEST_ASSERT(book.check_invariants());
            TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(0));
            assert_failed_and_blocked(book, 1);
        }
    }
}

void test_partial_match_failure_keeps_trades_but_not_orphaned_aggressive_order() {
    for (Side aggressive : {Side::BUY, Side::SELL}) {
        OrderBook book(4);
        NullEventSink seed;
        const Side passive = aggressive == Side::BUY ? Side::SELL : Side::BUY;
        const int64_t first_price = aggressive == Side::BUY ? 1'000'000 : 1'020'000;
        const int64_t limit = aggressive == Side::BUY ? 1'020'000 : 1'000'000;
        book.process(order(1, passive, first_price, 1), seed);
        book.process(order(2, passive, 1'010'000, 1), seed);
        FixedEventBuffer<4> sink;
        const Command sweep = order(3, aggressive, limit, 3);
        const auto failure = allocation_fault::inject(0, [&] { book.process(sweep, sink); });
        TEST_ASSERT(failure.threw && failure.hit && failure.allocations == 1);
        TEST_ASSERT(sink.size() == 0 && book.engine_events().size() == 2);
        const auto& first = std::get<TradeEvent>(book.engine_events()[0]);
        const auto& second = std::get<TradeEvent>(book.engine_events()[1]);
        TEST_ASSERT(first.command_sequence == CommandSequence(3) && first.event_index == EventIndex(0));
        TEST_ASSERT(first.passive_order_id == OrderId(1) && first.aggressive_order_id == OrderId(3));
        TEST_ASSERT(first.price == Price(first_price) && first.quantity == Quantity(1));
        TEST_ASSERT(second.event_index == EventIndex(1) && second.passive_order_id == OrderId(2));
        TEST_ASSERT(second.price == Price(1'010'000) && second.quantity == Quantity(1));
        std::cout << "partial_match side=" << to_string(aggressive)
                  << " indexed=" << book.active_order_count()
                  << " retained_trades=" << book.engine_events().size()
                  << " delivered=" << sink.size() << '\n';
        // Trades happened; this is structural cleanup, NOT a matching rollback.
        TEST_ASSERT(book.active_order_count() == 0 && book.available_level_slots() == 4);
        TEST_ASSERT(!book.best_bid() && !book.best_ask() && book.check_invariants());
        TEST_ASSERT(book.last_applied_command_sequence() == CommandSequence(2));
        assert_failed_and_blocked(book, 3);
    }
}

void test_cached_level_insert_and_cancel_do_not_allocate() {
    OrderBook book(4);
    NullEventSink seed;
    book.process(order(1, Side::BUY, 1'000'000, 1), seed);
    FixedEventBuffer<4> sink;
    const Command add = order(2, Side::BUY, 1'000'000, 1);
    ProcessResult result{ProcessStatus::REJECTED, 0, 0};
    const auto addition = allocation_fault::inject(0, [&] { result = book.process(add, sink); });
    TEST_ASSERT(!addition.hit && !addition.threw && addition.allocations == 0 && result.applied());
    const Command cancel = CancelOrderCommand(CommandSequence(3), OrderId(1));
    const auto removal = allocation_fault::inject(0, [&] { result = book.process(cancel, sink); });
    TEST_ASSERT(!removal.hit && !removal.threw && removal.allocations == 0 && result.applied());
    TEST_ASSERT(book.ready_for_commands() && book.check_invariants());
}

void test_partial_failure_preserves_unrelated_resting_orders() {
    for (Side aggressive : {Side::BUY, Side::SELL}) {
        OrderBook book(4);
        NullEventSink seed;
        const Side passive = aggressive == Side::BUY ? Side::SELL : Side::BUY;
        const int64_t own_price = aggressive == Side::BUY ? 990'000 : 1'010'000;
        const int64_t limit = aggressive == Side::BUY ? 1'010'000 : 990'000;
        book.process(order(1, aggressive, own_price, 2), seed);
        book.process(order(2, passive, 1'000'000, 1), seed);
        auto expected = book.capture_state();
        if (aggressive == Side::BUY) { expected.ask_levels.clear(); expected.best_ask.reset(); }
        else { expected.bid_levels.clear(); expected.best_bid.reset(); }
        expected.active_order_count = 1;
        FixedEventBuffer<4> sink;
        const Command command = order(3, aggressive, limit, 2);
        const auto failure = allocation_fault::inject(0, [&] { book.process(command, sink); });
        TEST_ASSERT(failure.hit && failure.threw && sink.size() == 0);
        TEST_ASSERT(book.capture_state() == expected && book.check_invariants());
        TEST_ASSERT(book.engine_events().size() == 1);
        const auto& trade = std::get<TradeEvent>(book.engine_events().front());
        TEST_ASSERT(trade.passive_order_id == OrderId(2) && trade.aggressive_order_id == OrderId(3));
        TEST_ASSERT(trade.price == Price(1'000'000) && trade.quantity == Quantity(1));
        assert_failed_and_blocked(book, 3);
    }
}

void test_maximum_ioc_batch_and_backpressure_use_reserved_storage() {
    OrderBook book(4, true, {}, 1);
    NullEventSink seed;
    for (uint64_t i = 1; i <= 3; ++i) book.process(order(i, Side::SELL, 1'000'000, 1), seed);
    const size_t capacity = book.event_log_capacity();
    FixedEventBuffer<1> sink;
    const Command sweep = order(4, Side::BUY, 1'000'000, 4, TimeInForce::IOC);
    ProcessResult result{ProcessStatus::REJECTED, 0, 0};
    const auto execution = allocation_fault::inject(0, [&] { result = book.process(sweep, sink); });
    TEST_ASSERT(!execution.hit && !execution.threw && execution.allocations == 0);
    TEST_ASSERT(result.applied() && result.event_count == 4 && !result.event_output_complete);
    TEST_ASSERT(book.pending_event_count() == 3 && book.event_log_capacity() == capacity);
    for (size_t remaining = 3; remaining != 0; --remaining) {
        sink.clear();
        bool complete = false;
        const auto retry = allocation_fault::inject(0, [&] { complete = book.retry_pending_events(sink); });
        TEST_ASSERT(!retry.hit && !retry.threw && retry.allocations == 0);
        TEST_ASSERT(complete == (remaining == 1));
    }
    TEST_ASSERT(book.active_order_count() == 0 && book.ready_for_commands() && book.check_invariants());
}

class CountingJournal final : public CommandJournal {
public:
    size_t begins = 0, commits = 0, aborts = 0;
    bool active = false;
    bool available() const noexcept override { return true; }
    bool begin_command(Timestamp, const Symbol&, const Command&, size_t) override {
        ++begins; active = true; return true;
    }
    bool append_trade(const TradeEvent&) override { return true; }
    bool commit_command(CommandOutcome) override { ++commits; active = false; return true; }
    void abort_command() noexcept override { if (active) { ++aborts; active = false; } }
};

void test_order_pool_operations_and_move_do_not_allocate() {
    ObjectPool<Order> pool(2), other(1);
    Order* first = pool.allocate();
    Order* second = pool.allocate();
    Order* foreign = other.allocate();
    Order stack(OrderId(1), PrioritySequence(1), Side::BUY, Price(100), Quantity(1));
    bool reused_same = false, exhausted = false;
    size_t final_available = 0, source_capacity = 1;
    const auto execution = allocation_fault::inject(0, [&] {
        pool.deallocate(first);
        pool.deallocate(first);
        pool.deallocate(nullptr);
        pool.deallocate(foreign);
        pool.deallocate(&stack);
        pool.deallocate(reinterpret_cast<Order*>(reinterpret_cast<unsigned char*>(first) + 1));
        Order* reused = pool.allocate();
        reused_same = reused == first; // Compare while the slot storage is alive.
        exhausted = pool.allocate() == nullptr;
        ObjectPool<Order> moved(std::move(pool));
        pool.deallocate(second);
        moved.deallocate(reused);
        moved.deallocate(second);
        final_available = moved.available();
        source_capacity = pool.capacity();
    });
    TEST_ASSERT(!execution.hit && !execution.threw && execution.allocations == 0);
    TEST_ASSERT(reused_same && exhausted && final_available == 2 && source_capacity == 0);
}

void test_logger_enqueue_allocation_failure_is_reported_without_throwing() {
    std::ostringstream output;
    AsyncLogger logger(output, 4);
    const auto event = SystemEvent::market_data(Timestamp(1), Symbol(std::string(512, 'S')),
        MarketDataEvent{Price(100), Quantity(1), Price(99), Price(101)});
    bool accepted = true;
    const auto execution = allocation_fault::inject(0, [&] { accepted = logger.enqueue(event); });
    TEST_ASSERT(execution.hit && !execution.threw && execution.allocations == 1 && !accepted);
    logger.stop();
    TEST_ASSERT(!logger.healthy() && logger.health() == LoggerHealth::ALLOCATION_FAILURE);
    TEST_ASSERT(logger.accepted_count() == 0 && logger.dropped_count() == 1 && logger.written_count() == 0);
    TEST_ASSERT(logger.failed_count() == 0);
}

void test_feed_encoder_success_uses_only_fixed_storage() {
    const FeedMessage add{{FeedMessageType::ADD_ORDER, 25, InstrumentId(0), 1, Timestamp(0)},
        FeedAddOrder{7, Price(100), Quantity(5), Side::BUY}};
    const FeedMessage cancel{{FeedMessageType::CANCEL_ORDER, 8, InstrumentId(0), 2, Timestamp(1)}, FeedCancelOrder{7}};
    const FeedMessage trade{{FeedMessageType::TRADE, 32, InstrumentId(0), 3, Timestamp(2)},
        FeedTrade{9, 7, Price(100), Quantity(2)}};
    size_t total = 0;
    uint64_t checksum = 0;
    const auto execution = allocation_fault::inject(0, [&] {
        for (const auto* message : {&add, &cancel, &trade}) {
            const auto frame = FeedEncoder::encode(*message);
            total += frame.size();
            for (size_t i = 0; i < frame.size(); ++i) checksum += frame.data()[i];
        }
    });
    TEST_ASSERT(!execution.hit && !execution.threw && execution.allocations == 0 && total == 49 + 32 + 56);
    TEST_ASSERT(checksum == 317);
}

FeedMessage consumer_add(uint32_t instrument, uint64_t sequence, uint64_t order_id,
                         int64_t price, uint64_t quantity, Side side) {
    return FeedMessage{{FeedMessageType::ADD_ORDER, 25, InstrumentId(instrument),
        sequence, Timestamp(0)}, FeedAddOrder{order_id, Price(price), Quantity(quantity), side}};
}

bool same_consumer_checkpoint(const MarketDataCheckpoint& left,
                              const MarketDataCheckpoint& right) {
    return left.books == right.books && left.sequences == right.sequences &&
        left.states == right.states && left.metrics.applied == right.metrics.applied &&
        left.metrics.gaps == right.metrics.gaps &&
        left.metrics.duplicates == right.metrics.duplicates &&
        left.metrics.out_of_order == right.metrics.out_of_order &&
        left.metrics.invalid_instruments == right.metrics.invalid_instruments &&
        left.metrics.invalid_messages == right.metrics.invalid_messages &&
        left.metrics.stale_messages == right.metrics.stale_messages;
}

void test_consumer_add_allocation_failures_roll_back() {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    for (Side side : {Side::BUY, Side::SELL}) {
        for (bool existing_level : {false, true}) {
            size_t hits = 0;
            bool completed = false;
            for (size_t fail_after = 0; fail_after < 64; ++fail_after) {
                MarketDataBook book;
                if (existing_level) {
                    TEST_ASSERT(book.apply(FeedAddOrder{1, Price(100), Quantity(maximum - 1), side}));
                }
                const auto before = book.capture_state();
                const FeedAddOrder addition{2, Price(100),
                    Quantity(existing_level ? 1 : maximum), side};
                bool accepted = false;
                const auto observation = allocation_fault::inject(fail_after, [&] {
                    accepted = book.apply(addition);
                });
                if (observation.hit) {
                    ++hits;
                    TEST_ASSERT(observation.threw && !accepted);
                    TEST_ASSERT(observation.allocations == fail_after + 1);
                    TEST_ASSERT(book.capture_state() == before);
                    TEST_ASSERT(book.order_count() == (existing_level ? 1 : 0));
                    TEST_ASSERT(book.bid_quantity(Price(100)) ==
                        (existing_level && side == Side::BUY ? maximum - 1 : 0));
                    TEST_ASSERT(book.ask_quantity(Price(100)) ==
                        (existing_level && side == Side::SELL ? maximum - 1 : 0));
                    // A normal retry must still be possible after structural rollback.
                    TEST_ASSERT(book.apply(addition));
                } else {
                    TEST_ASSERT(!observation.threw && accepted);
                    TEST_ASSERT(observation.allocations == fail_after);
                    completed = true;
                }
                TEST_ASSERT(book.order_count() == (existing_level ? 2 : 1));
                TEST_ASSERT(book.bid_quantity(Price(100)) == (side == Side::BUY ? maximum : 0));
                TEST_ASSERT(book.ask_quantity(Price(100)) == (side == Side::SELL ? maximum : 0));
                const auto full = book.capture_state();
                bool overflow_accepted = true;
                const auto overflow = allocation_fault::inject(0, [&] {
                    overflow_accepted = book.apply(FeedAddOrder{3, Price(100), Quantity(1), side});
                });
                TEST_ASSERT(!overflow.hit && !overflow.threw && overflow.allocations == 0);
                TEST_ASSERT(!overflow_accepted && book.capture_state() == full);
                TEST_ASSERT(book.apply(FeedCancelOrder{2}));
                TEST_ASSERT(book.capture_state() == before);
                if (completed) break;
            }
            TEST_ASSERT(completed && hits != 0);
            std::cout << "consumer_add side=" << to_string(side)
                      << " existing_level=" << existing_level << " failure_hits=" << hits << '\n';
        }
    }
}

void test_consumer_gateway_allocation_exceptions_fail_closed() {
    for (bool observed_before : {false, true}) {
        size_t hits = 0;
        bool completed = false, saw_advanced_sequence = false, saw_observe_failure = false;
        for (size_t fail_after = 0; fail_after < 64; ++fail_after) {
            MarketDataGateway gateway(2);
            TEST_ASSERT(gateway.on_message(consumer_add(1, 50, 50, 200, 7, Side::SELL)));
            if (observed_before) {
                TEST_ASSERT(gateway.on_message(consumer_add(0, 10, 1, 100, 5, Side::BUY)));
            }
            const auto before = gateway.checkpoint();
            const uint64_t next_sequence = observed_before ? 11 : 10;
            const auto message = consumer_add(0, next_sequence, 2, 101, 9, Side::BUY);
            bool accepted = false;
            const auto observation = allocation_fault::inject(fail_after, [&] {
                accepted = gateway.on_message(message);
            });
            if (observation.hit) {
                ++hits;
                TEST_ASSERT(observation.threw && !accepted);
                TEST_ASSERT(observation.allocations == fail_after + 1);
                TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::STALE);
                TEST_ASSERT(gateway.state(InstrumentId(1)) == FeedState::HEALTHY);
                const auto after = gateway.checkpoint();
                TEST_ASSERT(after.books == before.books);
                TEST_ASSERT(after.sequences[1] == before.sequences[1]);
                TEST_ASSERT(after.sequences[0] == before.sequences[0] ||
                            after.sequences[0] == next_sequence);
                saw_advanced_sequence |= after.sequences[0] == next_sequence;
                saw_observe_failure |= after.sequences[0] == before.sequences[0];
                auto expected = before;
                expected.states[0] = FeedState::STALE;
                expected.sequences[0] = after.sequences[0];
                TEST_ASSERT(same_consumer_checkpoint(after, expected));
                bool later_accepted = true;
                const auto blocked = allocation_fault::inject(0, [&] {
                    later_accepted = gateway.on_message(message);
                });
                TEST_ASSERT(!blocked.hit && !blocked.threw && blocked.allocations == 0 && !later_accepted);
                TEST_ASSERT(gateway.metrics().stale_messages == 1);
                TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == after.sequences[0]);
            } else {
                TEST_ASSERT(!observation.threw && accepted);
                TEST_ASSERT(observation.allocations == fail_after);
                TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::HEALTHY);
                TEST_ASSERT(gateway.state(InstrumentId(1)) == FeedState::HEALTHY);
                TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == next_sequence);
                TEST_ASSERT(gateway.book(InstrumentId(0))->bid_quantity(Price(101)) == 9);
                TEST_ASSERT(gateway.metrics().applied == before.metrics.applied + 1);
                completed = true;
                break;
            }
        }
        TEST_ASSERT(completed && hits != 0 && saw_advanced_sequence);
        TEST_ASSERT(observed_before || saw_observe_failure);
        std::cout << "consumer_gateway observed_before=" << observed_before
                  << " failure_hits=" << hits << '\n';
    }
}

void test_consumer_checkpoint_allocation_failures_are_transactional() {
    MarketDataGateway source(3);
    TEST_ASSERT(source.on_message(consumer_add(0, 30, 11, 110, 8, Side::SELL)));
    TEST_ASSERT(source.on_message(consumer_add(0, 31, 12, 111, 9, Side::BUY)));
    TEST_ASSERT(source.on_message(consumer_add(1, 40, 13, 210, 10, Side::BUY)));
    TEST_ASSERT(source.on_message(consumer_add(2, 50, 14, 310, 11, Side::SELL)));
    TEST_ASSERT(!source.on_message(consumer_add(0, 31, 15, 112, 1, Side::BUY)));
    source.invalidate(InstrumentId(1));
    const auto replacement = source.checkpoint();
    size_t hits = 0;
    bool completed = false;
    for (size_t fail_after = 0; fail_after < 128; ++fail_after) {
        MarketDataGateway gateway(3);
        TEST_ASSERT(gateway.on_message(consumer_add(0, 10, 1, 100, 5, Side::BUY)));
        TEST_ASSERT(gateway.on_message(consumer_add(1, 20, 2, 200, 7, Side::SELL)));
        TEST_ASSERT(gateway.on_message(consumer_add(2, 25, 3, 300, 6, Side::BUY)));
        gateway.invalidate(InstrumentId(2));
        const auto before = gateway.checkpoint();
        const auto observation = allocation_fault::inject(fail_after, [&] {
            gateway.restore_checkpoint(replacement);
        });
        if (observation.hit) {
            ++hits;
            TEST_ASSERT(observation.threw && observation.allocations == fail_after + 1);
            TEST_ASSERT(same_consumer_checkpoint(gateway.checkpoint(), before));
        } else {
            TEST_ASSERT(!observation.threw && observation.allocations == fail_after);
            TEST_ASSERT(same_consumer_checkpoint(gateway.checkpoint(), replacement));
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed && hits != 0);
    std::cout << "consumer_checkpoint failure_hits=" << hits << '\n';
}

void test_engine_aborts_uncommitted_journal_after_internal_allocation_failure() {
    MarketManager markets(4);
    EventDispatcher dispatcher;
    CountingJournal journal;
    TradingEngine engine(markets, dispatcher, journal);
    size_t orders = 0, other_events = 0;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent& event) {
        if (event.event_type == SystemEventType::ORDER) {
            ++orders;
            // Arm after SystemEvent/Symbol copies, at the exact core boundary.
            allocation_fault::remaining = 0;
            allocation_fault::attempts = 0;
            allocation_fault::triggered = false;
            allocation_fault::armed = true;
        } else ++other_events;
    });
    dispatcher.freeze();
    bool threw = false;
    try { engine.submit(Timestamp(1), Symbol("AAPL"), order(1, Side::BUY, 1'000'000, 1)); }
    catch (const std::bad_alloc&) { threw = true; }
    allocation_fault::armed = false;
    TEST_ASSERT(threw && allocation_fault::triggered && allocation_fault::attempts == 1);
    TEST_ASSERT(engine.failed() && journal.begins == 1 && journal.aborts == 1 && journal.commits == 0);
    TEST_ASSERT(orders == 1 && other_events == 0);
    auto* book = markets.find_book(Symbol("AAPL"));
    TEST_ASSERT(book->active_order_count() == 0 && book->check_invariants());
    assert_failed_and_blocked(*book, 1);
    TEST_ASSERT(engine.submit(Timestamp(2), Symbol("TSLA"), order(1, Side::BUY, 1'000'000, 1)).routing_status ==
        RoutingStatus::SYSTEM_UNAVAILABLE);
    TEST_ASSERT(orders == 1);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--new-level-only") {
            test_new_level_failure_cleans_unlinked_order_and_fails_closed();
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--partial-only") {
            test_partial_match_failure_keeps_trades_but_not_orphaned_aggressive_order();
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--feed-consumer-only") {
            test_feed_encoder_success_uses_only_fixed_storage();
            test_consumer_add_allocation_failures_roll_back();
            test_consumer_gateway_allocation_exceptions_fail_closed();
            test_consumer_checkpoint_allocation_failures_are_transactional();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown allocation failure test option");
        test_store_returns_acquired_level_when_map_allocation_fails();
        test_new_level_failure_cleans_unlinked_order_and_fails_closed();
        test_partial_match_failure_keeps_trades_but_not_orphaned_aggressive_order();
        test_cached_level_insert_and_cancel_do_not_allocate();
        test_partial_failure_preserves_unrelated_resting_orders();
        test_maximum_ioc_batch_and_backpressure_use_reserved_storage();
        test_order_pool_operations_and_move_do_not_allocate();
        test_logger_enqueue_allocation_failure_is_reported_without_throwing();
        test_feed_encoder_success_uses_only_fixed_storage();
        test_consumer_add_allocation_failures_roll_back();
        test_consumer_gateway_allocation_exceptions_fail_closed();
        test_consumer_checkpoint_allocation_failures_are_transactional();
        test_engine_aborts_uncommitted_journal_after_internal_allocation_failure();
        std::cout << "Allocation failure tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        allocation_fault::armed = false;
        std::cerr << "Allocation failure tests failed: " << error.what() << '\n';
        return 1;
    }
}
