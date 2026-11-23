#include "itch_book_rebuilder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <streambuf>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;

// Entirely invented messages, not extracted from a market-data file. These
// helpers express the audited wire fields, not a production paired encoder.
void number(Bytes& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = width; i != 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}
void stock(Bytes& bytes, std::string_view name) {
    TEST_ASSERT(name.size() <= 8);
    for (char letter : name) bytes.push_back(static_cast<std::uint8_t>(letter));
    bytes.insert(bytes.end(), 8 - name.size(), std::uint8_t{' '});
}
Bytes message(char type, std::uint16_t locate, const Bytes& body) {
    Bytes result{static_cast<std::uint8_t>(type)};
    number(result, locate, 2);
    number(result, 0x3210, 2); // Tracking number, not a sequence.
    number(result, 7, 6); // Deliberately tied timestamps across records.
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
Bytes system_event(char code, std::uint16_t locate = 0) {
    return message('S', locate, Bytes{static_cast<std::uint8_t>(code)});
}
Bytes directory(std::uint16_t locate, std::string_view name) {
    Bytes body;
    stock(body, name);
    body.insert(body.end(), {'Q', 'N'});
    number(body, 100, 4);
    body.insert(body.end(), {'N', 'C', ' ', ' ', 'P', 'N', 'N', '1', 'N'});
    number(body, 0, 4);
    body.push_back('N');
    return message('R', locate, body);
}
Bytes add(std::uint16_t locate, std::uint64_t reference, char side,
          std::uint32_t quantity, std::string_view name, std::uint32_t price,
          bool attributed = false) {
    Bytes body;
    number(body, reference, 8);
    body.push_back(static_cast<std::uint8_t>(side));
    number(body, quantity, 4);
    stock(body, name);
    number(body, price, 4);
    if (attributed) body.insert(body.end(), {'A', 'B', 'C', 'D'});
    return message(attributed ? 'F' : 'A', locate, body);
}
Bytes executed(std::uint16_t locate, std::uint64_t reference,
               std::uint32_t quantity, std::uint64_t match = 900) {
    Bytes body;
    number(body, reference, 8);
    number(body, quantity, 4);
    number(body, match, 8);
    return message('E', locate, body);
}
Bytes priced_execution(std::uint16_t locate, std::uint64_t reference,
                      std::uint32_t quantity, std::uint32_t price,
                      char printable = 'N', std::uint64_t match = 901) {
    auto result = executed(locate, reference, quantity, match);
    result[0] = 'C';
    result.push_back(static_cast<std::uint8_t>(printable));
    number(result, price, 4);
    return result;
}
Bytes cancel(std::uint16_t locate, std::uint64_t reference, std::uint32_t quantity) {
    Bytes body;
    number(body, reference, 8);
    number(body, quantity, 4);
    return message('X', locate, body);
}
Bytes erase(std::uint16_t locate, std::uint64_t reference) {
    Bytes body;
    number(body, reference, 8);
    return message('D', locate, body);
}
Bytes replace(std::uint16_t locate, std::uint64_t old_reference,
              std::uint64_t new_reference, std::uint32_t quantity, std::uint32_t price) {
    Bytes body;
    number(body, old_reference, 8);
    number(body, new_reference, 8);
    number(body, quantity, 4);
    number(body, price, 4);
    return message('U', locate, body);
}
Bytes trading(std::uint16_t locate, std::string_view name, char state) {
    Bytes body;
    stock(body, name);
    body.insert(body.end(), {static_cast<std::uint8_t>(state), ' ', 'T', '1', ' ', ' '});
    return message('H', locate, body);
}
Bytes hidden_trade(std::uint16_t locate, std::string_view name,
                   std::uint64_t reference = 0, char side = 'B',
                   std::uint32_t quantity = 10, std::uint32_t price = 103,
                   std::uint64_t match = 902) {
    Bytes body;
    number(body, reference, 8);
    body.push_back(static_cast<std::uint8_t>(side));
    number(body, quantity, 4);
    stock(body, name);
    number(body, price, 4);
    number(body, match, 8);
    return message('P', locate, body);
}
Bytes cross(std::uint16_t locate, std::string_view name,
            std::uint64_t quantity = 100, std::uint32_t price = 104,
            std::uint64_t match = 903, char type = 'O') {
    Bytes body;
    number(body, quantity, 8);
    stock(body, name);
    number(body, price, 4);
    number(body, match, 8);
    body.push_back(static_cast<std::uint8_t>(type));
    return message('Q', locate, body);
}
Bytes broken(std::uint16_t locate, std::uint64_t match = 902) {
    Bytes body;
    number(body, match, 8);
    return message('B', locate, body);
}
void append_frame(Bytes& wire, const Bytes& payload) {
    TEST_ASSERT(!payload.empty() && payload.size() <= 65535);
    number(wire, payload.size(), 2);
    wire.insert(wire.end(), payload.begin(), payload.end());
}
Bytes framed(const Bytes& payload) {
    Bytes wire;
    append_frame(wire, payload);
    return wire;
}
void send(ItchBookRebuilder& session, const Bytes& payload) {
    const auto wire = framed(payload);
    session.consume(wire.data(), wire.size());
}
void start(ItchBookRebuilder& session, bool initial_order = false) {
    send(session, system_event('O'));
    send(session, directory(1, "AAPL"));
    send(session, directory(2, "MSFT"));
    if (initial_order) send(session, add(1, 100, 'B', 12, "AAPL", 100));
}
void complete(ItchBookRebuilder& session) {
    send(session, system_event('C'));
    const std::uint8_t marker[]{0, 0};
    session.consume(marker, sizeof(marker));
    session.finish();
    TEST_ASSERT(session.state() == RebuildState::COMPLETE);
}
void order_is(const ItchBookRebuilder& session, std::uint16_t locate,
              std::uint64_t id, std::int64_t price, std::uint64_t quantity, Side side) {
    const auto* book = session.book(locate);
    TEST_ASSERT(book != nullptr && book->check_invariants());
    const auto* order = book->find_order(id);
    TEST_ASSERT(order != nullptr);
    TEST_ASSERT(order->price == Price(price) && order->remaining == Quantity(quantity));
    TEST_ASSERT(order->side == side);
    TEST_ASSERT(session.check_invariants());
}
template<typename Action>
void expect_failure(ItchBookRebuilder& session, Action&& action) {
    bool threw = false;
    try { action(); }
    catch (const std::exception&) { threw = true; }
    TEST_ASSERT(threw);
    TEST_ASSERT(session.state() == RebuildState::STALE);
    TEST_ASSERT(session.metrics().errors == 1);
    TEST_ASSERT(session.book(1) == nullptr && session.book(2) == nullptr);
    const auto records = session.metrics().processed_records;
    bool terminal = false;
    try { session.finish(); }
    catch (const std::exception&) { terminal = true; }
    TEST_ASSERT(terminal && session.metrics().errors == 1);
    TEST_ASSERT(session.metrics().processed_records == records);
}

void test_interleaved_golden_lifecycle() {
    static_assert(!std::is_copy_constructible_v<ItchBookRebuilder>);
    static_assert(!std::is_move_constructible_v<ItchBookRebuilder>);
    ItchBookRebuilder session;
    TEST_ASSERT(session.state() == RebuildState::AWAITING_START);
    TEST_ASSERT(!session.book(1) && !session.locate("AAPL"));
    start(session);
    send(session, directory(3, "BRK.B"));
    TEST_ASSERT(session.state() == RebuildState::REBUILDING);
    TEST_ASSERT(session.instrument_count() == 3);
    TEST_ASSERT(session.locate("AAPL") == 1 && session.locate("MSFT") == 2);
    TEST_ASSERT(session.locate("BRK.B") == 3 && !session.locate("UNKNOWN"));
    TEST_ASSERT(session.symbol(1) == "AAPL" && session.symbol(3) == "BRK.B");
    TEST_ASSERT(session.symbol(0).empty() && session.symbol(60000).empty());
    send(session, directory(1, "AAPL")); // Exactly repeated metadata is harmless.
    send(session, add(1, 100, 'B', 12, "AAPL", 100));
    send(session, add(2, 3, 'S', 9, "MSFT", 200, true)); // IDs need not increase.
    send(session, add(1, 77, 'S', 4, "AAPL", 110));
    send(session, add(3, 90, 'B', 1, "BRK.B", 300));
    order_is(session, 1, 100, 100, 12, Side::BUY);
    order_is(session, 2, 3, 200, 9, Side::SELL);
    TEST_ASSERT(session.active_order_count() == 4 && session.seen_order_reference_count() == 4);
    TEST_ASSERT(session.book(1)->best_bid() == Price(100));
    TEST_ASSERT(session.book(1)->best_ask() == Price(110));
    send(session, executed(1, 100, 2));
    order_is(session, 1, 100, 100, 10, Side::BUY);
    send(session, priced_execution(1, 100, 3, 105, 'N'));
    order_is(session, 1, 100, 100, 7, Side::BUY);
    TEST_ASSERT(session.book(1)->bid_quantity(Price(105)) == 0);
    send(session, cancel(1, 100, 2));
    order_is(session, 1, 100, 100, 5, Side::BUY);
    send(session, replace(1, 100, 40, 9, 101));
    order_is(session, 1, 40, 101, 9, Side::BUY); // New TOTAL, not +/-9.
    TEST_ASSERT(!session.book(1)->find_order(100));
    TEST_ASSERT(session.book(1)->bid_quantity(Price(100)) == 0);
    send(session, replace(2, 3, 2, 4, 201));
    order_is(session, 2, 2, 201, 4, Side::SELL);
    TEST_ASSERT(!session.attribution(3));
    TEST_ASSERT(session.attribution(2).has_value());
    TEST_ASSERT(text_view(*session.attribution(2)) == "ABCD");
    TEST_ASSERT(session.active_order_count() == 4 && session.seen_order_reference_count() == 6);
    for (const char state : {'H', 'P', 'Q', 'T'}) {
        send(session, trading(2, "MSFT", state));
        TEST_ASSERT(session.trading_action(2) != nullptr);
        TEST_ASSERT(session.trading_action(2)->trading_state == state);
        order_is(session, 2, 2, 201, 4, Side::SELL);
    }
    send(session, cancel(2, 2, 1));
    order_is(session, 2, 2, 201, 3, Side::SELL);
    const auto aapl_before = session.book(1)->capture_state();
    const auto msft_before = session.book(2)->capture_state();
    send(session, hidden_trade(1, "AAPL"));
    send(session, cross(1, "AAPL"));
    send(session, cross(1, "AAPL", 0, 0, 0)); // A zero-share cross is a valid report.
    send(session, broken(1));
    TEST_ASSERT(session.book(1)->capture_state() == aapl_before);
    TEST_ASSERT(session.book(2)->capture_state() == msft_before);
    TEST_ASSERT(!session.book(1)->find_order(0));
    send(session, system_event('S'));
    send(session, system_event('Q'));
    send(session, system_event('M'));
    send(session, system_event('E')); // System-hours end is not messages end.
    send(session, erase(1, 40));
    send(session, broken(1, 904));
    send(session, executed(2, 2, 3));
    send(session, erase(1, 77));
    TEST_ASSERT(session.book(1)->order_count() == 0 && session.book(2)->order_count() == 0);
    order_is(session, 3, 90, 300, 1, Side::BUY);
    complete(session);
    TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 6);
    TEST_ASSERT(session.metrics().errors == 0);
    TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>('F')] == 1);
    TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>('C')] == 1);
    TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>('P')] == 1);
    TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>('Q')] == 2);
    TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>('B')] == 2);
    std::uint64_t sum = 0;
    for (const auto count : session.metrics().records_by_type) sum += count;
    TEST_ASSERT(sum == session.metrics().processed_records);
}

void test_routing_and_reference_rejections() {
    const std::vector<Bytes> invalid{
        add(3, 200, 'B', 1, "UNKNOWN", 100),
        add(0, 200, 'B', 1, "AAPL", 100),
        add(1, 200, 'B', 1, "MSFT", 100),
        add(2, 100, 'S', 1, "MSFT", 200), // Globally active reference collision.
        executed(2, 100, 1), priced_execution(2, 100, 1, 123),
        cancel(2, 100, 1), erase(2, 100), replace(2, 100, 200, 1, 200),
        executed(1, 999, 1), cancel(1, 999, 1), erase(1, 999), replace(1, 999, 200, 1, 100),
        replace(1, 100, 100, 1, 100), replace(1, 100, 0, 1, 100),
        directory(1, "NVDA"), directory(3, "AAPL"), directory(0, "NVDA"),
        directory(3, ""), directory(3, "BAD NAME"), directory(3, " BAD"),
        trading(1, "MSFT", 'T'), trading(1, "AAPL", 'X'),
        hidden_trade(1, "MSFT"), cross(1, "MSFT"), broken(0), broken(3)
    };
    for (const auto& bad : invalid) {
        ItchBookRebuilder session;
        start(session, true);
        const auto before = session.metrics().processed_records;
        expect_failure(session, [&] { send(session, bad); });
        TEST_ASSERT(session.metrics().processed_records == before);
    }
    {
        ItchBookRebuilder session;
        start(session, true);
        send(session, erase(1, 100));
        expect_failure(session, [&] { send(session, add(2, 100, 'S', 1, "MSFT", 200)); });
    }
    {
        ItchBookRebuilder session;
        start(session, true);
        send(session, cancel(1, 100, 12));
        TEST_ASSERT(session.book(1)->order_count() == 0 && session.active_order_count() == 0);
        TEST_ASSERT(session.seen_order_reference_count() == 1 && session.check_invariants());
        expect_failure(session, [&] { send(session, add(1, 100, 'B', 1, "AAPL", 100)); });
    }
    {
        ItchBookRebuilder session;
        start(session, true);
        send(session, replace(1, 100, 200, 1, 101));
        expect_failure(session, [&] { send(session, replace(1, 200, 100, 1, 102)); });
    }
    {
        ItchBookRebuilder session;
        start(session, true);
        send(session, add(2, 200, 'S', 1, "MSFT", 200));
        expect_failure(session, [&] { send(session, replace(1, 100, 200, 1, 101)); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        auto changed = directory(1, "AAPL");
        changed[20] = 'D';
        expect_failure(session, [&] { send(session, changed); });
    }
    for (const auto bad_character : {std::uint8_t{0}, std::uint8_t{127}, std::uint8_t{255}}) {
        ItchBookRebuilder session;
        send(session, system_event('O'));
        auto invalid_symbol = directory(1, "AAPL");
        invalid_symbol[12] = bad_character;
        expect_failure(session, [&] { send(session, invalid_symbol); });
    }
}

void test_numeric_and_trade_profile_rejections() {
    const std::vector<Bytes> invalid{
        add(1, 0, 'B', 1, "AAPL", 100), add(1, 200, 'B', 0, "AAPL", 100),
        add(1, 200, 'B', 1, "AAPL", 0), add(1, 200, 'X', 1, "AAPL", 100),
        add(1, 200, 'B', 1, "AAPL", 2000000001U),
        executed(1, 100, 0), executed(1, 100, 13), executed(1, 100, 1, 0),
        priced_execution(1, 100, 1, 0), priced_execution(1, 100, 1, 105, 'X'),
        priced_execution(1, 100, 13, 105), cancel(1, 100, 0), cancel(1, 100, 13),
        replace(1, 100, 200, 0, 100), replace(1, 100, 200, 1, 0),
        hidden_trade(1, "AAPL", 100), hidden_trade(1, "AAPL", 0, 'S'),
        hidden_trade(1, "AAPL", 0, 'B', 0), hidden_trade(1, "AAPL", 0, 'B', 1, 0),
        hidden_trade(1, "AAPL", 0, 'B', 1, 100, 0),
        cross(1, "AAPL", 1, 0), cross(1, "AAPL", 1, 100, 0),
        cross(1, "AAPL", 1, 100, 900, 'Z'), broken(1, 0)
    };
    for (const auto& bad : invalid) {
        ItchBookRebuilder session;
        start(session, true);
        expect_failure(session, [&] { send(session, bad); });
    }
    // Full-width structural integers that are valid business values survive.
    ItchBookRebuilder session;
    start(session);
    send(session, add(1, std::numeric_limits<std::uint64_t>::max(), 'S',
                      std::numeric_limits<std::uint32_t>::max(), "AAPL",
                      2000000000U));
    order_is(session, 1, std::numeric_limits<std::uint64_t>::max(),
             2000000000,
             std::numeric_limits<std::uint32_t>::max(), Side::SELL);
    send(session, priced_execution(1, std::numeric_limits<std::uint64_t>::max(),
                                   std::numeric_limits<std::uint32_t>::max(), 1, 'Y'));
    TEST_ASSERT(session.active_order_count() == 0);
    complete(session);
}

void test_limits_and_retired_history() {
    const std::array<RebuildLimits, 4> invalid{{
        RebuildLimits{0, 1, 1}, RebuildLimits{1, 0, 1},
        RebuildLimits{1, 1, 0}, RebuildLimits{65536, 1, 1}
    }};
    for (const auto& limits : invalid) {
        bool threw = false;
        try { ItchBookRebuilder session(limits); }
        catch (const std::invalid_argument&) { threw = true; }
        TEST_ASSERT(threw);
    }
    {
        ItchBookRebuilder session(RebuildLimits{1, 2, 3});
        send(session, system_event('O'));
        send(session, directory(1, "AAPL"));
        expect_failure(session, [&] { send(session, directory(2, "MSFT")); });
        TEST_ASSERT(session.instrument_count() == 1);
    }
    {
        ItchBookRebuilder session(RebuildLimits{2, 1, 3});
        start(session, true);
        send(session, replace(1, 100, 200, 10, 101)); // Does not consume a new active slot.
        order_is(session, 1, 200, 101, 10, Side::BUY);
        expect_failure(session, [&] { send(session, add(2, 300, 'S', 1, "MSFT", 200)); });
        TEST_ASSERT(session.active_order_count() == 1);
    }
    {
        ItchBookRebuilder session(RebuildLimits{2, 2, 2});
        start(session, true);
        send(session, erase(1, 100));
        send(session, add(1, 200, 'B', 1, "AAPL", 101));
        send(session, erase(1, 200));
        TEST_ASSERT(session.active_order_count() == 0 && session.seen_order_reference_count() == 2);
        expect_failure(session, [&] { send(session, add(2, 300, 'S', 1, "MSFT", 200)); });
        TEST_ASSERT(session.seen_order_reference_count() == 2);
    }
    {
        ItchBookRebuilder session(RebuildLimits{2, 1, 2});
        start(session, true);
        send(session, replace(1, 100, 200, 1, 101));
        expect_failure(session, [&] { send(session, replace(1, 200, 300, 1, 102)); });
        TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 2);
    }
}

void test_system_and_completion_gates() {
    for (const auto& first : {directory(1, "AAPL"), system_event('C'), system_event('O', 1)}) {
        ItchBookRebuilder session;
        expect_failure(session, [&] { send(session, first); });
    }
    for (const auto& bad : {system_event('O'), system_event('X'), system_event('S', 1)}) {
        ItchBookRebuilder session;
        start(session);
        expect_failure(session, [&] { send(session, bad); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        send(session, system_event('Q'));
        expect_failure(session, [&] { send(session, system_event('S')); });
    }
    {
        ItchBookRebuilder session;
        start(session, true);
        send(session, system_event('C'));
        expect_failure(session, [&] { send(session, erase(1, 100)); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        send(session, system_event('C'));
        expect_failure(session, [&] { session.finish(); }); // S/C is not BinaryFILE EOF marker.
    }
    {
        ItchBookRebuilder session;
        start(session);
        const std::uint8_t marker[]{0, 0};
        session.consume(marker, sizeof(marker));
        expect_failure(session, [&] { session.finish(); }); // Framing alone is insufficient.
    }
    {
        ItchBookRebuilder session;
        const std::uint8_t marker[]{0, 0};
        session.consume(marker, sizeof(marker));
        expect_failure(session, [&] { session.finish(); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        complete(session);
        session.finish(); // A repeated validation does not rebuild or reset.
        TEST_ASSERT(session.state() == RebuildState::COMPLETE && session.metrics().errors == 0);
        const std::uint8_t byte = 1;
        expect_failure(session, [&] { session.consume(&byte, 1); });
    }
}

std::vector<Bytes> replay_records() {
    return {system_event('O'), directory(1, "AAPL"), directory(2, "MSFT"),
        add(1, 100, 'B', 12, "AAPL", 100), add(2, 3, 'S', 9, "MSFT", 200, true),
        executed(1, 100, 2), priced_execution(1, 100, 3, 105), cancel(1, 100, 2),
        replace(1, 100, 40, 9, 101), replace(2, 3, 2, 4, 201),
        trading(2, "MSFT", 'Q'), hidden_trade(1, "AAPL"), cross(1, "AAPL"),
        system_event('E'), broken(1), erase(1, 40), system_event('C')};
}
Bytes replay_wire() {
    Bytes wire;
    for (const auto& record : replay_records()) append_frame(wire, record);
    wire.insert(wire.end(), {0, 0});
    return wire;
}
void check_replay(const ItchBookRebuilder& session) {
    TEST_ASSERT(session.state() == RebuildState::COMPLETE && session.metrics().errors == 0);
    TEST_ASSERT(session.book(1)->order_count() == 0 && session.book(1)->check_invariants());
    order_is(session, 2, 2, 201, 4, Side::SELL);
    TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 4);
    TEST_ASSERT(session.check_invariants());
    TEST_ASSERT(session.metrics().processed_records == replay_records().size());
    TEST_ASSERT(session.book(2)->best_ask() == Price(201) && !session.book(2)->best_bid());
}
void test_all_splits_bytewise_and_istream() {
    const auto wire = replay_wire();
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        ItchBookRebuilder session;
        session.consume(wire.data(), split);
        session.consume(wire.data() + split, wire.size() - split);
        session.finish();
        check_replay(session);
    }
    ItchBookRebuilder bytewise;
    for (const auto byte : wire) bytewise.consume(&byte, 1);
    bytewise.finish();
    check_replay(bytewise);
    for (const auto mask : {std::ios::goodbit, std::ios::failbit | std::ios::eofbit | std::ios::badbit}) {
        std::istringstream input(std::string(wire.begin(), wire.end()));
        input.exceptions(mask);
        ItchBookRebuilder session;
        session.read(input);
        check_replay(session);
    }
}

void test_framing_coverage_and_error_propagation() {
    {
        ItchBookRebuilder session;
        expect_failure(session, [&] { session.consume(nullptr, 1); });
    }
    {
        ItchBookRebuilder session;
        expect_failure(session, [&] { session.finish(); });
    }
    {
        ItchBookRebuilder session;
        const std::uint8_t prefix[]{0};
        session.consume(prefix, 1);
        expect_failure(session, [&] { session.finish(); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        const auto frame = framed(add(1, 100, 'B', 1, "AAPL", 100));
        session.consume(frame.data(), frame.size() - 1);
        expect_failure(session, [&] { session.finish(); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        auto shortened = add(1, 100, 'B', 1, "AAPL", 100);
        shortened.pop_back();
        expect_failure(session, [&] { send(session, shortened); });
    }
    for (const auto& unknown : {message('T', 1, Bytes(9, 0)), message('Z', 1, Bytes(2, 0))}) {
        ItchBookRebuilder session;
        start(session, true);
        expect_failure(session, [&] { send(session, unknown); });
    }
    for (const bool via_stream : {false, true}) {
        auto wire = replay_wire();
        wire.push_back(1); // Even after S/C and marker, trailing input is failure.
        ItchBookRebuilder session;
        expect_failure(session, [&] {
            if (via_stream) {
                std::istringstream input(std::string(wire.begin(), wire.end()));
                session.read(input);
            } else session.consume(wire.data(), wire.size());
        });
    }
    for (const std::size_t chop : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
        auto wire = replay_wire();
        wire.resize(wire.size() - chop);
        std::istringstream input(std::string(wire.begin(), wire.end()));
        ItchBookRebuilder session;
        expect_failure(session, [&] { session.read(input); });
    }
    {
        std::istringstream input;
        input.setstate(std::ios::badbit);
        ItchBookRebuilder session;
        expect_failure(session, [&] { session.read(input); });
    }
}

class ReentryBuffer : public std::streambuf {
    ItchBookRebuilder& session_;
    bool marker_;
    bool caught_ = false;
public:
    ReentryBuffer(ItchBookRebuilder& session, bool marker) : session_(session), marker_(marker) {}
    bool caught() const noexcept { return caught_; }
private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        try { session_.finish(); }
        catch (const std::exception&) { caught_ = true; }
        if (!marker_) return 0;
        TEST_ASSERT(requested >= 2);
        destination[0] = 0;
        destination[1] = 0;
        return 2;
    }
};
class ThrowingBuffer : public std::streambuf {
    std::streamsize xsgetn(char*, std::streamsize) override {
        throw std::runtime_error("synthetic device failure after framing marker");
    }
};
void test_stream_callbacks_cannot_revive_session() {
    for (const bool returns_marker : {false, true}) {
        for (const auto mask : {std::ios::goodbit, std::ios::badbit}) {
            ItchBookRebuilder session;
            const auto wire = replay_wire();
            session.consume(wire.data(), wire.size()); // Reader has marker, not yet finalized.
            TEST_ASSERT(session.state() == RebuildState::REBUILDING);
            ReentryBuffer buffer(session, returns_marker);
            std::istream input(&buffer);
            input.exceptions(mask);
            expect_failure(session, [&] { session.read(input); });
            TEST_ASSERT(buffer.caught());
        }
    }
    ItchBookRebuilder session;
    const auto wire = replay_wire();
    session.consume(wire.data(), wire.size());
    ThrowingBuffer buffer;
    std::istream input(&buffer);
    input.exceptions(std::ios::badbit);
    expect_failure(session, [&] { session.read(input); });
}

void test_tracking_and_time_do_not_define_order() {
    ItchBookRebuilder session;
    start(session);
    auto first = add(1, 100, 'B', 1, "AAPL", 100);
    first[3] = 0xff;
    first[4] = 0xff;
    std::fill(first.begin() + 5, first.begin() + 11, std::uint8_t{0xff});
    send(session, first);
    auto second = add(1, 5, 'B', 1, "AAPL", 101);
    std::fill(second.begin() + 3, second.begin() + 11, std::uint8_t{0});
    send(session, second); // Neither a tracking wrap nor a clock decrease is a sequence gap.
    order_is(session, 1, 100, 100, 1, Side::BUY);
    order_is(session, 1, 5, 101, 1, Side::BUY);
    complete(session);
}
} // namespace

int main() {
    try {
        test_interleaved_golden_lifecycle();
        test_routing_and_reference_rejections();
        test_numeric_and_trade_profile_rejections();
        test_limits_and_retired_history();
        test_system_and_completion_gates();
        test_all_splits_bytewise_and_istream();
        test_framing_coverage_and_error_propagation();
        test_stream_callbacks_cannot_revive_session();
        test_tracking_and_time_do_not_define_order();
        std::cout << "Native ITCH book reconstruction tests passed (synthetic bytes only).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native ITCH reconstruction test failed: " << error.what() << '\n';
        return 1;
    }
}
