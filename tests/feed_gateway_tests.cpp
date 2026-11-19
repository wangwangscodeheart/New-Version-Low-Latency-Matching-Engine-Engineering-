#include "feed_decoder.hpp"
#include "market_data_gateway.hpp"
#include "market_data_replay.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <vector>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {
void put(std::vector<uint8_t>& out, uint64_t value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        const size_t shift = (bytes - i - 1) * 8;
        out.push_back(static_cast<uint8_t>(value >> shift));
    }
}
std::vector<uint8_t> add(uint32_t instrument, uint64_t sequence, uint64_t order,
                         int64_t price, uint64_t quantity, Side side) {
    std::vector<uint8_t> out;
    put(out, 1, 2); put(out, 25, 2); put(out, instrument, 4);
    put(out, sequence, 8); put(out, sequence * 100, 8);
    put(out, order, 8); put(out, static_cast<uint64_t>(price), 8);
    put(out, quantity, 8); put(out, side == Side::BUY ? 0 : 1, 1);
    return out;
}
std::vector<uint8_t> cancel(uint32_t instrument, uint64_t sequence, uint64_t order) {
    std::vector<uint8_t> out;
    put(out, 2, 2); put(out, 8, 2); put(out, instrument, 4);
    put(out, sequence, 8); put(out, sequence * 100, 8); put(out, order, 8);
    return out;
}
std::vector<uint8_t> trade(uint32_t instrument, uint64_t sequence,
                           uint64_t trade_id, uint64_t passive_order_id,
                           int64_t price, uint64_t quantity) {
    std::vector<uint8_t> out;
    put(out, 3, 2); put(out, 32, 2); put(out, instrument, 4);
    put(out, sequence, 8); put(out, sequence * 100, 8);
    put(out, trade_id, 8); put(out, passive_order_id, 8);
    put(out, static_cast<uint64_t>(price), 8); put(out, quantity, 8);
    return out;
}
void append(std::vector<uint8_t>& destination, const std::vector<uint8_t>& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}
FeedMessage direct_add(uint32_t instrument, uint64_t sequence, uint64_t order,
                       uint64_t quantity, Side side = Side::BUY) {
    return FeedMessage{FeedHeader{FeedMessageType::ADD_ORDER, 25,
        InstrumentId(instrument), sequence, Timestamp(0)},
        FeedAddOrder{order, Price(1000000), Quantity(quantity), side}};
}
bool same_checkpoint(const MarketDataCheckpoint& left,
                     const MarketDataCheckpoint& right) {
    return left.books == right.books && left.sequences == right.sequences &&
        left.states == right.states &&
        left.metrics.applied == right.metrics.applied &&
        left.metrics.gaps == right.metrics.gaps &&
        left.metrics.duplicates == right.metrics.duplicates &&
        left.metrics.out_of_order == right.metrics.out_of_order &&
        left.metrics.invalid_instruments == right.metrics.invalid_instruments &&
        left.metrics.invalid_messages == right.metrics.invalid_messages &&
        left.metrics.stale_messages == right.metrics.stale_messages;
}

void test_partial_multi_symbol_and_replay() {
    std::vector<uint8_t> wire;
    append(wire, add(0, 1, 10, 1000000, 5, Side::BUY));
    append(wire, add(1, 1, 20, 2000000, 7, Side::SELL));
    append(wire, cancel(0, 2, 10));

    const auto replay = [&](MarketDataGateway& gateway) {
        FeedDecoder decoder;
        decoder.consume(wire.data(), 11, [&](const FeedMessage& m) { gateway.on_message(m); });
        decoder.consume(wire.data() + 11, wire.size() - 11,
                        [&](const FeedMessage& m) { gateway.on_message(m); });
        decoder.finish();
    };
    MarketDataGateway first(3), second(3);
    replay(first); replay(second);
    TEST_ASSERT(first.metrics().applied == 3);
    TEST_ASSERT(first.book(InstrumentId(0))->order_count() == 0);
    TEST_ASSERT(first.book(InstrumentId(1))->ask_quantity(Price(2000000)) == 7);
    TEST_ASSERT(second.book(InstrumentId(1))->ask_quantity(Price(2000000)) == 7);
    TEST_ASSERT(second.last_sequence(InstrumentId(0)) == 2);

    std::string binary(reinterpret_cast<const char*>(wire.data()), wire.size());
    std::istringstream input(binary, std::ios::binary);
    MarketDataGateway streamed(3);
    const MarketDataReplayResult replayed = MarketDataReplay::replay(input, streamed);
    TEST_ASSERT(replayed.decoded == 3 && replayed.applied == 3);
    TEST_ASSERT(streamed.book(InstrumentId(1))->ask_quantity(Price(2000000)) == 7);
}

void test_sequence_and_checkpoint() {
    MarketDataGateway gateway(2);
    auto send = [&](const std::vector<uint8_t>& bytes) {
        FeedDecoder decoder;
        decoder.consume(bytes, [&](const FeedMessage& m) { gateway.on_message(m); });
        decoder.finish();
    };
    send(add(0, 10, 1, 1000000, 1, Side::BUY));
    send(add(0, 12, 2, 1000001, 1, Side::BUY));
    send(add(0, 12, 3, 1000002, 1, Side::BUY));
    send(add(0, 11, 4, 1000003, 1, Side::BUY));
    send(add(9, 1, 5, 1000004, 1, Side::BUY));
    TEST_ASSERT(gateway.metrics().gaps == 1);
    TEST_ASSERT(gateway.metrics().stale_messages == 2);
    TEST_ASSERT(gateway.metrics().invalid_instruments == 1);
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 1);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 10);
    TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::STALE);

    // Sequence and book state must be recovered together from an authoritative
    // contiguous source; there is intentionally no sequence-only recovery API.
    MarketDataGateway authoritative(2);
    auto recover_send = [&](const std::vector<uint8_t>& bytes) {
        FeedDecoder decoder;
        decoder.consume(bytes, [&](const FeedMessage& m) {
            TEST_ASSERT(authoritative.on_message(m));
        });
        decoder.finish();
    };
    recover_send(add(0, 10, 1, 1000000, 1, Side::BUY));
    recover_send(add(0, 11, 4, 1000003, 1, Side::BUY));
    recover_send(add(0, 12, 2, 1000001, 1, Side::BUY));
    MarketDataGateway recovered(2);
    recovered.restore_checkpoint(authoritative.checkpoint());
    TEST_ASSERT(recovered.last_sequence(InstrumentId(0)) == 12);
    TEST_ASSERT(recovered.state(InstrumentId(0)) == FeedState::HEALTHY);
    TEST_ASSERT(recovered.book(InstrumentId(0))->capture_state() ==
                authoritative.book(InstrumentId(0))->capture_state());

    // The next contiguous message is accepted after atomic recovery.
    FeedDecoder continuation;
    const auto next = add(0, 13, 6, 1000005, 2, Side::SELL);
    continuation.consume(next, [&](const FeedMessage& message) {
        TEST_ASSERT(recovered.on_message(message));
    });
    TEST_ASSERT(recovered.last_sequence(InstrumentId(0)) == 13);
}

void test_trade_reduces_passive_order() {
    MarketDataGateway gateway(1);
    const auto apply = [&](const std::vector<uint8_t>& bytes) {
        FeedDecoder decoder;
        bool accepted = false;
        decoder.consume(bytes, [&](const FeedMessage& message) {
            accepted = gateway.on_message(message);
        });
        decoder.finish();
        return accepted;
    };
    TEST_ASSERT(apply(add(0, 1, 42, 1000000, 10, Side::SELL)));
    TEST_ASSERT(apply(trade(0, 2, 900, 42, 1000000, 4)));
    TEST_ASSERT(gateway.book(InstrumentId(0))->ask_quantity(Price(1000000)) == 6);
    TEST_ASSERT(apply(trade(0, 3, 901, 42, 1000000, 6)));
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 0);
}

void test_invalid_business_message_marks_feed_stale() {
    MarketDataGateway gateway(1);
    auto send = [&](const std::vector<uint8_t>& bytes) {
        FeedDecoder decoder;
        bool accepted = false;
        decoder.consume(bytes, [&](const FeedMessage& message) {
            accepted = gateway.on_message(message);
        });
        decoder.finish();
        return accepted;
    };
    TEST_ASSERT(send(add(0, 1, 10, 1000000, 5, Side::BUY)));
    TEST_ASSERT(!send(cancel(0, 2, 999))); // unknown order corrupts continuity
    TEST_ASSERT(gateway.metrics().invalid_messages == 1);
    TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::STALE);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 2);
    TEST_ASSERT(!send(add(0, 3, 11, 1000001, 1, Side::BUY)));
    TEST_ASSERT(gateway.metrics().stale_messages == 1);
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 1);
}

void test_invalid_side_and_aggregate_overflow_do_not_mutate_book() {
    const uint64_t maximum = std::numeric_limits<uint64_t>::max();
    MarketDataBook book;
    TEST_ASSERT(!book.apply(FeedAddOrder{1, Price(1000000), Quantity(1),
                                        static_cast<Side>(2)}));
    TEST_ASSERT(book.order_count() == 0);
    TEST_ASSERT(book.ask_quantity(Price(1000000)) == 0);
    TEST_ASSERT(book.apply(FeedAddOrder{1, Price(1000000), Quantity(maximum - 1),
                                       Side::BUY}));
    TEST_ASSERT(book.apply(FeedAddOrder{2, Price(1000000), Quantity(maximum),
                                       Side::SELL}));
    const auto before = book.capture_state();
    TEST_ASSERT(!book.apply(FeedAddOrder{3, Price(1000000), Quantity(2), Side::BUY}));
    TEST_ASSERT(!book.apply(FeedAddOrder{4, Price(1000000), Quantity(1), Side::SELL}));
    TEST_ASSERT(!book.apply(FeedAddOrder{5, Price(1000000), Quantity(1),
                                        static_cast<Side>(255)}));
    TEST_ASSERT(book.capture_state() == before);
    TEST_ASSERT(book.bid_quantity(Price(1000000)) == maximum - 1);
    TEST_ASSERT(book.ask_quantity(Price(1000000)) == maximum);
    TEST_ASSERT(book.apply(FeedAddOrder{3, Price(1000000), Quantity(1), Side::BUY}));
    TEST_ASSERT(book.bid_quantity(Price(1000000)) == maximum);

    MarketDataGateway invalid_side(2);
    TEST_ASSERT(!invalid_side.on_message(direct_add(0, 1, 1, 1, static_cast<Side>(2))));
    TEST_ASSERT(invalid_side.book(InstrumentId(0))->order_count() == 0);
    TEST_ASSERT(invalid_side.state(InstrumentId(0)) == FeedState::STALE);
    TEST_ASSERT(invalid_side.state(InstrumentId(1)) == FeedState::HEALTHY);
    TEST_ASSERT(invalid_side.metrics().invalid_messages == 1);

    MarketDataGateway overflow(2);
    TEST_ASSERT(overflow.on_message(direct_add(0, 1, 1, maximum)));
    const auto gateway_before = overflow.book(InstrumentId(0))->capture_state();
    TEST_ASSERT(!overflow.on_message(direct_add(0, 2, 2, 1)));
    TEST_ASSERT(overflow.book(InstrumentId(0))->capture_state() == gateway_before);
    TEST_ASSERT(overflow.book(InstrumentId(0))->bid_quantity(Price(1000000)) == maximum);
    TEST_ASSERT(overflow.last_sequence(InstrumentId(0)) == 2);
    TEST_ASSERT(overflow.state(InstrumentId(0)) == FeedState::STALE);
    TEST_ASSERT(overflow.state(InstrumentId(1)) == FeedState::HEALTHY);
    TEST_ASSERT(overflow.metrics().invalid_messages == 1);
}

void test_healthy_duplicates_and_older_messages_are_dropped() {
    MarketDataGateway gateway(1);
    TEST_ASSERT(gateway.on_message(direct_add(0, 10, 1, 1)));
    TEST_ASSERT(!gateway.on_message(direct_add(0, 10, 2, 1)));
    TEST_ASSERT(!gateway.on_message(direct_add(0, 9, 3, 1)));
    TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::HEALTHY);
    TEST_ASSERT(gateway.book(InstrumentId(0))->order_count() == 1);
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 10);
    TEST_ASSERT(gateway.metrics().duplicates == 1);
    TEST_ASSERT(gateway.metrics().out_of_order == 1);
    TEST_ASSERT(gateway.on_message(direct_add(0, 11, 4, 1)));
}

void test_explicit_invalidation_preserves_book_and_sequence() {
    MarketDataGateway gateway(3);
    static_assert(noexcept(gateway.invalidate(InstrumentId(0))), "invalidation must not throw");
    static_assert(noexcept(gateway.invalidate_all()), "global invalidation must not throw");
    static_assert(noexcept(gateway.instrument_count()), "instrument count must not throw");
    TEST_ASSERT(gateway.instrument_count() == 3);
    TEST_ASSERT(gateway.on_message(direct_add(0, 10, 1, 5)));
    TEST_ASSERT(gateway.on_message(direct_add(1, 20, 2, 7, Side::SELL)));
    const auto before = gateway.checkpoint();
    gateway.invalidate(InstrumentId(1));
    gateway.invalidate(InstrumentId(1));
    gateway.invalidate(InstrumentId());
    gateway.invalidate(InstrumentId(3));
    TEST_ASSERT(gateway.state(InstrumentId(0)) == FeedState::HEALTHY);
    TEST_ASSERT(gateway.state(InstrumentId(1)) == FeedState::STALE);
    TEST_ASSERT(gateway.state(InstrumentId(2)) == FeedState::HEALTHY);
    gateway.invalidate_all();
    for (uint32_t i = 0; i < gateway.instrument_count(); ++i) {
        TEST_ASSERT(gateway.state(InstrumentId(i)) == FeedState::STALE);
    }
    auto expected = before;
    expected.states.assign(3, FeedState::STALE);
    TEST_ASSERT(same_checkpoint(gateway.checkpoint(), expected));
    TEST_ASSERT(!gateway.on_message(direct_add(0, 11, 3, 1)));
    TEST_ASSERT(gateway.last_sequence(InstrumentId(0)) == 10);
    TEST_ASSERT(gateway.metrics().stale_messages == 1);

    MarketDataGateway empty(0);
    empty.invalidate(InstrumentId(0));
    empty.invalidate_all();
    TEST_ASSERT(empty.instrument_count() == 0);
}

void test_invalid_checkpoint_is_rejected_atomically() {
    MarketDataGateway gateway(2), authoritative(2);
    TEST_ASSERT(gateway.on_message(direct_add(0, 10, 1, 5)));
    TEST_ASSERT(gateway.on_message(direct_add(1, 20, 2, 7, Side::SELL)));
    TEST_ASSERT(authoritative.on_message(direct_add(0, 30, 11, 8, Side::SELL)));
    TEST_ASSERT(authoritative.on_message(direct_add(1, 40, 12, 9)));
    const auto before = gateway.checkpoint();
    const auto valid = authoritative.checkpoint();
    const auto reject = [&](const MarketDataCheckpoint& checkpoint) {
        bool rejected = false;
        try { gateway.restore_checkpoint(checkpoint); }
        catch (const std::runtime_error&) { rejected = true; }
        TEST_ASSERT(rejected);
        TEST_ASSERT(same_checkpoint(gateway.checkpoint(), before));
    };
    auto invalid = valid;
    invalid.states[1] = static_cast<FeedState>(255);
    reject(invalid);
    invalid = valid;
    invalid.books[1].orders[0].side = static_cast<Side>(2);
    reject(invalid);
    invalid = valid;
    invalid.books[1].orders.push_back(invalid.books[1].orders[0]);
    reject(invalid);
    invalid = valid;
    invalid.books[1].orders[0].remaining = Quantity(std::numeric_limits<uint64_t>::max());
    invalid.books[1].orders.push_back(MarketDataSnapshotOrder{
        13, Price(1000000), Quantity(1), Side::BUY});
    reject(invalid);
    invalid = valid;
    invalid.books[1].orders[0].remaining = Quantity(0);
    reject(invalid);
    invalid = valid;
    invalid.states.pop_back();
    reject(invalid);
    gateway.restore_checkpoint(valid);
    TEST_ASSERT(same_checkpoint(gateway.checkpoint(), valid));
}

void test_malformed_and_incomplete() {
    std::vector<uint8_t> invalid = add(0, 1, 1, 1000000, 1, Side::BUY);
    invalid[2] = 0; invalid[3] = 8;
    FeedDecoder decoder;
    bool invalid_rejected = false;
    try { decoder.consume(invalid, [](const FeedMessage&) {}); }
    catch (const FeedDecodeException& e) {
        invalid_rejected = e.error() == FeedDecodeError::INVALID_LENGTH;
    }
    TEST_ASSERT(invalid_rejected);

    FeedDecoder partial;
    const auto frame = add(0, 1, 1, 1000000, 1, Side::BUY);
    partial.consume(frame.data(), frame.size() - 1, [](const FeedMessage&) {});
    bool tail_rejected = false;
    try { partial.finish(); }
    catch (const FeedDecodeException& e) {
        tail_rejected = e.error() == FeedDecodeError::INCOMPLETE_TAIL;
    }
    TEST_ASSERT(tail_rejected);
}
}

int main() {
    try {
        test_partial_multi_symbol_and_replay();
        test_sequence_and_checkpoint();
        test_trade_reduces_passive_order();
        test_invalid_business_message_marks_feed_stale();
        test_invalid_side_and_aggregate_overflow_do_not_mutate_book();
        test_healthy_duplicates_and_older_messages_are_dropped();
        test_explicit_invalidation_preserves_book_and_sequence();
        test_invalid_checkpoint_is_rejected_atomically();
        test_malformed_and_incomplete();
        std::cout << "Feed gateway tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Feed gateway tests failed: " << error.what() << '\n';
        return 1;
    }
}
