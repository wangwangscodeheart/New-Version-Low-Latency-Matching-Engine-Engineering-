// Executable-local fault injection only. Do not link these allocator
// replacements into normal tests, demos, the rebuilder, or benchmarks.
#include "itch_book_rebuilder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace native_book_fault {
thread_local std::size_t remaining = 0;
thread_local bool armed = false;
thread_local bool triggered = false;
thread_local std::size_t attempts = 0;

void checkpoint() {
    if (!armed) return;
    ++attempts;
    if (remaining == 0) {
        armed = false; // Diagnostics and exception handling must still allocate.
        triggered = true;
        throw std::bad_alloc();
    }
    --remaining;
}
void* allocate(std::size_t size) {
    checkpoint();
    if (void* value = std::malloc(size == 0 ? 1 : size)) return value;
    throw std::bad_alloc();
}
void* allocate_aligned(std::size_t size, std::size_t alignment) {
    checkpoint();
    size = size == 0 ? 1 : size;
#ifdef _WIN32
    if (void* value = _aligned_malloc(size, alignment)) return value;
#else
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) throw std::bad_alloc();
    const auto rounded = (size + alignment - 1) / alignment * alignment;
    if (void* value = std::aligned_alloc(alignment, rounded)) return value;
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
struct Observation { bool threw; bool hit; std::size_t allocations; };
template<typename Action>
Observation inject(std::size_t successful_allocations, Action&& action) {
    remaining = successful_allocations;
    triggered = false;
    attempts = 0;
    armed = true;
    bool threw = false;
    try { action(); }
    catch (const std::bad_alloc&) { threw = true; }
    catch (...) { armed = false; throw; }
    armed = false; // Disarm before any assertion, snapshot, stream or oracle.
    return {threw, triggered, attempts};
}
} // namespace native_book_fault

void* operator new(std::size_t size) { return native_book_fault::allocate(size); }
void* operator new[](std::size_t size) { return native_book_fault::allocate(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return native_book_fault::allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return native_book_fault::allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    return native_book_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return native_book_fault::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value, std::align_val_t) noexcept { native_book_fault::free_aligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { native_book_fault::free_aligned(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { native_book_fault::free_aligned(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { native_book_fault::free_aligned(value); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return native_book_fault::allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return native_book_fault::allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    native_book_fault::free_aligned(value);
}
void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    native_book_fault::free_aligned(value);
}

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
constexpr std::uint64_t NEW_ID = 100000;
constexpr std::uint64_t NEW_QUANTITY = 23;
constexpr std::size_t MAXIMUM_FAULT_POINTS = 64;
std::size_t scenarios = 0;
std::size_t injected_failures = 0;
std::size_t session_scenarios = 0;
std::size_t session_injected_failures = 0;
std::size_t metadata_injected_failures = 0;
std::size_t report_injected_failures = 0;

MarketDataBook fixture(Side side, std::size_t order_count) {
    TEST_ASSERT(order_count >= 4 && order_count < NEW_ID - 10);
    MarketDataBook book;
    TEST_ASSERT(book.add_order(1, Price(100), Quantity(10), side));
    TEST_ASSERT(book.add_order(2, Price(100), Quantity(7), side));
    TEST_ASSERT(book.add_order(3, Price(110), Quantity(5), side));
    const Side opposite = side == Side::BUY ? Side::SELL : Side::BUY;
    TEST_ASSERT(book.add_order(4, Price(120), Quantity(6), opposite));
    for (std::size_t i = 4; i < order_count; ++i) {
        TEST_ASSERT(book.add_order(static_cast<std::uint64_t>(i + 10), Price(100), Quantity(1), side));
    }
    TEST_ASSERT(book.check_invariants() && book.order_count() == order_count);
    return book;
}

// An independent state-derived oracle compares every order, depth on all
// relevant prices (including absent prices), and both sides of BBO.
void verify(const MarketDataBook& book, const MarketDataBookState& expected) {
    TEST_ASSERT(!native_book_fault::armed);
    TEST_ASSERT(book.capture_state() == expected);
    TEST_ASSERT(book.check_invariants());
    std::map<std::int64_t, std::uint64_t> bids, asks;
    for (const auto& order : expected.orders) {
        (order.side == Side::BUY ? bids : asks)[order.price.get()] += order.remaining.get();
        const auto* actual = book.find_order(order.order_id);
        TEST_ASSERT(actual && actual->price == order.price && actual->remaining == order.remaining &&
                    actual->side == order.side);
    }
    for (const auto price : {std::int64_t{90}, std::int64_t{100}, std::int64_t{105},
                             std::int64_t{110}, std::int64_t{120}, std::int64_t{999}}) {
        const auto bid = bids.find(price), ask = asks.find(price);
        TEST_ASSERT(book.bid_quantity(Price(price)) == (bid == bids.end() ? 0 : bid->second));
        TEST_ASSERT(book.ask_quantity(Price(price)) == (ask == asks.end() ? 0 : ask->second));
    }
    const auto expected_bid = bids.empty() ? std::optional<Price>{} : std::optional<Price>{Price(bids.rbegin()->first)};
    const auto expected_ask = asks.empty() ? std::optional<Price>{} : std::optional<Price>{Price(asks.begin()->first)};
    TEST_ASSERT(book.best_bid() == expected_bid && book.best_ask() == expected_ask);
}

MarketDataBookState replaced_state(MarketDataBookState state, std::int64_t target) {
    const auto old = std::find_if(state.orders.begin(), state.orders.end(),
                                  [](const auto& order) { return order.order_id == 1; });
    TEST_ASSERT(old != state.orders.end());
    old->order_id = NEW_ID;
    old->price = Price(target);
    old->remaining = Quantity(NEW_QUANTITY);
    std::sort(state.orders.begin(), state.orders.end(),
              [](const auto& left, const auto& right) { return left.order_id < right.order_id; });
    return state;
}

void scan_replace(Side side, std::size_t count, std::int64_t target) {
    bool completed = false;
    for (std::size_t failpoint = 0; failpoint < MAXIMUM_FAULT_POINTS; ++failpoint) {
        auto book = fixture(side, count);
        const auto before = book.capture_state();
        const auto after = replaced_state(before, target);
        bool applied = false;
        const auto observed = native_book_fault::inject(failpoint, [&] {
            applied = book.replace_order(1, NEW_ID, Price(target), Quantity(NEW_QUANTITY));
        });
        if (observed.hit) {
            TEST_ASSERT(observed.threw && !applied && observed.allocations == failpoint + 1);
            ++injected_failures;
            verify(book, before);
            TEST_ASSERT(book.find_order(1) && book.find_order(NEW_ID) == nullptr);
            // Failure must not poison the generic book: an unarmed retry works.
            TEST_ASSERT(book.replace_order(1, NEW_ID, Price(target), Quantity(NEW_QUANTITY)));
            verify(book, after);
        } else {
            TEST_ASSERT(!observed.threw && applied && observed.allocations == failpoint);
            TEST_ASSERT(book.find_order(1) == nullptr && book.find_order(NEW_ID));
            verify(book, after);
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed);
    ++scenarios;
}

void scan_add(Side side, std::size_t count, std::int64_t target) {
    bool completed = false;
    for (std::size_t failpoint = 0; failpoint < MAXIMUM_FAULT_POINTS; ++failpoint) {
        auto book = fixture(side, count);
        const auto before = book.capture_state();
        auto after = before;
        after.orders.push_back({NEW_ID, Price(target), Quantity(NEW_QUANTITY), side});
        bool applied = false;
        const auto observed = native_book_fault::inject(failpoint, [&] {
            applied = book.add_order(NEW_ID, Price(target), Quantity(NEW_QUANTITY), side);
        });
        if (observed.hit) {
            TEST_ASSERT(observed.threw && !applied && observed.allocations == failpoint + 1);
            ++injected_failures;
            verify(book, before);
            TEST_ASSERT(book.find_order(NEW_ID) == nullptr);
            TEST_ASSERT(book.add_order(NEW_ID, Price(target), Quantity(NEW_QUANTITY), side));
            verify(book, after);
        } else {
            TEST_ASSERT(!observed.threw && applied && observed.allocations == failpoint);
            verify(book, after);
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed);
    ++scenarios;
}

struct GrowthBoundary { std::size_t count; std::size_t ordinary_allocations; std::size_t growth_allocations; };
GrowthBoundary find_growth_boundary() {
    // Existing-price replacement allocates only through orders_.emplace.
    // Discover extra node/bucket work without exposing private bucket counts or
    // assuming a particular STL's bucket sizes. Each trial is a fresh fixture.
    std::size_t smallest = std::numeric_limits<std::size_t>::max();
    for (std::size_t count = 4; count <= 1024; ++count) {
        auto book = fixture(Side::BUY, count);
        bool applied = false;
        const auto observed = native_book_fault::inject(std::numeric_limits<std::size_t>::max(), [&] {
            applied = book.replace_order(1, NEW_ID, Price(100), Quantity(NEW_QUANTITY));
        });
        TEST_ASSERT(!observed.hit && !observed.threw && applied && book.check_invariants());
        if (observed.allocations > smallest) return {count, smallest, observed.allocations};
        smallest = std::min(smallest, observed.allocations);
    }
    throw std::runtime_error("no extra unordered_map growth allocation observed within 1024 fresh sizes");
}

void test_nonallocating_reduction_and_erase() {
    for (const Side side : {Side::BUY, Side::SELL}) {
        for (const int operation : {0, 1, 2}) {
            auto book = fixture(side, 4);
            auto expected = book.capture_state();
            const auto old = std::find_if(expected.orders.begin(), expected.orders.end(),
                                          [](const auto& order) { return order.order_id == 1; });
            TEST_ASSERT(old != expected.orders.end());
            if (operation == 0) old->remaining = Quantity(7);
            else expected.orders.erase(old);
            bool applied = false;
            const auto observed = native_book_fault::inject(0, [&] {
                applied = operation == 2 ? book.erase_order(1) :
                    book.reduce_order(1, Quantity(operation == 0 ? 3 : 10));
            });
            TEST_ASSERT(applied && !observed.threw && !observed.hit && observed.allocations == 0);
            verify(book, expected);
        }
    }
}

using Bytes = std::vector<std::uint8_t>;
void append_be(Bytes& output, std::uint64_t value, std::size_t width) {
    for (std::size_t i = width; i != 0; --i) {
        output.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}
Bytes session_frame(char type, std::uint16_t locate, const Bytes& body) {
    // Self-invented BinaryFILE bytes, never sourced from a market-data file.
    Bytes result;
    append_be(result, 11 + body.size(), 2);
    result.push_back(static_cast<std::uint8_t>(type));
    append_be(result, locate, 2);
    append_be(result, 37, 2); // Tracking is diagnostic, not a sequence.
    append_be(result, 123, 6);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
Bytes directory_frame() {
    return session_frame('R', 1, Bytes{
        'A','A','P','L',' ',' ',' ',' ', 'Q','N', 0,0,0,100,
        'N','C','A','B','P','N','N','1','N', 0,0,0,1, 'N'});
}
Bytes native_add_frame(char type, std::uint64_t reference, std::uint32_t price) {
    Bytes body;
    append_be(body, reference, 8);
    body.push_back('B');
    append_be(body, 10, 4);
    body.insert(body.end(), {'A','A','P','L',' ',' ',' ',' '});
    append_be(body, price, 4);
    if (type == 'F') body.insert(body.end(), {'A','B','C','D'});
    return session_frame(type, 1, body);
}
Bytes native_replace_frame() {
    Bytes body;
    append_be(body, 11, 8);
    append_be(body, 12, 8);
    append_be(body, 20, 4);
    append_be(body, 105, 4);
    return session_frame('U', 1, body);
}
void ingest(nasdaq_itch::ItchBookRebuilder& session, const Bytes& bytes) {
    session.consume(bytes.data(), bytes.size());
}

enum class SessionOperation { DIRECTORY, ADD, REPLACE };
void scan_session_operation(SessionOperation operation) {
    const auto target = operation == SessionOperation::DIRECTORY ? directory_frame() :
        (operation == SessionOperation::ADD ? native_add_frame('A', 12, 105) : native_replace_frame());
    const unsigned char type = operation == SessionOperation::DIRECTORY ? 'R' :
        (operation == SessionOperation::ADD ? 'A' : 'U');
    bool completed = false;
    for (std::size_t failpoint = 0; failpoint < MAXIMUM_FAULT_POINTS; ++failpoint) {
        nasdaq_itch::ItchBookRebuilder session;
        ingest(session, session_frame('S', 0, Bytes{'O'}));
        if (operation != SessionOperation::DIRECTORY) {
            ingest(session, directory_frame());
            ingest(session, native_add_frame('F', 11, 100));
        }
        const auto* prior_book = session.book(1);
        const auto before = prior_book ? prior_book->capture_state() : MarketDataBookState{};
        const auto before_metrics = session.metrics();
        const auto before_instruments = session.instrument_count();
        const auto before_active = session.active_order_count();
        const auto before_seen = session.seen_order_reference_count();
        TEST_ASSERT(session.check_invariants());

        const auto observed = native_book_fault::inject(failpoint, [&] { ingest(session, target); });
        if (observed.hit) {
            TEST_ASSERT(observed.threw && observed.allocations == failpoint + 1);
            ++session_injected_failures;
            TEST_ASSERT(session.state() == nasdaq_itch::RebuildState::STALE);
            TEST_ASSERT(session.metrics().errors == 1 && session.book(1) == nullptr);
            TEST_ASSERT(session.metrics().processed_records == before_metrics.processed_records);
            TEST_ASSERT(session.metrics().records_by_type == before_metrics.records_by_type);
            TEST_ASSERT(session.instrument_count() == before_instruments);
            TEST_ASSERT(session.active_order_count() == before_active);
            TEST_ASSERT(session.seen_order_reference_count() == before_seen);
            TEST_ASSERT(session.check_invariants());
            if (prior_book) {
                // A pointer borrowed before failure remains diagnostic-only;
                // the public session lookup refuses to expose a trusted book.
                verify(*prior_book, before);
                TEST_ASSERT(session.locate("AAPL") == std::uint16_t{1});
            } else {
                TEST_ASSERT(!session.locate("AAPL") && session.symbol(1).empty());
            }
            bool blocked = false;
            try { session.consume(nullptr, 0); }
            catch (const nasdaq_itch::RebuildError&) { blocked = true; }
            TEST_ASSERT(blocked && session.metrics().errors == 1);
        } else {
            TEST_ASSERT(!observed.threw && observed.allocations == failpoint);
            TEST_ASSERT(session.state() == nasdaq_itch::RebuildState::REBUILDING);
            TEST_ASSERT(session.metrics().errors == 0);
            TEST_ASSERT(session.metrics().processed_records == before_metrics.processed_records + 1);
            TEST_ASSERT(session.metrics().records_by_type[type] == before_metrics.records_by_type[type] + 1);
            TEST_ASSERT(session.book(1) && session.locate("AAPL") == std::uint16_t{1});
            TEST_ASSERT(session.check_invariants());
            if (operation == SessionOperation::DIRECTORY) {
                TEST_ASSERT(session.instrument_count() == 1 && session.active_order_count() == 0 &&
                            session.seen_order_reference_count() == 0);
                verify(*session.book(1), MarketDataBookState{});
            } else {
                TEST_ASSERT(session.instrument_count() == 1 && session.seen_order_reference_count() == 2);
                auto expected = before;
                if (operation == SessionOperation::ADD) {
                    expected.orders.push_back({12, Price(105), Quantity(10), Side::BUY});
                    TEST_ASSERT(session.active_order_count() == 2);
                } else {
                    TEST_ASSERT(expected.orders.size() == 1);
                    expected.orders[0] = {12, Price(105), Quantity(20), Side::BUY};
                    TEST_ASSERT(session.active_order_count() == 1);
                    TEST_ASSERT(!session.attribution(11));
                    const auto attribution = session.attribution(12);
                    TEST_ASSERT(attribution && nasdaq_itch::text_view(*attribution) == "ABCD");
                }
                verify(*session.book(1), expected);
            }
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed);
    ++session_scenarios;
}

Bytes participant_frame(const Bytes& mpid, char mode = 'N', char state = 'A') {
    TEST_ASSERT(mpid.size() == 4);
    Bytes body = mpid;
    body.insert(body.end(), {'A','A','P','L',' ',' ',' ',' ', 'Y',
        static_cast<std::uint8_t>(mode), static_cast<std::uint8_t>(state)});
    return session_frame('L', 1, body);
}
void scan_participant_metadata_allocation() {
    const nasdaq_itch::FixedText<4> first{{'A','B','C','D'}};
    const nasdaq_itch::FixedText<4> second{{'E','F','G','H'}};
    const auto initial = participant_frame(Bytes(first.begin(), first.end()));
    const auto insertion = participant_frame(Bytes(second.begin(), second.end()));
    const auto update = participant_frame(Bytes(first.begin(), first.end()), 'P', 'S');
    bool completed = false;
    for (std::size_t failpoint = 0; failpoint < MAXIMUM_FAULT_POINTS; ++failpoint) {
        nasdaq_itch::ItchBookRebuilder session;
        ingest(session, session_frame('S', 0, Bytes{'O'}));
        ingest(session, directory_frame());
        ingest(session, native_add_frame('F', 11, 100));
        ingest(session, initial);
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_metadata = session.metadata(1);
        const auto before = diagnostic_book->capture_state();
        const auto metrics = session.metrics();
        const auto result = native_book_fault::inject(failpoint, [&] { ingest(session, insertion); });
        verify(*diagnostic_book, before);
        TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 1);
        TEST_ASSERT(session.check_invariants());
        if (result.hit) {
            ++metadata_injected_failures;
            TEST_ASSERT(result.threw && result.allocations == failpoint + 1);
            TEST_ASSERT(session.state() == nasdaq_itch::RebuildState::STALE && session.metrics().errors == 1);
            TEST_ASSERT(!session.book(1) && !session.metadata(1) && !session.market_wide_metadata());
            TEST_ASSERT(session.metrics().processed_records == metrics.processed_records);
            TEST_ASSERT(session.metrics().records_by_type == metrics.records_by_type);
            TEST_ASSERT(diagnostic_metadata->market_participants.size() == 1);
            TEST_ASSERT(diagnostic_metadata->market_participants.at(first).market_maker_mode == 'N');
            TEST_ASSERT(diagnostic_metadata->market_participants.find(second) == diagnostic_metadata->market_participants.end());
        } else {
            TEST_ASSERT(!result.threw && result.allocations == failpoint);
            TEST_ASSERT(session.metadata(1)->market_participants.size() == 2);
            TEST_ASSERT(session.metrics().processed_records == metrics.processed_records + 1);
            TEST_ASSERT(session.metrics().records_by_type['L'] == metrics.records_by_type['L'] + 1);
            const auto updated = native_book_fault::inject(0, [&] { ingest(session, update); });
            TEST_ASSERT(!updated.threw && !updated.hit && updated.allocations == 0);
            TEST_ASSERT(session.metadata(1)->market_participants.at(first).market_maker_mode == 'P');
            TEST_ASSERT(session.metadata(1)->market_participants.at(first).market_participant_state == 'S');
            verify(*session.book(1), before);
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed);
}
void scan_correlated_execution_allocation() {
    Bytes execution_body;
    append_be(execution_body, 11, 8);
    append_be(execution_body, 3, 4);
    append_be(execution_body, 1001, 8);
    const auto execution = session_frame('E', 1, execution_body);
    Bytes broken_body;
    append_be(broken_body, 1001, 8);
    const auto broken = session_frame('B', 1, broken_body);
    bool completed = false;
    for (std::size_t failpoint = 0; failpoint < MAXIMUM_FAULT_POINTS; ++failpoint) {
        nasdaq_itch::RebuildValidation validation;
        validation.correlate_trade_reports = true;
        validation.validate_intraday_timestamps = true;
        nasdaq_itch::ItchBookRebuilder session(nasdaq_itch::RebuildLimits{}, validation);
        ingest(session, session_frame('S', 0, Bytes{'O'}));
        ingest(session, directory_frame());
        ingest(session, native_add_frame('F', 11, 100));
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_reports = session.trade_ledger();
        const auto* diagnostic_clock = session.temporal_metrics();
        const auto before = diagnostic_book->capture_state();
        const auto metrics = session.metrics();
        const auto observations = diagnostic_clock->observations;
        const auto result = native_book_fault::inject(failpoint, [&] { ingest(session, execution); });
        TEST_ASSERT(session.check_invariants());
        if (result.hit) {
            ++report_injected_failures;
            TEST_ASSERT(result.threw && result.allocations == failpoint + 1);
            TEST_ASSERT(session.state() == nasdaq_itch::RebuildState::STALE && session.metrics().errors == 1);
            TEST_ASSERT(!session.book(1) && !session.trade_ledger() && !session.temporal_metrics());
            verify(*diagnostic_book, before);
            TEST_ASSERT(diagnostic_reports->size() == 0 && diagnostic_reports->totals().reports == 0);
            TEST_ASSERT(diagnostic_reports->totals().printable_volume == 0);
            TEST_ASSERT(diagnostic_clock->observations == observations);
            TEST_ASSERT(session.metrics().processed_records == metrics.processed_records);
            TEST_ASSERT(session.metrics().records_by_type == metrics.records_by_type);
        } else {
            TEST_ASSERT(!result.threw && result.allocations == failpoint);
            TEST_ASSERT(session.book(1)->find_order(11)->remaining == Quantity(7));
            TEST_ASSERT(session.trade_ledger()->find(1001)->price == 100);
            TEST_ASSERT(session.trade_ledger()->totals().printable_volume == 3);
            TEST_ASSERT(session.temporal_metrics()->observations == observations + 1);
            const auto after_execution = session.book(1)->capture_state();
            const auto no_allocation = native_book_fault::inject(0, [&] { ingest(session, broken); });
            TEST_ASSERT(!no_allocation.threw && !no_allocation.hit && no_allocation.allocations == 0);
            TEST_ASSERT(session.trade_ledger()->find(1001)->broken);
            TEST_ASSERT(session.trade_ledger()->totals().printable_volume == 0);
            verify(*session.book(1), after_execution); // Never resurrect executed shares.
            completed = true;
            break;
        }
    }
    TEST_ASSERT(completed);
}
} // namespace

int main() {
    try {
        const auto boundary = find_growth_boundary();
        for (const Side side : {Side::BUY, Side::SELL}) {
            for (const auto count : {std::size_t{4}, boundary.count}) {
                for (const auto target : {std::int64_t{100}, std::int64_t{110}, std::int64_t{105}}) {
                    scan_replace(side, count, target);
                }
                for (const auto target : {std::int64_t{100}, std::int64_t{105}}) scan_add(side, count, target);
            }
        }
        test_nonallocating_reduction_and_erase();
        scan_session_operation(SessionOperation::DIRECTORY);
        scan_session_operation(SessionOperation::ADD);
        scan_session_operation(SessionOperation::REPLACE);
        scan_participant_metadata_allocation();
        scan_correlated_execution_allocation();
        std::cout << "MarketDataBook native allocation tests passed: scans=" << scenarios
                  << " injected_failures=" << injected_failures
                  << " growth_order_count=" << boundary.count
                  << " ordinary_replace_allocations=" << boundary.ordinary_allocations
                  << " growth_replace_allocations=" << boundary.growth_allocations << '\n';
        std::cout << "Native ITCH session allocation tests passed: scans=" << session_scenarios
                  << " injected_failures=" << session_injected_failures << '\n';
        std::cout << "Participant metadata allocation tests passed: scans=1 injected_failures="
                  << metadata_injected_failures << " existing_update_allocations=0\n";
        std::cout << "Correlated execution allocation tests passed: scans=1 injected_failures="
                  << report_injected_failures << " valid_break_allocations=0\n";
        std::cout << "Allocator replacements are executable-local; this is not a global ASan/allocation-coverage claim.\n";
        return 0;
    } catch (const std::exception& error) {
        native_book_fault::armed = false;
        std::cerr << "MarketDataBook native allocation test failed: " << error.what() << '\n';
        return 1;
    }
}
