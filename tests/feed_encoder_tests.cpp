#include "feed_encoder.hpp"
#include "market_data_gateway.hpp"
#include "market_data_replay.hpp"
#include "trading_engine.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {
FeedMessage add(uint32_t instrument, uint64_t sequence, uint64_t reference,
                int64_t price, uint64_t quantity, Side side = Side::BUY) {
    return {{FeedMessageType::ADD_ORDER, 25, InstrumentId(instrument), sequence, Timestamp(100)},
            FeedAddOrder{reference, Price(price), Quantity(quantity), side}};
}
FeedMessage cancel(uint32_t instrument, uint64_t sequence, uint64_t reference) {
    return {{FeedMessageType::CANCEL_ORDER, 8, InstrumentId(instrument), sequence, Timestamp(101)},
            FeedCancelOrder{reference}};
}
FeedMessage trade(uint32_t instrument, uint64_t sequence, uint64_t trade_id,
                  uint64_t passive, int64_t price, uint64_t quantity) {
    return {{FeedMessageType::TRADE, 32, InstrumentId(instrument), sequence, Timestamp(102)},
            FeedTrade{trade_id, passive, Price(price), Quantity(quantity)}};
}
bool same_message(const FeedMessage& left, const FeedMessage& right) {
    if (left.header.type != right.header.type || left.header.body_length != right.header.body_length ||
        left.header.instrument_id != right.header.instrument_id || left.header.sequence != right.header.sequence ||
        left.header.exchange_timestamp != right.header.exchange_timestamp || left.payload.index() != right.payload.index()) return false;
    if (const auto* value = std::get_if<FeedAddOrder>(&left.payload)) {
        const auto& other = std::get<FeedAddOrder>(right.payload);
        return value->order_id == other.order_id && value->price == other.price &&
               value->quantity == other.quantity && value->side == other.side;
    }
    if (const auto* value = std::get_if<FeedCancelOrder>(&left.payload)) {
        return value->order_id == std::get<FeedCancelOrder>(right.payload).order_id;
    }
    const auto& value = std::get<FeedTrade>(left.payload);
    const auto& other = std::get<FeedTrade>(right.payload);
    return value.trade_id == other.trade_id && value.passive_order_id == other.passive_order_id &&
           value.price == other.price && value.quantity == other.quantity;
}
void check_roundtrip(const FeedMessage& expected) {
    const auto frame = FeedEncoder::encode(expected);
    FeedDecoder decoder;
    size_t count = 0;
    decoder.consume(frame.data(), frame.size(), [&](const FeedMessage& actual) {
        TEST_ASSERT(same_message(actual, expected)); ++count;
    });
    decoder.finish();
    TEST_ASSERT(count == 1 && frame.size() == 24 + expected.header.body_length);
    for (size_t i = frame.size(); i < frame.bytes.size(); ++i) TEST_ASSERT(frame.bytes[i] == 0);
}
void append(std::vector<uint8_t>& bytes, const FeedMessage& message) {
    const auto frame = FeedEncoder::encode(message);
    bytes.insert(bytes.end(), frame.data(), frame.data() + frame.size());
}
void test_literal_wire_goldens() {
    const uint32_t instrument = 0x01020304;
    const uint64_t sequence = 0x0102030405060708ULL, timestamp = 0x1112131415161718ULL;
    const uint64_t reference = 0x2122232425262728ULL, quantity = 0x4142434445464748ULL;
    const int64_t price = 0x3132333435363738LL;
    std::vector<uint8_t> expected_add{
        0,1,0,25, 1,2,3,4, 1,2,3,4,5,6,7,8, 0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28, 0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,
        0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48, 0};
    auto value = add(instrument, sequence, reference, price, quantity);
    value.header.exchange_timestamp = Timestamp(timestamp);
    for (Side side : {Side::BUY, Side::SELL}) {
        std::get<FeedAddOrder>(value.payload).side = side;
        expected_add.back() = side == Side::BUY ? 0 : 1;
        const auto frame = FeedEncoder::encode(value);
        TEST_ASSERT(frame.size() == 49 && std::equal(expected_add.begin(), expected_add.end(), frame.data()));
        check_roundtrip(value);
    }
    const std::vector<uint8_t> expected_cancel{
        0,2,0,8, 1,2,3,4, 1,2,3,4,5,6,7,8, 0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28};
    value = cancel(instrument, sequence, reference);
    value.header.exchange_timestamp = Timestamp(timestamp);
    auto frame = FeedEncoder::encode(value);
    TEST_ASSERT(frame.size() == 32 && std::equal(expected_cancel.begin(), expected_cancel.end(), frame.data()));
    check_roundtrip(value);
    const std::vector<uint8_t> expected_trade{
        0,3,0,32, 1,2,3,4, 1,2,3,4,5,6,7,8, 0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
        0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58, 0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
        0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38, 0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48};
    value = trade(instrument, sequence, 0x5152535455565758ULL, reference, price, quantity);
    value.header.exchange_timestamp = Timestamp(timestamp);
    frame = FeedEncoder::encode(value);
    TEST_ASSERT(frame.size() == 56 && std::equal(expected_trade.begin(), expected_trade.end(), frame.data()));
    check_roundtrip(value);
    std::cout << "Wire goldens passed: Add BUY/SELL 49, Cancel 32, Trade 56 bytes\n";
}
void test_full_width_valid_fields() {
    const auto maximum = std::numeric_limits<uint64_t>::max();
    auto value = add(InstrumentId::INVALID_VALUE - 1, maximum, maximum,
                     std::numeric_limits<int64_t>::max(), maximum, Side::SELL);
    value.header.exchange_timestamp = Timestamp(maximum);
    check_roundtrip(value);
    value.header.exchange_timestamp = Timestamp(0);
    check_roundtrip(value);
    check_roundtrip(cancel(0, maximum, maximum));
    check_roundtrip(trade(0, maximum, maximum, maximum, std::numeric_limits<int64_t>::max(), maximum));
}
void test_invalid_output_profile_is_rejected() {
    size_t rejected = 0;
    const auto check = [&](const FeedMessage& message) {
        const FeedMessage before = message;
        bool failed = false;
        try { (void)FeedEncoder::encode(message); } catch (const std::invalid_argument&) { failed = true; }
        TEST_ASSERT(failed && same_message(message, before)); ++rejected;
    };
    const std::vector<FeedMessage> valid{add(0,1,1,100,1), cancel(0,1,1), trade(0,1,1,1,100,1)};
    for (size_t i = 0; i < valid.size(); ++i) {
        auto bad = valid[i]; bad.header.sequence = 0; check(bad);
        bad = valid[i]; bad.header.instrument_id = InstrumentId{}; check(bad);
        for (uint16_t length : {uint16_t{0}, uint16_t{1}, uint16_t{65535}}) {
            bad = valid[i]; bad.header.body_length = length; check(bad);
        }
        for (size_t other = 0; other < valid.size(); ++other) {
            if (i == other) continue;
            bad = valid[i]; bad.payload = valid[other].payload; check(bad);
        }
    }
    for (uint16_t type : {uint16_t{0}, uint16_t{65535}}) {
        auto bad = valid[0]; bad.header.type = static_cast<FeedMessageType>(type); check(bad);
    }
    auto bad = valid[0]; std::get<FeedAddOrder>(bad.payload).order_id = 0; check(bad);
    for (int64_t price : {int64_t{0}, int64_t{-1}, std::numeric_limits<int64_t>::min()}) {
        bad = valid[0]; std::get<FeedAddOrder>(bad.payload).price = Price(price); check(bad);
        bad = valid[2]; std::get<FeedTrade>(bad.payload).price = Price(price); check(bad);
    }
    bad = valid[0]; std::get<FeedAddOrder>(bad.payload).quantity = Quantity(0); check(bad);
    for (uint8_t side : {uint8_t{2}, uint8_t{255}}) {
        bad = valid[0]; std::get<FeedAddOrder>(bad.payload).side = static_cast<Side>(side); check(bad);
    }
    bad = valid[1]; std::get<FeedCancelOrder>(bad.payload).order_id = 0; check(bad);
    bad = valid[2]; std::get<FeedTrade>(bad.payload).trade_id = 0; check(bad);
    bad = valid[2]; std::get<FeedTrade>(bad.payload).passive_order_id = 0; check(bad);
    bad = valid[2]; std::get<FeedTrade>(bad.payload).quantity = Quantity(0); check(bad);
    std::cout << "Invalid profile rejected without changing input: " << rejected << " cases\n";
}
void test_every_split_and_concatenated_frames() {
    const std::vector<FeedMessage> messages{add(0,1,11,100,5), trade(0,2,7,11,100,2), cancel(0,3,11)};
    std::vector<uint8_t> bytes;
    for (const auto& message : messages) append(bytes, message);
    for (size_t split = 0; split <= bytes.size(); ++split) {
        FeedDecoder decoder;
        size_t index = 0;
        const auto accept = [&](const FeedMessage& message) {
            TEST_ASSERT(index < messages.size() && same_message(message, messages[index++]));
        };
        decoder.consume(bytes.data(), split, accept);
        decoder.consume(bytes.data() + split, bytes.size() - split, accept);
        decoder.finish();
        TEST_ASSERT(index == messages.size());
    }
    FeedDecoder bytewise;
    size_t decoded = 0;
    for (size_t i = 0; i < bytes.size(); ++i) bytewise.consume(bytes.data() + i, 1, [&](const FeedMessage& message) {
        TEST_ASSERT(decoded < messages.size());
        TEST_ASSERT(same_message(message, messages[decoded++]));
    });
    bytewise.finish();
    TEST_ASSERT(decoded == 3);
    FeedDecoder truncated;
    truncated.consume(bytes.data(), bytes.size() - 1, [](const FeedMessage&) {});
    bool failed = false;
    try { truncated.finish(); } catch (const FeedDecodeException& error) { failed = error.error() == FeedDecodeError::INCOMPLETE_TAIL; }
    TEST_ASSERT(failed);
    std::cout << "Framing passed: " << bytes.size() + 1 << " split points, bytewise, concatenated, truncated tail\n";
}
bool send(MarketDataGateway& gateway, const FeedMessage& message) {
    const auto frame = FeedEncoder::encode(message);
    FeedDecoder decoder;
    bool applied = false;
    decoder.consume(frame.data(), frame.size(), [&](const FeedMessage& decoded) {
        TEST_ASSERT(same_message(decoded, message)); applied = gateway.on_message(decoded);
    });
    decoder.finish();
    return applied;
}
void test_three_instrument_reconstruction_and_replay() {
    const std::vector<FeedMessage> messages{add(0,1,11,1'000'000,5), add(1,1,22,2'000'000,7,Side::SELL),
        trade(0,2,1001,11,1'000'000,2), cancel(0,3,11), trade(1,2,1002,22,2'000'000,7), add(2,1,33,3'000'000,3)};
    MarketDataGateway gateway(3);
    std::vector<uint8_t> bytes;
    for (const auto& message : messages) { TEST_ASSERT(send(gateway, message)); append(bytes, message); }
    TEST_ASSERT(gateway.metrics().applied == 6 && gateway.last_sequence(InstrumentId(0)) == 3);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(1)) == 2 && gateway.last_sequence(InstrumentId(2)) == 1);
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 0 && gateway.book(InstrumentId(1))->order_count() == 0);
    TEST_ASSERT(gateway.book(InstrumentId(2))->bid_quantity(Price(3'000'000)) == 3);
    const std::string binary(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(binary, std::ios::binary);
    MarketDataGateway replayed(3);
    const auto result = MarketDataReplay::replay(input, replayed);
    TEST_ASSERT(result.decoded == 6 && result.applied == 6);
    for (uint32_t i = 0; i < 3; ++i) TEST_ASSERT(replayed.book(InstrumentId(i))->capture_state() == gateway.book(InstrumentId(i))->capture_state());
}
void test_existing_sequence_semantics_remain_explicit() {
    MarketDataGateway gateway(1);
    TEST_ASSERT(send(gateway, add(0,10,1,100,2))); // Existing legacy FIRST is not snapshot-anchored.
    TEST_ASSERT(!send(gateway, cancel(0,10,999)) && gateway.state(InstrumentId(0)) == FeedState::HEALTHY);
    TEST_ASSERT(!send(gateway, cancel(0,9,999)) && gateway.state(InstrumentId(0)) == FeedState::HEALTHY);
    TEST_ASSERT(!send(gateway, add(0,12,2,101,1)) && gateway.state(InstrumentId(0)) == FeedState::STALE);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 10 && !send(gateway, cancel(0,11,1)));
    TEST_ASSERT(gateway.metrics().duplicates == 1 && gateway.metrics().out_of_order == 1 && gateway.metrics().gaps == 1);
    MarketDataGateway invalid(1);
    TEST_ASSERT(send(invalid, add(0,10,1,100,2)));
    TEST_ASSERT(!send(invalid, cancel(0,11,999)) && invalid.last_sequence(InstrumentId(0)) == 11);
    TEST_ASSERT(invalid.state(InstrumentId(0)) == FeedState::STALE);
    TEST_ASSERT(!send(invalid, add(9,1,1,100,1)) && invalid.metrics().invalid_instruments == 1);
}
void test_completed_matching_events_can_supply_codec_fields() {
    MarketManager markets(8);
    EventDispatcher dispatcher;
    TradingEngine engine(markets, dispatcher);
    std::vector<EngineEvent> events;
    auto subscription = dispatcher.subscribe_all([&](const SystemEvent& event) {
        if (const auto* value = std::get_if<EngineEvent>(&event.payload)) events.push_back(*value);
    });
    MarketDataGateway local(3);
    const auto submit = [&](const Symbol& symbol, const Command& command) {
        events.clear();
        const auto result = engine.submit(Timestamp(100), symbol, command);
        TEST_ASSERT(result.routed() && result.process_result.applied() && result.process_result.event_output_complete && events.size() == 1);
    };
    // TEST-ONLY hand projection after completed submit; this is not a live
    // post-Journal Builder/Publisher. Public references are explicitly assigned.
    submit(Symbol("AAPL"), NewOrderCommand(CommandSequence(1), OrderId(50), Side::SELL, Price(1'000'000), Quantity(5)));
    const auto first = std::get<OrderRestedEvent>(events[0]);
    TEST_ASSERT(send(local, add(0,1,5000,first.price.get(),first.remaining_quantity.get(),first.side)));
    submit(Symbol("TSLA"), NewOrderCommand(CommandSequence(1), OrderId(50), Side::BUY, Price(2'000'000), Quantity(2)));
    const auto second = std::get<OrderRestedEvent>(events[0]);
    TEST_ASSERT(send(local, add(1,1,6000,second.price.get(),second.remaining_quantity.get(),second.side)));
    submit(Symbol("AAPL"), NewOrderCommand(CommandSequence(2), OrderId(51), Side::BUY, Price(1'000'000), Quantity(2)));
    const auto fill = std::get<TradeEvent>(events[0]);
    TEST_ASSERT(fill.passive_order_id == OrderId(50) && fill.aggressive_order_id == OrderId(51));
    TEST_ASSERT(send(local, trade(0,2,8000,5000,fill.price.get(),fill.quantity.get())));
    const auto authoritative = markets.find_book(Symbol("AAPL"))->capture_state();
    const auto visible = local.book(InstrumentId(0))->capture_state();
    TEST_ASSERT(!authoritative.ask_levels.empty() && !visible.orders.empty());
    TEST_ASSERT(local.book(InstrumentId(0))->ask_quantity(fill.price) == authoritative.ask_levels.front().total_volume.get());
    TEST_ASSERT(visible.orders.front().order_id == 5000);
    submit(Symbol("AAPL"), CancelOrderCommand(CommandSequence(3), OrderId(50)));
    TEST_ASSERT(std::get<OrderCancelledEvent>(events[0]).cancelled_quantity == Quantity(3));
    TEST_ASSERT(send(local, cancel(0,3,5000)) && local.book(InstrumentId(0))->order_count() == 0);
    TEST_ASSERT(markets.find_book(Symbol("AAPL"))->active_order_count() == 0);
    submit(Symbol("TSLA"), CancelOrderCommand(CommandSequence(2), OrderId(50)));
    TEST_ASSERT(send(local, cancel(1,2,6000)) && local.book(InstrumentId(1))->order_count() == 0);
    TEST_ASSERT(markets.find_book(Symbol("TSLA"))->active_order_count() == 0);
    std::cout << "Matching-event codec integration passed: explicit test-only public references\n";
}
} // namespace

int main() {
    try {
        test_literal_wire_goldens();
        test_full_width_valid_fields();
        test_invalid_output_profile_is_rejected();
        test_every_split_and_concatenated_frames();
        test_three_instrument_reconstruction_and_replay();
        test_existing_sequence_semantics_remain_explicit();
        test_completed_matching_events_can_supply_codec_fields();
        std::cout << "Feed encoder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Feed encoder tests failed: " << error.what() << '\n';
        return 1;
    }
}
