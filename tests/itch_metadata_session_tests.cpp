#include "itch_book_rebuilder.hpp"

#include <algorithm>
#include <array>
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

// All bytes are invented here. This is not a copied exchange sample and these
// field helpers are not a production ITCH encoder used as the only oracle.
void number(Bytes& bytes, std::uint64_t value, std::size_t width) {
    for (std::size_t i = width; i != 0; --i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8)));
    }
}
void fixed(Bytes& bytes, std::string_view text, std::size_t width) {
    TEST_ASSERT(text.size() <= width);
    for (const char value : text) bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.insert(bytes.end(), width - text.size(), std::uint8_t{' '});
}
Bytes message(char type, std::uint16_t locate, const Bytes& body) {
    Bytes result{static_cast<std::uint8_t>(type)};
    number(result, locate, 2);
    number(result, 0x4321, 2); // Deliberately repeated tracking number, not a sequence.
    number(result, 1, 6);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
Bytes system_event(char code) {
    return message('S', 0, Bytes{static_cast<std::uint8_t>(code)});
}
Bytes directory(std::uint16_t locate, std::string_view name) {
    Bytes body;
    fixed(body, name, 8);
    body.insert(body.end(), {'Q', 'N'});
    number(body, 100, 4);
    body.insert(body.end(), {'N', 'C', ' ', ' ', 'P', 'N', 'N', '1', 'N'});
    number(body, 0, 4);
    body.push_back('N');
    return message('R', locate, body);
}
Bytes add(std::uint16_t locate, std::uint64_t reference, char side,
          std::uint32_t shares, std::string_view name, std::uint32_t price,
          bool attributed = false) {
    Bytes body;
    number(body, reference, 8);
    body.push_back(static_cast<std::uint8_t>(side));
    number(body, shares, 4);
    fixed(body, name, 8);
    number(body, price, 4);
    if (attributed) fixed(body, "ABCD", 4);
    return message(attributed ? 'F' : 'A', locate, body);
}
Bytes execute(std::uint16_t locate, std::uint64_t reference,
              std::uint32_t shares, std::uint64_t match) {
    Bytes body;
    number(body, reference, 8);
    number(body, shares, 4);
    number(body, match, 8);
    return message('E', locate, body);
}
Bytes priced_execute(std::uint16_t locate, std::uint64_t reference,
                     std::uint32_t shares, std::uint64_t match) {
    auto result = execute(locate, reference, shares, match);
    result[0] = 'C';
    result.push_back('N');
    number(result, 105, 4);
    return result;
}
Bytes cancel(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares) {
    Bytes body;
    number(body, reference, 8);
    number(body, shares, 4);
    return message('X', locate, body);
}
Bytes erase(std::uint16_t locate, std::uint64_t reference) {
    Bytes body;
    number(body, reference, 8);
    return message('D', locate, body);
}
Bytes replace(std::uint16_t locate, std::uint64_t old_reference,
              std::uint64_t new_reference, std::uint32_t shares, std::uint32_t price) {
    Bytes body;
    number(body, old_reference, 8);
    number(body, new_reference, 8);
    number(body, shares, 4);
    number(body, price, 4);
    return message('U', locate, body);
}
Bytes trading() {
    Bytes body;
    fixed(body, "MSFT", 8);
    body.insert(body.end(), {'Q', ' ', 'T', '1', ' ', ' '});
    return message('H', 2, body);
}
Bytes non_displayed() {
    auto result = add(1, 0, 'B', 1, "AAPL", 102);
    result[0] = 'P';
    number(result, 502, 8);
    return result;
}
Bytes cross() {
    Bytes body;
    number(body, 0, 8);
    fixed(body, "AAPL", 8);
    number(body, 0, 4);
    number(body, 0, 8);
    body.push_back('O');
    return message('Q', 1, body);
}
Bytes broken() {
    Bytes body;
    number(body, 502, 8);
    return message('B', 1, body);
}
Bytes reg_sho(std::uint16_t locate = 1, std::string_view name = "AAPL", char action = '1') {
    Bytes body;
    fixed(body, name, 8);
    body.push_back(static_cast<std::uint8_t>(action));
    return message('Y', locate, body);
}
Bytes participant(std::uint16_t locate = 1, std::string_view name = "AAPL",
                  std::string_view mpid = "ABCD", char primary = 'Y',
                  char mode = 'N', char state = 'A') {
    Bytes body;
    fixed(body, mpid, 4);
    fixed(body, name, 8);
    body.insert(body.end(), {static_cast<std::uint8_t>(primary),
        static_cast<std::uint8_t>(mode), static_cast<std::uint8_t>(state)});
    return message('L', locate, body);
}
Bytes decline_levels(std::uint16_t locate = 0, std::uint64_t first = 1,
                     std::uint64_t second = 2, std::uint64_t third = 3) {
    Bytes body;
    number(body, first, 8);
    number(body, second, 8);
    number(body, third, 8);
    return message('V', locate, body);
}
Bytes breach(std::uint16_t locate = 0, char level = '2') {
    return message('W', locate, Bytes{static_cast<std::uint8_t>(level)});
}
Bytes ipo(std::uint16_t locate = 0, std::string_view name = "AAPL",
          std::uint32_t release_time = 12345, char qualifier = 'A',
          std::uint32_t price = 99) {
    Bytes body;
    fixed(body, name, 8);
    number(body, release_time, 4);
    body.push_back(static_cast<std::uint8_t>(qualifier));
    number(body, price, 4);
    return message('K', locate, body);
}
Bytes collar(std::uint16_t locate = 1, std::string_view name = "AAPL",
             std::uint32_t reference = 100, std::uint32_t upper = 110,
             std::uint32_t lower = 90, std::uint32_t extension = 5) {
    Bytes body;
    fixed(body, name, 8);
    number(body, reference, 4);
    number(body, upper, 4);
    number(body, lower, 4);
    number(body, extension, 4);
    return message('J', locate, body);
}
Bytes halt(std::uint16_t locate = 1, std::string_view name = "AAPL",
           char market = 'Q', char action = 'H') {
    Bytes body;
    fixed(body, name, 8);
    body.insert(body.end(), {static_cast<std::uint8_t>(market), static_cast<std::uint8_t>(action)});
    return message('h', locate, body);
}
Bytes imbalance(std::uint16_t locate = 1, std::string_view name = "AAPL",
                std::uint64_t paired = 100, std::uint64_t unpaired = 5,
                char direction = 'B', std::uint32_t far = 95,
                std::uint32_t near = 99, std::uint32_t current = 100,
                char cross_type = 'O', char variation = 'L') {
    Bytes body;
    number(body, paired, 8);
    number(body, unpaired, 8);
    body.push_back(static_cast<std::uint8_t>(direction));
    fixed(body, name, 8);
    number(body, far, 4);
    number(body, near, 4);
    number(body, current, 4);
    body.insert(body.end(), {static_cast<std::uint8_t>(cross_type), static_cast<std::uint8_t>(variation)});
    return message('I', locate, body);
}
Bytes retail(std::uint16_t locate = 1, std::string_view name = "AAPL", char interest = 'A') {
    Bytes body;
    fixed(body, name, 8);
    body.push_back(static_cast<std::uint8_t>(interest));
    return message('N', locate, body);
}
void append_frame(Bytes& destination, const Bytes& payload) {
    TEST_ASSERT(!payload.empty() && payload.size() <= 65535);
    number(destination, payload.size(), 2);
    destination.insert(destination.end(), payload.begin(), payload.end());
}
void send(ItchBookRebuilder& session, const Bytes& payload) {
    Bytes frame;
    append_frame(frame, payload);
    session.consume(frame.data(), frame.size());
}
void start(ItchBookRebuilder& session, bool with_orders = false) {
    send(session, system_event('O'));
    send(session, directory(1, "AAPL"));
    send(session, directory(2, "MSFT"));
    if (with_orders) {
        send(session, add(1, 100, 'B', 10, "AAPL", 100));
        send(session, add(2, 200, 'S', 8, "MSFT", 200, true));
    }
}
void complete(ItchBookRebuilder& session) {
    send(session, system_event('C'));
    const std::uint8_t marker[]{0, 0};
    session.consume(marker, sizeof(marker));
    session.finish();
    TEST_ASSERT(session.state() == RebuildState::COMPLETE);
}
template<class Action>
void expect_failure(ItchBookRebuilder& session, Action&& action) {
    const auto records = session.metrics().processed_records;
    const auto counts = session.metrics().records_by_type;
    bool threw = false;
    try { action(); }
    catch (const std::exception&) { threw = true; }
    TEST_ASSERT(threw && session.state() == RebuildState::STALE);
    TEST_ASSERT(session.metrics().errors == 1 && session.metrics().processed_records == records);
    TEST_ASSERT(session.metrics().records_by_type == counts);
    TEST_ASSERT(!session.book(1) && !session.book(2));
    TEST_ASSERT(!session.metadata(0) && !session.metadata(1) && !session.metadata(2));
    TEST_ASSERT(!session.market_wide_metadata());
    bool terminal = false;
    try { session.finish(); }
    catch (const std::exception&) { terminal = true; }
    TEST_ASSERT(terminal && session.metrics().errors == 1);
}
FixedText<4> key(std::string_view name) {
    TEST_ASSERT(name.size() <= 4);
    FixedText<4> result{{' ', ' ', ' ', ' '}};
    std::copy(name.begin(), name.end(), result.begin());
    return result;
}
std::vector<Bytes> metadata_records() {
    return {reg_sho(), participant(), participant(1, "AAPL", "EFGH", 'N', 'P', 'E'),
        ipo(), collar(), halt(), halt(1, "AAPL", 'B', 'T'), halt(1, "AAPL", 'X', 'H'),
        imbalance(), retail()};
}

void check_golden_metadata(const ItchBookRebuilder& session) {
    const auto* metadata = session.metadata(1);
    TEST_ASSERT(metadata && !session.metadata(0) && !session.metadata(65535));
    TEST_ASSERT(metadata->reg_sho && metadata->reg_sho->action == '1');
    TEST_ASSERT(text_view(metadata->reg_sho->stock) == "AAPL");
    TEST_ASSERT(metadata->market_participants.size() == 2);
    const auto& first = metadata->market_participants.at(key("ABCD"));
    const auto& second = metadata->market_participants.at(key("EFGH"));
    TEST_ASSERT(text_view(first.stock) == "AAPL" && text_view(first.mpid) == "ABCD");
    TEST_ASSERT(first.primary_market_maker == 'Y' && first.market_maker_mode == 'N');
    TEST_ASSERT(first.market_participant_state == 'A');
    TEST_ASSERT(second.primary_market_maker == 'N' && second.market_maker_mode == 'P');
    TEST_ASSERT(second.market_participant_state == 'E');
    TEST_ASSERT(metadata->ipo && metadata->ipo->quotation_release_time == 12345);
    TEST_ASSERT(metadata->ipo->release_qualifier == 'A' && metadata->ipo->ipo_price == 99);
    TEST_ASSERT(metadata->luld && metadata->luld->reference_price == 100);
    TEST_ASSERT(metadata->luld->upper_price == 110 && metadata->luld->lower_price == 90);
    TEST_ASSERT(metadata->luld->extension == 5);
    for (std::size_t index = 0; index < 3; ++index) TEST_ASSERT(metadata->operational_halts[index]);
    TEST_ASSERT(metadata->operational_halts[0]->market_code == 'Q' && metadata->operational_halts[0]->action == 'H');
    TEST_ASSERT(metadata->operational_halts[1]->market_code == 'B' && metadata->operational_halts[1]->action == 'T');
    TEST_ASSERT(metadata->operational_halts[2]->market_code == 'X' && metadata->operational_halts[2]->action == 'H');
    TEST_ASSERT(metadata->imbalance && metadata->imbalance->paired_shares == 100);
    TEST_ASSERT(metadata->imbalance->imbalance_shares == 5 && metadata->imbalance->imbalance_direction == 'B');
    TEST_ASSERT(metadata->imbalance->far_price == 95 && metadata->imbalance->near_price == 99);
    TEST_ASSERT(metadata->imbalance->current_reference_price == 100);
    TEST_ASSERT(metadata->imbalance->cross_type == 'O' && metadata->imbalance->price_variation_indicator == 'L');
    TEST_ASSERT(metadata->retail && metadata->retail->interest == 'A');
    const auto* other = session.metadata(2);
    TEST_ASSERT(other && !other->reg_sho && other->market_participants.empty());
    TEST_ASSERT(!other->ipo && !other->luld && !other->imbalance && !other->retail);
    for (const auto& state : other->operational_halts) TEST_ASSERT(!state);
}
void test_metadata_does_not_mutate_visible_books() {
    ItchBookRebuilder session;
    TEST_ASSERT(session.market_wide_metadata() && !session.market_wide_metadata()->decline_levels);
    TEST_ASSERT(!session.market_wide_metadata()->status && !session.metadata(1));
    start(session, true);
    const auto aapl = session.book(1)->capture_state();
    const auto msft = session.book(2)->capture_state();
    const auto active = session.active_order_count();
    const auto seen = session.seen_order_reference_count();
    for (const auto& record : metadata_records()) {
        send(session, record);
        TEST_ASSERT(session.book(1)->capture_state() == aapl);
        TEST_ASSERT(session.book(2)->capture_state() == msft);
        TEST_ASSERT(session.active_order_count() == active && session.seen_order_reference_count() == seen);
        TEST_ASSERT(session.book(1)->best_bid() == Price(100));
        TEST_ASSERT(session.book(2)->best_ask() == Price(200) && session.check_invariants());
    }
    check_golden_metadata(session);
    // Changes update the current state, rather than accumulating stale copies.
    send(session, reg_sho(1, "AAPL", '2'));
    send(session, halt(1, "AAPL", 'Q', 'T'));
    send(session, retail(2, "MSFT", 'S'));
    TEST_ASSERT(session.metadata(1)->reg_sho->action == '2');
    TEST_ASSERT(session.metadata(1)->operational_halts[0]->action == 'T');
    TEST_ASSERT(session.metadata(1)->operational_halts[1]->action == 'T');
    TEST_ASSERT(session.metadata(1)->operational_halts[2]->action == 'H');
    TEST_ASSERT(session.metadata(2)->retail->interest == 'S');
    TEST_ASSERT(session.book(1)->capture_state() == aapl && session.book(2)->capture_state() == msft);
    complete(session);
}
void test_market_wide_and_special_zero_values() {
    ItchBookRebuilder session;
    send(session, system_event('O'));
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    send(session, decline_levels(0, maximum, 0, 1));
    send(session, breach(0, '3')); // Both global kinds are allowed before directories.
    TEST_ASSERT(session.instrument_count() == 0);
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level1 == maximum);
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level2 == 0);
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level3 == 1);
    TEST_ASSERT(session.market_wide_metadata()->status->breached_level == '3');
    send(session, directory(1, "AAPL"));
    send(session, ipo(0, "AAPL", 0, 'A', 0));
    TEST_ASSERT(session.metadata(1)->ipo && session.metadata(1)->ipo->ipo_price == 0);
    send(session, ipo(0, "AAPL", 0, 'C', 0));
    TEST_ASSERT(session.metadata(1)->ipo->release_qualifier == 'C');
    send(session, imbalance(1, "AAPL", 0, 0, 'O', 0, 0, 0, 'H', ' '));
    TEST_ASSERT(session.metadata(1)->imbalance->paired_shares == 0);
    TEST_ASSERT(session.metadata(1)->imbalance->current_reference_price == 0);
    send(session, imbalance(1, "AAPL", maximum, maximum, 'N', 2000000000U,
                            0, 1, 'C', 'C'));
    TEST_ASSERT(session.metadata(1)->imbalance->paired_shares == maximum);
    TEST_ASSERT(session.metadata(1)->imbalance->imbalance_shares == maximum);
    TEST_ASSERT(session.metadata(1)->imbalance->far_price == 2000000000U);
    send(session, collar(1, "AAPL", 0, 0, 0, std::numeric_limits<std::uint32_t>::max()));
    TEST_ASSERT(session.metadata(1)->luld->reference_price == 0);
    TEST_ASSERT(session.metadata(1)->luld->extension == std::numeric_limits<std::uint32_t>::max());
    send(session, collar(1, "AAPL", 100, 90, 110, 0));
    TEST_ASSERT(session.metadata(1)->luld->upper_price == 90);
    TEST_ASSERT(session.metadata(1)->luld->lower_price == 110); // No invented collar-order constraint.
    send(session, decline_levels(0, 0, maximum, 0));
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level1 == 0);
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level2 == maximum);
    TEST_ASSERT(session.book(1)->order_count() == 0 && session.check_invariants());
    complete(session);
}
void test_supported_historical_enum_updates() {
    ItchBookRebuilder session;
    start(session);
    for (const char action : {'0', '1', '2'}) {
        send(session, reg_sho(1, "AAPL", action));
        TEST_ASSERT(session.metadata(1)->reg_sho->action == action);
    }
    for (const char primary : {'Y', 'N'}) {
        for (const char mode : {'N', 'P', 'S', 'R', 'L'}) {
            for (const char state : {'A', 'E', 'W', 'S', 'D'}) {
                send(session, participant(1, "AAPL", "ABC", primary, mode, state));
                const auto& value = session.metadata(1)->market_participants.at(key("ABC"));
                TEST_ASSERT(value.primary_market_maker == primary && value.market_maker_mode == mode);
                TEST_ASSERT(value.market_participant_state == state);
            }
        }
    }
    for (const char level : {'1', '2', '3'}) {
        send(session, breach(0, level));
        TEST_ASSERT(session.market_wide_metadata()->status->breached_level == level);
    }
    const std::array<char, 3> markets{{'Q', 'B', 'X'}};
    for (std::size_t index = 0; index < markets.size(); ++index) {
        for (const char action : {'H', 'T'}) {
            send(session, halt(1, "AAPL", markets[index], action));
            TEST_ASSERT(session.metadata(1)->operational_halts[index]->market_code == markets[index]);
            TEST_ASSERT(session.metadata(1)->operational_halts[index]->action == action);
        }
    }
    for (const char direction : {'B', 'S', 'N', 'O'}) {
        for (const char type : {'O', 'C', 'H'}) {
            for (const char variation : {'L', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', ' '}) {
                send(session, imbalance(1, "AAPL", 0, 0, direction, 0, 0, 0, type, variation));
                TEST_ASSERT(session.metadata(1)->imbalance->imbalance_direction == direction);
                TEST_ASSERT(session.metadata(1)->imbalance->cross_type == type);
                TEST_ASSERT(session.metadata(1)->imbalance->price_variation_indicator == variation);
            }
        }
    }
    for (const char interest : {'B', 'S', 'A', 'N'}) {
        send(session, retail(1, "AAPL", interest));
        TEST_ASSERT(session.metadata(1)->retail->interest == interest);
    }
    TEST_ASSERT(session.metadata(1)->market_participants.size() == 1);
    complete(session);
}
void test_metadata_routing_and_field_failures() {
    const std::vector<Bytes> invalid{
        reg_sho(0), reg_sho(3), reg_sho(1, "MSFT"), reg_sho(1, "AAPL", '3'),
        participant(0), participant(3), participant(1, "MSFT"),
        participant(1, "AAPL", "ABCD", 'X'), participant(1, "AAPL", "ABCD", 'Y', 'X'),
        participant(1, "AAPL", "ABCD", 'Y', 'N', 'X'),
        participant(1, "AAPL", ""), participant(1, "AAPL", "A B"),
        decline_levels(1), breach(1), breach(0, '0'), breach(0, '4'),
        ipo(1), ipo(0, "UNKNOWN"), ipo(0, "AAPL", 86400),
        ipo(0, "AAPL", 1, 'X'), ipo(0, "AAPL", 1, 'C', 0),
        ipo(0, "AAPL", 0, 'C', 1), ipo(0, "AAPL", 1, 'A', 2000000001U),
        collar(0), collar(3), collar(1, "MSFT"), collar(1, "AAPL", 2000000001U),
        collar(1, "AAPL", 100, 2000000001U), collar(1, "AAPL", 100, 110, 2000000001U),
        halt(0), halt(3), halt(1, "MSFT"), halt(1, "AAPL", 'Z'), halt(1, "AAPL", 'Q', 'X'),
        imbalance(0), imbalance(3), imbalance(1, "MSFT"),
        imbalance(1, "AAPL", 0, 0, 'X'),
        imbalance(1, "AAPL", 0, 0, 'B', 2000000001U),
        imbalance(1, "AAPL", 0, 0, 'B', 0, 2000000001U),
        imbalance(1, "AAPL", 0, 0, 'B', 0, 0, 2000000001U),
        imbalance(1, "AAPL", 0, 0, 'B', 0, 0, 0, 'I'),
        imbalance(1, "AAPL", 0, 0, 'B', 0, 0, 0, 'A'),
        imbalance(1, "AAPL", 0, 0, 'B', 0, 0, 0, 'O', 'D'),
        retail(0), retail(3), retail(1, "MSFT"), retail(1, "AAPL", 'X')
    };
    for (const auto& record : invalid) {
        ItchBookRebuilder session;
        start(session, true);
        const auto* borrowed = session.book(1);
        const auto before = borrowed->capture_state();
        expect_failure(session, [&] { send(session, record); });
        TEST_ASSERT(borrowed->capture_state() == before && borrowed->check_invariants());
    }
    {
        ItchBookRebuilder session;
        start(session);
        auto malformed = participant();
        malformed[12] = 0;
        expect_failure(session, [&] { send(session, malformed); });
    }
    for (const auto& record : {reg_sho(), participant(), ipo(), collar(), halt(), imbalance(), retail()}) {
        ItchBookRebuilder session;
        send(session, system_event('O'));
        expect_failure(session, [&] { send(session, record); }); // No directory, even K's symbol cannot route.
    }
}
void test_participant_capacity_and_updates() {
    {
        RebuildLimits limits;
        limits.maximum_market_participants_per_instrument = 0;
        bool rejected = false;
        try { ItchBookRebuilder session(limits); }
        catch (const std::invalid_argument&) { rejected = true; }
        TEST_ASSERT(rejected);
    }
    RebuildLimits limits;
    limits.maximum_market_participants_per_instrument = 1;
    ItchBookRebuilder session(limits);
    start(session, true);
    send(session, participant());
    send(session, participant(1, "AAPL", "ABCD", 'N', 'R', 'W')); // Existing slot updates at the cap.
    send(session, participant(2, "MSFT", "EFGH")); // Cap is per instrument, not global.
    TEST_ASSERT(session.metadata(1)->market_participants.size() == 1);
    TEST_ASSERT(session.metadata(2)->market_participants.size() == 1);
    TEST_ASSERT(session.metadata(1)->market_participants.at(key("ABCD")).market_maker_mode == 'R');
    const auto* borrowed = session.metadata(1);
    expect_failure(session, [&] { send(session, participant(1, "AAPL", "IJKL")); });
    TEST_ASSERT(borrowed->market_participants.size() == 1);
    TEST_ASSERT(borrowed->market_participants.at(key("ABCD")).market_participant_state == 'W');
}
std::vector<Bytes> all_kind_records() {
    std::vector<Bytes> records{system_event('O'), decline_levels(0, 1, 2, 3), breach(),
        directory(1, "AAPL"), directory(2, "MSFT"),
        add(1, 100, 'B', 10, "AAPL", 100), add(2, 200, 'S', 8, "MSFT", 200, true)};
    const auto metadata = metadata_records();
    records.insert(records.end(), metadata.begin(), metadata.end());
    const std::vector<Bytes> suffix{trading(), execute(1, 100, 2, 500), priced_execute(1, 100, 1, 501),
        cancel(1, 100, 1), replace(1, 100, 101, 7, 101), non_displayed(), cross(), broken(),
        erase(2, 200), system_event('C')};
    records.insert(records.end(), suffix.begin(), suffix.end());
    return records;
}
Bytes all_kind_wire() {
    Bytes wire;
    for (const auto& record : all_kind_records()) append_frame(wire, record);
    wire.insert(wire.end(), {0, 0});
    return wire;
}
void check_all_kind_replay(const ItchBookRebuilder& session) {
    TEST_ASSERT(session.state() == RebuildState::COMPLETE && session.metrics().errors == 0);
    const auto* aapl = session.book(1);
    const auto* msft = session.book(2);
    TEST_ASSERT(aapl && msft && aapl->order_count() == 1 && msft->order_count() == 0);
    const auto* order = aapl->find_order(101);
    TEST_ASSERT(order && order->price == Price(101) && order->remaining == Quantity(7));
    TEST_ASSERT(order->side == Side::BUY && !aapl->find_order(100));
    TEST_ASSERT(aapl->best_bid() == Price(101) && !aapl->best_ask());
    TEST_ASSERT(aapl->bid_quantity(Price(100)) == 0 && aapl->bid_quantity(Price(101)) == 7);
    TEST_ASSERT(session.active_order_count() == 1 && session.seen_order_reference_count() == 3);
    check_golden_metadata(session);
    TEST_ASSERT(session.market_wide_metadata()->decline_levels->level1 == 1);
    TEST_ASSERT(session.market_wide_metadata()->status->breached_level == '2');
    TEST_ASSERT(session.trading_action(2)->trading_state == 'Q' && session.check_invariants());
    const std::string_view kinds = "SRHYLVWKJhAFECXDUPQBIN";
    TEST_ASSERT(kinds.size() == 22);
    std::size_t covered = 0;
    std::uint64_t total = 0;
    for (const auto count : session.metrics().records_by_type) {
        if (count != 0) ++covered;
        total += count;
    }
    TEST_ASSERT(covered == 22);
    for (const char kind : kinds) TEST_ASSERT(session.metrics().records_by_type[static_cast<unsigned char>(kind)] != 0);
    TEST_ASSERT(total == all_kind_records().size() && total == session.metrics().processed_records);
}
void test_all_22_kind_streaming_replay() {
    const auto wire = all_kind_wire();
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        ItchBookRebuilder session;
        session.consume(wire.data(), split);
        session.consume(wire.data() + split, wire.size() - split);
        session.finish();
        check_all_kind_replay(session);
    }
    ItchBookRebuilder bytewise;
    for (const auto value : wire) bytewise.consume(&value, 1);
    bytewise.finish();
    check_all_kind_replay(bytewise);
    for (const auto mask : {std::ios::goodbit, std::ios::eofbit | std::ios::failbit | std::ios::badbit}) {
        std::istringstream input(std::string(wire.begin(), wire.end()));
        input.exceptions(mask);
        ItchBookRebuilder session;
        session.read(input);
        check_all_kind_replay(session);
    }
}
void test_metadata_lengths_and_unknown_still_fail_closed() {
    auto records = metadata_records();
    records.push_back(decline_levels());
    records.push_back(breach());
    for (const auto& record : records) {
        for (const bool shortened : {false, true}) {
            ItchBookRebuilder session;
            start(session, true);
            auto malformed = record;
            if (shortened) malformed.pop_back();
            else malformed.push_back(0);
            expect_failure(session, [&] { send(session, malformed); });
        }
    }
    for (const auto& record : {message('Z', 1, Bytes(9, 0)), message('T', 0, Bytes(1, 0))}) {
        ItchBookRebuilder session;
        start(session, true);
        expect_failure(session, [&] { send(session, record); });
    }
    {
        ItchBookRebuilder session;
        start(session);
        send(session, system_event('C'));
        expect_failure(session, [&] { send(session, reg_sho()); });
    }
    {
        auto wire = all_kind_wire();
        wire.pop_back();
        std::istringstream input(std::string(wire.begin(), wire.end()));
        ItchBookRebuilder session;
        bool threw = false;
        try { session.read(input); }
        catch (const std::exception&) { threw = true; }
        TEST_ASSERT(threw && session.state() == RebuildState::STALE);
        TEST_ASSERT(!session.metadata(1) && !session.market_wide_metadata());
    }
}
} // namespace

int main() {
    try {
        test_metadata_does_not_mutate_visible_books();
        test_market_wide_and_special_zero_values();
        test_supported_historical_enum_updates();
        test_metadata_routing_and_field_failures();
        test_participant_capacity_and_updates();
        test_all_22_kind_streaming_replay();
        test_metadata_lengths_and_unknown_still_fail_closed();
        std::cout << "ITCH metadata session tests passed (synthetic only; all 22 historical wire kinds).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH metadata session test failed: " << error.what() << '\n';
        return 1;
    }
}
