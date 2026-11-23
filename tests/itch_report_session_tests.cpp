#include "itch_book_rebuilder.hpp"
#include "itch_trade_ledger.hpp"
#include "itch_session_validation.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;

// All values and wire bytes are invented for this test. These field helpers
// do not encode downloaded records or use a paired production ITCH encoder.
void number(Bytes& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = width; i != 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}
void stock(Bytes& bytes, std::string_view name) {
    TEST_ASSERT(name.size() <= 8);
    for (const char value : name) bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.insert(bytes.end(), 8 - name.size(), std::uint8_t{' '});
}
Bytes message(char type, std::uint16_t locate, const Bytes& body, std::uint64_t timestamp = 7) {
    Bytes result{static_cast<std::uint8_t>(type)};
    number(result, locate, 2);
    number(result, 0x7654, 2); // Deliberately repeated tracking number, not a transport sequence.
    number(result, timestamp, 6);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
Bytes system_event(char code, std::uint64_t timestamp = 7) {
    return message('S', 0, Bytes{static_cast<std::uint8_t>(code)}, timestamp);
}
Bytes directory(std::uint16_t locate, std::string_view name, std::uint64_t timestamp = 7) {
    Bytes body;
    stock(body, name);
    body.insert(body.end(), {'Q', 'N'});
    number(body, 100, 4);
    body.insert(body.end(), {'N', 'C', ' ', ' ', 'P', 'N', 'N', '1', 'N'});
    number(body, 0, 4);
    body.push_back('N');
    return message('R', locate, body, timestamp);
}
Bytes add(std::uint16_t locate, std::uint64_t reference, std::string_view name,
          std::uint32_t price, std::uint32_t shares, std::uint64_t timestamp = 7) {
    Bytes body;
    number(body, reference, 8);
    body.push_back('S');
    number(body, shares, 4);
    stock(body, name);
    number(body, price, 4);
    return message('A', locate, body, timestamp);
}
Bytes execute(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares,
              std::uint64_t match, std::uint64_t timestamp = 7) {
    Bytes body;
    number(body, reference, 8);
    number(body, shares, 4);
    number(body, match, 8);
    return message('E', locate, body, timestamp);
}
Bytes priced_execute(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares,
                     std::uint64_t match, std::uint32_t price, char printable,
                     std::uint64_t timestamp = 7) {
    auto result = execute(locate, reference, shares, match, timestamp);
    result[0] = 'C';
    result.push_back(static_cast<std::uint8_t>(printable));
    number(result, price, 4);
    return result;
}
Bytes hidden(std::uint16_t locate, std::string_view name, std::uint32_t shares,
             std::uint64_t match, std::uint32_t price = 102, std::uint64_t timestamp = 7) {
    Bytes body;
    number(body, 0, 8);
    body.push_back('B');
    number(body, shares, 4);
    stock(body, name);
    number(body, price, 4);
    number(body, match, 8);
    return message('P', locate, body, timestamp);
}
Bytes cross(std::uint16_t locate, std::string_view name, std::uint64_t shares,
            std::uint64_t match, std::uint32_t price = 201, std::uint64_t timestamp = 7) {
    Bytes body;
    number(body, shares, 8);
    stock(body, name);
    number(body, price, 4);
    number(body, match, 8);
    body.push_back('O');
    return message('Q', locate, body, timestamp);
}
Bytes broken(std::uint16_t locate, std::uint64_t match, std::uint64_t timestamp = 7) {
    Bytes body;
    number(body, match, 8);
    return message('B', locate, body, timestamp);
}
Bytes erase(std::uint16_t locate, std::uint64_t reference) {
    Bytes body;
    number(body, reference, 8);
    return message('D', locate, body);
}
void append_frame(Bytes& bytes, const Bytes& payload) {
    TEST_ASSERT(!payload.empty() && payload.size() <= 65535);
    number(bytes, static_cast<std::uint64_t>(payload.size()), 2);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
}
void send(ItchBookRebuilder& session, const Bytes& payload) {
    Bytes framed;
    append_frame(framed, payload);
    session.consume(framed.data(), framed.size());
}
RebuildValidation validation(bool reports = true, bool temporal = true, bool all_phases = false) {
    RebuildValidation result;
    result.correlate_trade_reports = reports;
    result.validate_intraday_timestamps = temporal;
    result.require_all_system_events = all_phases;
    return result;
}
void start(ItchBookRebuilder& session, std::uint64_t timestamp = 7) {
    send(session, system_event('O', timestamp));
    send(session, directory(1, "AAPL", timestamp));
    send(session, directory(2, "MSFT", timestamp));
    send(session, add(1, 100, "AAPL", 100, 20, timestamp));
    send(session, add(2, 200, "MSFT", 200, 10, timestamp));
}
void complete(ItchBookRebuilder& session, std::uint64_t timestamp = 7) {
    send(session, system_event('C', timestamp));
    const std::uint8_t marker[]{0, 0};
    session.consume(marker, sizeof(marker));
    session.finish();
    TEST_ASSERT(session.state() == RebuildState::COMPLETE);
}
void order_is(const ItchBookRebuilder& session, std::uint16_t locate,
              std::uint64_t reference, std::int64_t price, std::uint64_t shares) {
    const auto* book = session.book(locate);
    TEST_ASSERT(book && book->check_invariants());
    const auto* order = book->find_order(reference);
    TEST_ASSERT(order && order->side == Side::SELL);
    TEST_ASSERT(order->price == Price(price) && order->remaining == Quantity(shares));
    TEST_ASSERT(session.check_invariants());
}
void totals_are(const TradeTotals& totals, std::uint64_t reports, std::uint64_t broken_count,
                std::uint64_t printable_count, std::uint64_t printable_volume) {
    TEST_ASSERT(totals.reports == reports && totals.broken_reports == broken_count);
    TEST_ASSERT(totals.printable_reports == printable_count && totals.printable_volume == printable_volume);
}
void same_totals(const TradeTotals& left, const TradeTotals& right) {
    totals_are(left, right.reports, right.broken_reports, right.printable_reports, right.printable_volume);
}
template<class Action>
void expect_stale(ItchBookRebuilder& session, Action&& action) {
    bool threw = false;
    try { action(); }
    catch (const std::exception&) { threw = true; }
    TEST_ASSERT(threw && session.state() == RebuildState::STALE && session.metrics().errors == 1);
    TEST_ASSERT(!session.book(1) && !session.book(2));
    TEST_ASSERT(!session.trade_ledger() && !session.temporal_metrics());
    const auto applied = session.metrics().processed_records;
    bool blocked = false;
    try { session.consume(nullptr, 0); }
    catch (const std::exception&) { blocked = true; }
    TEST_ASSERT(blocked && session.metrics().errors == 1 && session.metrics().processed_records == applied);
}

void test_legacy_profile_is_preserved() {
    ItchBookRebuilder session;
    TEST_ASSERT(!session.trade_ledger() && !session.temporal_metrics());
    start(session);
    send(session, execute(1, 100, 1, 501));
    send(session, execute(1, 100, 1, 501)); // Legacy book-only input does not correlate matches.
    send(session, broken(1, 999)); // Nor does it claim the referenced report was observed.
    send(session, directory(1, "AAPL", 0xffffffffffffULL));
    send(session, directory(1, "AAPL", 0));
    order_is(session, 1, 100, 100, 18);
    complete(session); // Missing intermediate system phases remain supported by default.
    TEST_ASSERT(!session.trade_ledger() && !session.temporal_metrics());
}

void test_correlated_reports_and_breaks_do_not_restore_book() {
    ItchBookRebuilder session(RebuildLimits{}, validation());
    start(session);
    TEST_ASSERT(session.trade_ledger() && session.temporal_metrics());
    totals_are(session.trade_ledger()->totals(), 0, 0, 0, 0);
    send(session, execute(1, 100, 3, 501));
    order_is(session, 1, 100, 100, 17);
    const auto* first = session.trade_ledger()->find(501);
    TEST_ASSERT(first && first->kind == ReportKind::DISPLAYED_EXECUTION && first->price == 100);
    TEST_ASSERT(first->shares == 3 && first->stock_locate == 1 && first->printable && !first->broken);
    send(session, priced_execute(1, 100, 2, 502, 105, 'N'));
    order_is(session, 1, 100, 100, 15);
    const auto* second = session.trade_ledger()->find(502);
    TEST_ASSERT(second && second->kind == ReportKind::PRICED_EXECUTION && second->price == 105);
    TEST_ASSERT(!second->printable && second->shares == 2);
    TEST_ASSERT(session.book(1)->ask_quantity(Price(105)) == 0);
    send(session, priced_execute(2, 200, 4, 503, 198, 'Y'));
    order_is(session, 2, 200, 200, 6);
    const auto aapl = session.book(1)->capture_state();
    const auto msft = session.book(2)->capture_state();
    send(session, hidden(1, "AAPL", 5, 504));
    send(session, cross(2, "MSFT", 6, 505));
    send(session, cross(1, "AAPL", 0, 0, 0));
    send(session, cross(1, "AAPL", 0, 901, 100));
    TEST_ASSERT(session.trade_ledger()->size() == 5 && !session.trade_ledger()->find(901));
    TEST_ASSERT(session.trade_ledger()->find(504)->kind == ReportKind::NON_DISPLAYED);
    TEST_ASSERT(session.trade_ledger()->find(505)->kind == ReportKind::CROSS);
    totals_are(session.trade_ledger()->totals(), 5, 0, 4, 18);
    TEST_ASSERT(session.book(1)->capture_state() == aapl && session.book(2)->capture_state() == msft);
    send(session, broken(1, 501));
    totals_are(session.trade_ledger()->totals(), 5, 1, 3, 15);
    send(session, broken(1, 502)); // A non-printable execution still has an irreversible break.
    totals_are(session.trade_ledger()->totals(), 5, 2, 3, 15);
    send(session, broken(1, 504));
    totals_are(session.trade_ledger()->totals(), 5, 3, 2, 10);
    send(session, broken(2, 503));
    totals_are(session.trade_ledger()->totals(), 5, 4, 1, 6);
    for (const auto match : {std::uint64_t{501}, std::uint64_t{502}, std::uint64_t{503}, std::uint64_t{504}}) {
        TEST_ASSERT(session.trade_ledger()->find(match)->broken);
    }
    TEST_ASSERT(!session.trade_ledger()->find(505)->broken);
    TEST_ASSERT(session.book(1)->capture_state() == aapl && session.book(2)->capture_state() == msft);
    TEST_ASSERT(session.active_order_count() == 2 && session.seen_order_reference_count() == 2);
    complete(session);
    TEST_ASSERT(session.temporal_metrics()->observations == session.metrics().processed_records);
    TEST_ASSERT(session.temporal_metrics()->regressions == 0);
}

void test_bad_correlations_fail_before_book_mutation() {
    const std::vector<Bytes> invalid{
        execute(1, 100, 1, 501), priced_execute(1, 100, 1, 501, 105, 'Y'),
        execute(2, 200, 1, 501), priced_execute(2, 200, 1, 501, 198, 'N'),
        hidden(1, "AAPL", 1, 501), cross(2, "MSFT", 1, 501),
        broken(1, 999), broken(2, 501),
        // A fresh match must not be registered when its order/quantity/price
        // cannot be applied. Validation has to precede ledger insertion too.
        execute(1, 999, 1, 502), execute(2, 100, 1, 502),
        execute(1, 100, 0, 502), execute(1, 100, 19, 502),
        priced_execute(1, 999, 1, 502, 105, 'Y'),
        priced_execute(1, 100, 0, 502, 105, 'Y'),
        priced_execute(1, 100, 19, 502, 105, 'N'),
        priced_execute(1, 100, 1, 502, 0, 'Y'),
        priced_execute(1, 100, 1, 502, 105, 'Z')
    };
    for (const auto& payload : invalid) {
        ItchBookRebuilder session(RebuildLimits{}, validation());
        start(session);
        send(session, execute(1, 100, 2, 501));
        const auto* diagnostic_aapl = session.book(1);
        const auto* diagnostic_msft = session.book(2);
        const auto* diagnostic_ledger = session.trade_ledger();
        const auto* diagnostic_clock = session.temporal_metrics();
        const auto before_aapl = diagnostic_aapl->capture_state();
        const auto before_msft = diagnostic_msft->capture_state();
        const auto before_totals = diagnostic_ledger->totals();
        const auto before_clock = *diagnostic_clock;
        const auto before_records = session.metrics().processed_records;
        expect_stale(session, [&] { send(session, payload); });
        TEST_ASSERT(diagnostic_aapl->capture_state() == before_aapl && diagnostic_msft->capture_state() == before_msft);
        same_totals(diagnostic_ledger->totals(), before_totals);
        TEST_ASSERT(diagnostic_ledger->size() == 1 && !diagnostic_ledger->find(501)->broken);
        TEST_ASSERT(diagnostic_clock->observations == before_clock.observations);
        TEST_ASSERT(diagnostic_clock->last_timestamp == before_clock.last_timestamp);
        TEST_ASSERT(session.metrics().processed_records == before_records && session.check_invariants());
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation());
        start(session);
        send(session, execute(1, 100, 2, 501));
        send(session, broken(1, 501));
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_ledger = session.trade_ledger();
        const auto before = diagnostic_book->capture_state();
        const auto totals = diagnostic_ledger->totals();
        expect_stale(session, [&] { send(session, broken(1, 501)); });
        TEST_ASSERT(diagnostic_book->capture_state() == before);
        same_totals(diagnostic_ledger->totals(), totals);
        TEST_ASSERT(diagnostic_ledger->find(501)->broken);
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation());
        start(session);
        send(session, cross(2, "MSFT", 6, 505));
        const auto* diagnostic_book = session.book(2);
        const auto* diagnostic_ledger = session.trade_ledger();
        const auto before = diagnostic_book->capture_state();
        expect_stale(session, [&] { send(session, broken(2, 505)); });
        TEST_ASSERT(diagnostic_book->capture_state() == before && !diagnostic_ledger->find(505)->broken);
        totals_are(diagnostic_ledger->totals(), 1, 0, 1, 6);
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation());
        start(session);
        send(session, cross(1, "AAPL", 0, 0, 0));
        TEST_ASSERT(session.trade_ledger()->size() == 0);
        expect_stale(session, [&] { send(session, broken(1, 0)); });
    }
}

void test_report_capacity_keeps_broken_tombstones() {
    RebuildLimits limits;
    limits.maximum_trade_reports = 1;
    for (const bool break_first : {false, true}) {
        ItchBookRebuilder session(limits, validation());
        start(session);
        send(session, execute(1, 100, 2, 501));
        if (break_first) send(session, broken(1, 501));
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_ledger = session.trade_ledger();
        const auto before = diagnostic_book->capture_state();
        const auto totals = diagnostic_ledger->totals();
        expect_stale(session, [&] { send(session, execute(1, 100, 1, 502)); });
        TEST_ASSERT(diagnostic_book->capture_state() == before && diagnostic_ledger->size() == 1);
        TEST_ASSERT(!diagnostic_ledger->find(502) && diagnostic_ledger->find(501)->broken == break_first);
        same_totals(diagnostic_ledger->totals(), totals);
    }
    limits.maximum_trade_reports = 0;
    bool rejected = false;
    try { ItchBookRebuilder invalid(limits, validation()); }
    catch (const std::invalid_argument&) { rejected = true; }
    TEST_ASSERT(rejected);
}

void test_timestamp_profile_and_transactional_observations() {
    {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, true));
        start(session);
        TEST_ASSERT(session.temporal_metrics()->observations == 5);
        send(session, directory(1, "AAPL", 8));
        send(session, directory(1, "AAPL", 6));
        send(session, directory(1, "AAPL", 6));
        send(session, directory(1, "AAPL", 7));
        TEST_ASSERT(session.temporal_metrics()->observations == 9);
        TEST_ASSERT(session.temporal_metrics()->regressions == 1);
        TEST_ASSERT(session.temporal_metrics()->last_timestamp == std::uint64_t{7});
        TEST_ASSERT(!session.trade_ledger());
        complete(session, NANOSECONDS_PER_DAY - 1);
        TEST_ASSERT(session.temporal_metrics()->last_timestamp == NANOSECONDS_PER_DAY - 1);
    }
    for (const auto timestamp : {NANOSECONDS_PER_DAY, NANOSECONDS_PER_DAY + 1, std::uint64_t{0xffffffffffffULL}}) {
        ItchBookRebuilder session(RebuildLimits{}, validation());
        start(session);
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_ledger = session.trade_ledger();
        const auto* diagnostic_clock = session.temporal_metrics();
        const auto before = diagnostic_book->capture_state();
        const auto observed = diagnostic_clock->observations;
        expect_stale(session, [&] { send(session, execute(1, 100, 1, 501, timestamp)); });
        TEST_ASSERT(diagnostic_book->capture_state() == before && diagnostic_ledger->size() == 0);
        TEST_ASSERT(diagnostic_clock->observations == observed && diagnostic_clock->last_timestamp == std::uint64_t{7});
    }
    {
        auto options = validation();
        options.timestamp_regression_policy = TimestampRegressionPolicy::REJECT;
        ItchBookRebuilder session(RebuildLimits{}, options);
        start(session);
        send(session, directory(1, "AAPL", 7)); // Ties are valid even under REJECT.
        const auto* diagnostic_book = session.book(1);
        const auto* diagnostic_clock = session.temporal_metrics();
        const auto before = diagnostic_book->capture_state();
        const auto observed = diagnostic_clock->observations;
        expect_stale(session, [&] { send(session, execute(1, 100, 1, 501, 6)); });
        TEST_ASSERT(diagnostic_book->capture_state() == before);
        TEST_ASSERT(diagnostic_clock->observations == observed && diagnostic_clock->regressions == 0);
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, true));
        start(session, 0);
        complete(session, 0);
        TEST_ASSERT(session.temporal_metrics()->last_timestamp == std::uint64_t{0});
    }
}

void test_complete_system_event_profile_is_opt_in() {
    {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, false, true));
        start(session);
        for (const char code : {'S', 'Q', 'M', 'E'}) send(session, system_event(code));
        send(session, erase(1, 100)); // System E is not end-of-messages.
        complete(session);
        TEST_ASSERT(!session.trade_ledger() && !session.temporal_metrics());
    }
    for (const char skipped : {'C', 'Q', 'M', 'E'}) {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, false, true));
        start(session);
        expect_stale(session, [&] { send(session, system_event(skipped)); });
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, false, true));
        start(session);
        send(session, system_event('S'));
        expect_stale(session, [&] { send(session, system_event('M')); });
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, validation(false, false, true));
        start(session);
        for (const char code : {'S', 'Q', 'M', 'E'}) send(session, system_event(code));
        const std::uint8_t marker[]{0, 0};
        session.consume(marker, sizeof(marker));
        expect_stale(session, [&] { session.finish(); }); // All earlier phases do not replace S/C.
    }
}

Bytes strict_wire() {
    const std::vector<Bytes> records{
        system_event('O'), directory(1, "AAPL"), directory(2, "MSFT"),
        add(1, 100, "AAPL", 100, 20), add(2, 200, "MSFT", 200, 10),
        system_event('S'), system_event('Q'), execute(1, 100, 3, 501),
        priced_execute(1, 100, 2, 502, 105, 'N'), hidden(1, "AAPL", 4, 503),
        cross(2, "MSFT", 5, 504), cross(1, "AAPL", 0, 0, 0),
        system_event('M'), system_event('E'), broken(1, 503), erase(1, 100), system_event('C')
    };
    Bytes wire;
    for (const auto& record : records) append_frame(wire, record);
    wire.push_back(0);
    wire.push_back(0);
    return wire;
}
void check_strict_replay(const ItchBookRebuilder& session) {
    TEST_ASSERT(session.state() == RebuildState::COMPLETE && session.check_invariants());
    TEST_ASSERT(session.book(1)->order_count() == 0 && session.book(2)->order_count() == 1);
    order_is(session, 2, 200, 200, 10);
    TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 2);
    TEST_ASSERT(session.trade_ledger() && session.trade_ledger()->size() == 4);
    totals_are(session.trade_ledger()->totals(), 4, 1, 2, 8);
    TEST_ASSERT(session.trade_ledger()->find(503)->broken);
    TEST_ASSERT(session.trade_ledger()->find(502)->price == 105 && !session.trade_ledger()->find(502)->printable);
    TEST_ASSERT(session.temporal_metrics() && session.temporal_metrics()->observations == session.metrics().processed_records);
    TEST_ASSERT(session.temporal_metrics()->regressions == 0 && session.temporal_metrics()->last_timestamp == std::uint64_t{7});
}
void test_strict_streaming_and_finish() {
    const auto wire = strict_wire();
    const auto options = validation(true, true, true);
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        ItchBookRebuilder session(RebuildLimits{}, options);
        session.consume(wire.data(), split);
        session.consume(wire.data() + split, wire.size() - split);
        session.finish();
        check_strict_replay(session);
        session.finish(); // Sequential repeated finish does not count new timestamps/reports.
        check_strict_replay(session);
    }
    ItchBookRebuilder bytewise(RebuildLimits{}, options);
    for (const auto byte : wire) bytewise.consume(&byte, 1);
    bytewise.finish();
    check_strict_replay(bytewise);
    for (const auto mask : {std::ios::goodbit, std::ios::failbit | std::ios::eofbit | std::ios::badbit}) {
        std::istringstream input(std::string(wire.begin(), wire.end()));
        input.exceptions(mask);
        ItchBookRebuilder session(RebuildLimits{}, options);
        session.read(input);
        check_strict_replay(session);
    }
    auto truncated = wire;
    truncated.pop_back();
    std::istringstream input(std::string(truncated.begin(), truncated.end()));
    ItchBookRebuilder failed(RebuildLimits{}, options);
    expect_stale(failed, [&] { failed.read(input); });
}
} // namespace

int main() {
    try {
        test_legacy_profile_is_preserved();
        test_correlated_reports_and_breaks_do_not_restore_book();
        test_bad_correlations_fail_before_book_mutation();
        test_report_capacity_keeps_broken_tombstones();
        test_timestamp_profile_and_transactional_observations();
        test_complete_system_event_profile_is_opt_in();
        test_strict_streaming_and_finish();
        std::cout << "ITCH report/session validation tests passed (synthetic bytes only; seven groups).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH report/session test failed: " << error.what() << '\n';
        return 1;
    }
}
