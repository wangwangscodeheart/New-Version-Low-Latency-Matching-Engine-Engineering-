#include "itch50_decoder.hpp"
#include "itch_binary_reader.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t ORDER = 0x1112131415161718ULL;
constexpr std::uint32_t SHARES = 0x21222324U;
constexpr std::uint32_t PRICE = 0x31323334U;
constexpr std::uint64_t MATCH = 0x4142434445464748ULL;

// Every byte below is invented, not extracted from a Nasdaq file. Fixtures
// combine literal wire fields with separately stated expectations; there is no
// production ITCH encoder whose mistakes could cancel decoder mistakes.
Bytes wire(char type, std::initializer_list<std::uint8_t> body) {
    Bytes bytes{static_cast<std::uint8_t>(type), 0x12,0x34, 0x56,0x78, 1,2,3,4,5,6};
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
std::vector<Bytes> goldens() {
    return {
        wire('S', {'O'}),
        wire('R', {'A','A','P','L',' ',' ',' ',' ', 'Q','N', 0x21,0x22,0x23,0x24,
                   'N','C','A','B','P','N','N','1','N', 0x31,0x32,0x33,0x34, 'N'}),
        wire('H', {'A','A','P','L',' ',' ',' ',' ', 'H',' ', 'T','1',' ',' '}),
        wire('A', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18, 'B', 0x21,0x22,0x23,0x24,
                   'A','A','P','L',' ',' ',' ',' ', 0x31,0x32,0x33,0x34}),
        wire('F', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18, 'S', 0x21,0x22,0x23,0x24,
                   'A','A','P','L',' ',' ',' ',' ', 0x31,0x32,0x33,0x34, 'A','B','C','D'}),
        wire('E', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18, 0x21,0x22,0x23,0x24,
                   0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48}),
        wire('C', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18, 0x21,0x22,0x23,0x24,
                   0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48, 'N', 0x31,0x32,0x33,0x34}),
        wire('X', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18, 0x21,0x22,0x23,0x24}),
        wire('D', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18}),
        wire('U', {0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
                   0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,
                   0x21,0x22,0x23,0x24, 0x31,0x32,0x33,0x34}),
        wire('P', {0,0,0,0,0,0,0,0, 'B', 0x21,0x22,0x23,0x24,
                   'A','A','P','L',' ',' ',' ',' ', 0x31,0x32,0x33,0x34,
                   0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48}),
        wire('Q', {0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
                   'A','A','P','L',' ',' ',' ',' ', 0x31,0x32,0x33,0x34,
                   0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48, 'O'}),
        wire('B', {0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48})
    };
}
ItchMessage decode(const Bytes& bytes,
                   UnsupportedPolicy policy = UnsupportedPolicy::REJECT) {
    return Itch50Decoder::decode(bytes.data(), bytes.size(), policy);
}
template<typename Action>
void expect_error(ItchDecodeErrorCode expected, Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const ItchDecodeError& error) {
        TEST_ASSERT(error.code() == expected);
        caught = true;
    }
    TEST_ASSERT(caught);
}
void check_header(const ItchMessage& message, char type) {
    TEST_ASSERT(message.header.type == type);
    TEST_ASSERT(message.header.stock_locate == 0x1234);
    TEST_ASSERT(message.header.tracking_number == 0x5678);
    TEST_ASSERT(message.header.nanoseconds_since_midnight == 0x010203040506ULL);
}
void test_literal_goldens() {
    const auto records = goldens();
    const std::array<std::size_t, 13> sizes{{12,39,25,36,40,31,36,23,19,35,44,40,19}};
    TEST_ASSERT(records.size() == sizes.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        TEST_ASSERT(records[i].size() == sizes[i]);
        TEST_ASSERT(Itch50Decoder::known_payload_length(static_cast<char>(records[i][0])) == sizes[i]);
        check_header(decode(records[i]), static_cast<char>(records[i][0]));
    }
    TEST_ASSERT(std::get<SystemEvent>(decode(records[0]).payload).event_code == 'O');
    const auto directory = std::get<StockDirectory>(decode(records[1]).payload);
    TEST_ASSERT(text_view(directory.stock) == "AAPL");
    TEST_ASSERT(directory.market_category == 'Q' && directory.financial_status == 'N');
    TEST_ASSERT(directory.round_lot_size == SHARES && directory.round_lots_only == 'N');
    TEST_ASSERT(directory.issue_classification == 'C' && text_view(directory.issue_sub_type) == "AB");
    TEST_ASSERT(directory.authenticity == 'P' && directory.short_sale_threshold == 'N');
    TEST_ASSERT(directory.ipo_flag == 'N' && directory.luld_reference_price_tier == '1');
    TEST_ASSERT(directory.etp_flag == 'N' && directory.etp_leverage_factor == PRICE);
    TEST_ASSERT(directory.inverse_indicator == 'N');
    const auto halt = std::get<StockTradingAction>(decode(records[2]).payload);
    TEST_ASSERT(text_view(halt.stock) == "AAPL" && halt.trading_state == 'H');
    TEST_ASSERT(halt.reserved == ' ' && text_view(halt.reason) == "T1");
    for (std::size_t i : {std::size_t{3}, std::size_t{4}}) {
        const auto add = std::get<AddOrder>(decode(records[i]).payload);
        TEST_ASSERT(add.order_reference == ORDER && add.shares == SHARES && add.price == PRICE);
        TEST_ASSERT(text_view(add.stock) == "AAPL");
        TEST_ASSERT(add.side == (i == 3 ? 'B' : 'S'));
        if (i == 3) TEST_ASSERT(!add.attribution.has_value());
        else TEST_ASSERT(add.attribution.has_value() && text_view(*add.attribution) == "ABCD");
    }
    const auto executed = std::get<OrderExecuted>(decode(records[5]).payload);
    TEST_ASSERT(executed.order_reference == ORDER && executed.executed_shares == SHARES);
    TEST_ASSERT(executed.match_number == MATCH);
    const auto priced = std::get<OrderExecutedWithPrice>(decode(records[6]).payload);
    TEST_ASSERT(priced.order_reference == ORDER && priced.executed_shares == SHARES);
    TEST_ASSERT(priced.match_number == MATCH && priced.printable == 'N' && priced.execution_price == PRICE);
    const auto cancel = std::get<OrderCancel>(decode(records[7]).payload);
    TEST_ASSERT(cancel.order_reference == ORDER && cancel.cancelled_shares == SHARES);
    TEST_ASSERT(std::get<OrderDelete>(decode(records[8]).payload).order_reference == ORDER);
    const auto replace = std::get<OrderReplace>(decode(records[9]).payload);
    TEST_ASSERT(replace.original_order_reference == ORDER);
    TEST_ASSERT(replace.new_order_reference == 0x5152535455565758ULL);
    TEST_ASSERT(replace.shares == SHARES && replace.price == PRICE);
    const auto trade = std::get<NonDisplayedTrade>(decode(records[10]).payload);
    TEST_ASSERT(trade.order_reference == 0 && trade.side == 'B' && trade.shares == SHARES);
    TEST_ASSERT(text_view(trade.stock) == "AAPL" && trade.price == PRICE && trade.match_number == MATCH);
    const auto cross = std::get<CrossTrade>(decode(records[11]).payload);
    TEST_ASSERT(cross.shares == 0x2122232425262728ULL && text_view(cross.stock) == "AAPL");
    TEST_ASSERT(cross.cross_price == PRICE && cross.match_number == MATCH && cross.cross_type == 'O');
    TEST_ASSERT(std::get<BrokenTrade>(decode(records[12]).payload).match_number == MATCH);
}
void test_full_width_zero_and_owned_values() {
    auto add = goldens()[3];
    std::fill(add.begin() + 1, add.end(), std::uint8_t{0xff});
    add[19] = 'S';
    const std::array<std::uint8_t, 8> stock{{'B','R','K','.','B',' ',' ',' '}};
    std::copy(stock.begin(), stock.end(), add.begin() + 24);
    const auto message = decode(add);
    TEST_ASSERT(message.header.stock_locate == std::numeric_limits<std::uint16_t>::max());
    TEST_ASSERT(message.header.tracking_number == std::numeric_limits<std::uint16_t>::max());
    TEST_ASSERT(message.header.nanoseconds_since_midnight == 0xffffffffffffULL);
    const auto& value = std::get<AddOrder>(message.payload);
    TEST_ASSERT(value.order_reference == std::numeric_limits<std::uint64_t>::max());
    TEST_ASSERT(value.shares == std::numeric_limits<std::uint32_t>::max());
    TEST_ASSERT(value.price == std::numeric_limits<std::uint32_t>::max());
    std::fill(add.begin(), add.end(), std::uint8_t{0});
    TEST_ASSERT(text_view(value.stock) == "BRK.B" && value.side == 'S');

    auto records = goldens();
    for (auto& bytes : records) {
        const char type = static_cast<char>(bytes[0]);
        std::fill(bytes.begin() + 1, bytes.end(), std::uint8_t{0});
        if (type == 'A' || type == 'F' || type == 'P') bytes[19] = 'B';
        if (type == 'C') bytes[31] = 'Y';
        // Zero prices/quantities/IDs are not silently reinterpreted as an enum
        // or rejected by a structural decoder. Business validation is separate.
        const auto zero = decode(bytes);
        TEST_ASSERT(zero.header.stock_locate == 0 && zero.header.tracking_number == 0);
        TEST_ASSERT(zero.header.nanoseconds_since_midnight == 0);
    }
    const auto zero_add = std::get<AddOrder>(decode(records[3]).payload);
    TEST_ASSERT(zero_add.order_reference == 0 && zero_add.shares == 0 && zero_add.price == 0);
    const auto zero_trade = std::get<NonDisplayedTrade>(decode(records[10]).payload);
    TEST_ASSERT(zero_trade.order_reference == 0 && zero_trade.match_number == 0);

    auto cross = goldens()[11];
    std::fill(cross.begin() + 11, cross.begin() + 19, std::uint8_t{0xff});
    std::fill(cross.begin() + 31, cross.begin() + 39, std::uint8_t{0xff});
    const auto full_cross = std::get<CrossTrade>(decode(cross).payload);
    TEST_ASSERT(full_cross.shares == std::numeric_limits<std::uint64_t>::max());
    TEST_ASSERT(full_cross.match_number == std::numeric_limits<std::uint64_t>::max());

    auto directory = goldens()[1];
    std::fill(directory.begin() + 34, directory.begin() + 38, std::uint8_t{0xff});
    const auto leverage = std::get<StockDirectory>(decode(directory).payload);
    TEST_ASSERT(leverage.etp_leverage_factor == std::numeric_limits<std::uint32_t>::max());
    // The raw integer is not assigned a later specification's fractional scale.
    const FixedText<8> full{{'A','B','C','D','E','F','G','H'}};
    const FixedText<8> blank{{' ',' ',' ',' ',' ',' ',' ',' '}};
    TEST_ASSERT(text_view(full) == "ABCDEFGH" && text_view(blank).empty());
}
void test_misaligned_inputs() {
    for (const auto& bytes : goldens()) {
        Bytes misaligned(bytes.size() + 1, 0);
        std::copy(bytes.begin(), bytes.end(), misaligned.begin() + 1);
        const auto value = Itch50Decoder::decode(misaligned.data() + 1, bytes.size());
        check_header(value, static_cast<char>(bytes[0]));
        TEST_ASSERT(value.payload.index() == decode(bytes).payload.index());
    }
}
void test_reject_bad_lengths_and_flags() {
    expect_error(ItchDecodeErrorCode::TRUNCATED_HEADER, [] {
        (void)Itch50Decoder::decode(nullptr, 0);
    });
    expect_error(ItchDecodeErrorCode::NULL_INPUT, [] {
        (void)Itch50Decoder::decode(nullptr, 11);
    });
    for (const auto& bytes : goldens()) {
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            // A shortened logical size over a full backing frame can hide an
            // accidental overread from ASan. Give each case its exact physical
            // allocation instead; size zero deliberately uses a null pointer.
            std::unique_ptr<std::uint8_t[]> exact;
            if (size != 0) {
                exact = std::make_unique<std::uint8_t[]>(size);
                std::copy_n(bytes.data(), size, exact.get());
            }
            for (const auto policy : {UnsupportedPolicy::REJECT, UnsupportedPolicy::REPORT_UNSUPPORTED}) {
                expect_error(size < Itch50Decoder::HEADER_SIZE ? ItchDecodeErrorCode::TRUNCATED_HEADER :
                             ItchDecodeErrorCode::INVALID_LENGTH, [&] {
                    (void)Itch50Decoder::decode(exact.get(), size, policy);
                });
            }
        }
        auto too_long = bytes;
        too_long.push_back(0);
        expect_error(ItchDecodeErrorCode::INVALID_LENGTH, [&] { (void)decode(too_long); });
        expect_error(ItchDecodeErrorCode::INVALID_LENGTH, [&] {
            (void)decode(too_long, UnsupportedPolicy::REPORT_UNSUPPORTED);
        });
    }
    for (const std::size_t index : {std::size_t{3}, std::size_t{4}, std::size_t{10}}) {
        auto invalid = goldens()[index];
        for (const std::uint8_t bad : {std::uint8_t{0}, std::uint8_t{' '}, std::uint8_t{'X'}, std::uint8_t{0xff}}) {
            invalid[19] = bad;
            expect_error(ItchDecodeErrorCode::INVALID_SIDE, [&] { (void)decode(invalid); });
        }
        for (const char valid : {'B', 'S'}) {
            invalid[19] = static_cast<std::uint8_t>(valid);
            (void)decode(invalid);
        }
    }
    auto execution = goldens()[6];
    for (const std::uint8_t bad : {std::uint8_t{0}, std::uint8_t{' '}, std::uint8_t{'X'}, std::uint8_t{0xff}}) {
        execution[31] = bad;
        expect_error(ItchDecodeErrorCode::INVALID_PRINTABLE, [&] { (void)decode(execution); });
    }
    for (const char valid : {'Y', 'N'}) {
        execution[31] = static_cast<std::uint8_t>(valid);
        TEST_ASSERT(std::get<OrderExecutedWithPrice>(decode(execution).payload).printable == valid);
    }
    try {
        execution.push_back(0);
        (void)decode(execution);
        TEST_ASSERT(false);
    } catch (const ItchDecodeError& error) {
        TEST_ASSERT(error.code() == ItchDecodeErrorCode::INVALID_LENGTH);
        TEST_ASSERT(error.type() == 'C' && error.length() == 37);
    }
}
void test_explicit_unsupported_coverage() {
    const std::array<char, 9> known{{'Y','L','V','W','K','J','h','I','N'}};
    const std::array<std::size_t, 9> sizes{{20,26,35,12,28,35,21,50,20}};
    for (std::size_t i = 0; i < known.size(); ++i) {
        TEST_ASSERT(Itch50Decoder::known_payload_length(known[i]) == sizes[i]);
        auto bytes = wire(known[i], {});
        bytes.resize(sizes[i], 0);
        const auto typed = decode(bytes);
        check_header(typed, known[i]);
        TEST_ASSERT(!std::holds_alternative<UnsupportedMessage>(typed.payload));
        const auto reported = decode(bytes, UnsupportedPolicy::REPORT_UNSUPPORTED);
        check_header(reported, known[i]);
        TEST_ASSERT(reported.payload.index() == typed.payload.index());
        bytes.push_back(0);
        expect_error(ItchDecodeErrorCode::INVALID_LENGTH, [&] {
            (void)decode(bytes, UnsupportedPolicy::REPORT_UNSUPPORTED);
        });
        bytes.resize(sizes[i] - 1);
        expect_error(ItchDecodeErrorCode::INVALID_LENGTH, [&] {
            (void)decode(bytes, UnsupportedPolicy::REPORT_UNSUPPORTED);
        });
    }
    // T was used in older ITCH versions, not an ITCH 5.0 timestamp message.
    for (const char unknown : {'T','Z','\0'}) {
        auto bytes = wire(unknown, {1,2,3});
        TEST_ASSERT(Itch50Decoder::known_payload_length(unknown) == 0);
        expect_error(ItchDecodeErrorCode::UNSUPPORTED_TYPE, [&] { (void)decode(bytes); });
        const auto reported = decode(bytes, UnsupportedPolicy::REPORT_UNSUPPORTED);
        const auto missing = std::get<UnsupportedMessage>(reported.payload);
        TEST_ASSERT(missing.type == unknown && missing.length == 14);
    }
}
void test_binaryfile_interoperation_every_split() {
    auto records = goldens();
    // ITCH S/C reports an application-level system event. It is still an
    // ordinary BinaryFILE payload and must not prevent framing a later record.
    records.push_back(wire('S', {'C'}));
    records.push_back(wire('D', {0,0,0,0,0,0,0,1}));
    Bytes framed;
    std::vector<std::uint64_t> offsets;
    for (const auto& bytes : records) {
        offsets.push_back(static_cast<std::uint64_t>(framed.size()));
        framed.push_back(0);
        framed.push_back(static_cast<std::uint8_t>(bytes.size()));
        framed.insert(framed.end(), bytes.begin(), bytes.end());
    }
    framed.push_back(0);
    framed.push_back(0); // BinaryFILE end, distinct from ITCH System Event C.
    for (std::size_t split = 0; split <= framed.size(); ++split) {
        BinaryFileReader reader;
        std::size_t received = 0;
        std::vector<ItchMessage> retained;
        const auto sink = [&](const BinaryRecordView& record) {
            TEST_ASSERT(received < records.size());
            TEST_ASSERT(record.ordinal == received + 1 && record.offset == offsets[received]);
            TEST_ASSERT(record.size == records[received].size());
            TEST_ASSERT(std::equal(record.data, record.data + record.size, records[received].begin()));
            retained.push_back(Itch50Decoder::decode(record.data, record.size));
            ++received;
        };
        reader.consume(framed.data(), split, sink);
        reader.consume(framed.data() + split, framed.size() - split, sink);
        TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        TEST_ASSERT(received == records.size());
        TEST_ASSERT(reader.records() == records.size() && reader.bytes_consumed() == framed.size());
        TEST_ASSERT(text_view(std::get<AddOrder>(retained[3].payload).stock) == "AAPL");
        TEST_ASSERT(std::get<SystemEvent>(retained[13].payload).event_code == 'C');
        TEST_ASSERT(std::get<OrderDelete>(retained[14].payload).order_reference == 1);
    }
    BinaryFileReader bytewise;
    std::size_t received = 0;
    for (const auto byte : framed) {
        bytewise.consume(&byte, 1, [&](const BinaryRecordView& record) {
            check_header(Itch50Decoder::decode(record.data, record.size), static_cast<char>(records[received][0]));
            ++received;
        });
    }
    TEST_ASSERT(bytewise.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(received == records.size());

    const std::string stream_bytes(framed.begin(), framed.end());
    std::istringstream stream(stream_bytes, std::ios::in | std::ios::binary);
    BinaryFileReader streamed;
    received = 0;
    const auto status = streamed.read(stream, [&](const BinaryRecordView& record) {
        TEST_ASSERT(received < records.size() && record.ordinal == received + 1);
        TEST_ASSERT(record.offset == offsets[received]);
        const auto message = Itch50Decoder::decode(record.data, record.size);
        check_header(message, static_cast<char>(records[received][0]));
        if (received == 13) TEST_ASSERT(std::get<SystemEvent>(message.payload).event_code == 'C');
        if (received == 14) TEST_ASSERT(std::get<OrderDelete>(message.payload).order_reference == 1);
        ++received;
    });
    TEST_ASSERT(status == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    TEST_ASSERT(received == records.size() && streamed.records() == records.size());
    TEST_ASSERT(streamed.bytes_consumed() == framed.size());
}
} // namespace

int main() {
    try {
        test_literal_goldens();
        test_full_width_zero_and_owned_values();
        test_misaligned_inputs();
        test_reject_bad_lengths_and_flags();
        test_explicit_unsupported_coverage();
        test_binaryfile_interoperation_every_split();
        std::cout << "ITCH 5.0 core decoder tests passed (synthetic only; 22 typed historical wire kinds)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH 5.0 decoder test failed: " << error.what() << '\n';
        return 1;
    }
}
