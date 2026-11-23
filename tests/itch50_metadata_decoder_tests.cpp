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
#include <type_traits>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;

// These bytes are wholly invented. Literal field bytes and separately stated
// expectations avoid a paired production encoder becoming the only oracle.
Bytes wire(char type, std::initializer_list<std::uint8_t> body) {
    Bytes bytes{static_cast<std::uint8_t>(type), 0x12, 0x34, 0x56, 0x78, 1, 2, 3, 4, 5, 6};
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}
std::vector<Bytes> goldens() {
    return {
        wire('Y', {'A','A','P','L',' ',' ',' ',' ', '2'}),
        wire('L', {'A','B','C','D', 'B','R','K','.','B',' ',' ',' ', 'Y','N','A'}),
        wire('V', {1,2,3,4,5,6,7,8, 0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
                   0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28}),
        wire('W', {'3'}),
        wire('K', {'I','P','O','.','A',' ',' ',' ', 0x31,0x32,0x33,0x34, 'C', 0x41,0x42,0x43,0x44}),
        wire('J', {'A','A','P','L',' ',' ',' ',' ', 0x51,0x52,0x53,0x54,
                   0x61,0x62,0x63,0x64, 0x71,0x72,0x73,0x74, 0x81,0x82,0x83,0x84}),
        wire('h', {'M','S','F','T',' ',' ',' ',' ', 'Q','H'}),
        wire('I', {0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,
                   0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8, 'S',
                   'N','V','D','A',' ',' ',' ',' ', 0xb1,0xb2,0xb3,0xb4,
                   0xc1,0xc2,0xc3,0xc4, 0xd1,0xd2,0xd3,0xd4, 'H','A'}),
        wire('N', {'T','S','L','A',' ',' ',' ',' ', 'B'})
    };
}
ItchMessage decode(const Bytes& bytes, UnsupportedPolicy policy = UnsupportedPolicy::REJECT) {
    return Itch50Decoder::decode(bytes.data(), bytes.size(), policy);
}
template<class Action>
void expect_error(ItchDecodeErrorCode code, Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const ItchDecodeError& error) { TEST_ASSERT(error.code() == code); caught = true; }
    TEST_ASSERT(caught);
}
void check_header(const ItchMessage& message, char type) {
    TEST_ASSERT(message.header.type == type);
    TEST_ASSERT(message.header.stock_locate == 0x1234 && message.header.tracking_number == 0x5678);
    TEST_ASSERT(message.header.nanoseconds_since_midnight == 0x010203040506ULL);
}
void check_literal_fields(const std::vector<ItchMessage>& values) {
    TEST_ASSERT(values.size() == 9);
    const auto& sho = std::get<RegShoRestriction>(values[0].payload);
    TEST_ASSERT(text_view(sho.stock) == "AAPL" && sho.action == '2');
    const auto& participant = std::get<MarketParticipantPosition>(values[1].payload);
    TEST_ASSERT(text_view(participant.mpid) == "ABCD" && text_view(participant.stock) == "BRK.B");
    TEST_ASSERT(participant.primary_market_maker == 'Y' && participant.market_maker_mode == 'N');
    TEST_ASSERT(participant.market_participant_state == 'A');
    const auto& levels = std::get<MwcbDeclineLevels>(values[2].payload);
    TEST_ASSERT(levels.level1 == 0x0102030405060708ULL && levels.level2 == 0x1112131415161718ULL);
    TEST_ASSERT(levels.level3 == 0x2122232425262728ULL);
    TEST_ASSERT(std::get<MwcbStatus>(values[3].payload).breached_level == '3');
    const auto& ipo = std::get<IpoQuotingPeriodUpdate>(values[4].payload);
    TEST_ASSERT(text_view(ipo.stock) == "IPO.A" && ipo.quotation_release_time == 0x31323334U);
    TEST_ASSERT(ipo.release_qualifier == 'C' && ipo.ipo_price == 0x41424344U);
    const auto& collar = std::get<LuldAuctionCollar>(values[5].payload);
    TEST_ASSERT(text_view(collar.stock) == "AAPL" && collar.reference_price == 0x51525354U);
    TEST_ASSERT(collar.upper_price == 0x61626364U && collar.lower_price == 0x71727374U);
    TEST_ASSERT(collar.extension == 0x81828384U);
    const auto& halt = std::get<OperationalHalt>(values[6].payload);
    TEST_ASSERT(text_view(halt.stock) == "MSFT" && halt.market_code == 'Q' && halt.action == 'H');
    const auto& imbalance = std::get<NetOrderImbalance>(values[7].payload);
    TEST_ASSERT(imbalance.paired_shares == 0x9192939495969798ULL);
    TEST_ASSERT(imbalance.imbalance_shares == 0xa1a2a3a4a5a6a7a8ULL);
    TEST_ASSERT(imbalance.imbalance_direction == 'S' && text_view(imbalance.stock) == "NVDA");
    TEST_ASSERT(imbalance.far_price == 0xb1b2b3b4U && imbalance.near_price == 0xc1c2c3c4U);
    TEST_ASSERT(imbalance.current_reference_price == 0xd1d2d3d4U);
    TEST_ASSERT(imbalance.cross_type == 'H' && imbalance.price_variation_indicator == 'A');
    const auto& retail = std::get<RetailPriceImprovement>(values[8].payload);
    TEST_ASSERT(text_view(retail.stock) == "TSLA" && retail.interest == 'B');
}
void test_literal_layouts_misalignment_and_ownership() {
    static_assert(std::is_same_v<std::variant_alternative_t<0, ItchPayload>, SystemEvent>);
    static_assert(std::is_same_v<std::variant_alternative_t<11, ItchPayload>, BrokenTrade>);
    static_assert(std::is_same_v<std::variant_alternative_t<12, ItchPayload>, RegShoRestriction>);
    static_assert(std::is_same_v<std::variant_alternative_t<20, ItchPayload>, RetailPriceImprovement>);
    const std::array<std::size_t, 9> sizes{{20,26,35,12,28,35,21,50,20}};
    auto bytes = goldens();
    std::vector<ItchMessage> direct;
    std::vector<ItchMessage> misaligned;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char type = static_cast<char>(bytes[i][0]);
        TEST_ASSERT(bytes[i].size() == sizes[i] && Itch50Decoder::known_payload_length(type) == sizes[i]);
        direct.push_back(decode(bytes[i]));
        check_header(direct.back(), type);
        TEST_ASSERT(direct.back().payload.index() == i + 12);
        auto shifted = std::make_unique<std::uint8_t[]>(bytes[i].size() + 1);
        std::copy(bytes[i].begin(), bytes[i].end(), shifted.get() + 1);
        misaligned.push_back(Itch50Decoder::decode(shifted.get() + 1, bytes[i].size()));
        check_header(misaligned.back(), type);
        std::fill_n(shifted.get(), bytes[i].size() + 1, std::uint8_t{0});
        std::fill(bytes[i].begin(), bytes[i].end(), std::uint8_t{0});
    }
    check_literal_fields(direct); // Input buffers no longer contain the fields.
    check_literal_fields(misaligned); // Also check every payload field, not just variant index.
}
template<std::size_t N>
void check_text_fill(const FixedText<N>& text, char byte) {
    TEST_ASSERT(std::all_of(text.begin(), text.end(), [byte](char value) { return value == byte; }));
}
void check_raw_extremes(const std::vector<ItchMessage>& values, std::uint64_t wide,
                        std::uint32_t narrow, char byte) {
    const auto& sho = std::get<RegShoRestriction>(values[0].payload);
    check_text_fill(sho.stock, byte); TEST_ASSERT(sho.action == byte);
    const auto& participant = std::get<MarketParticipantPosition>(values[1].payload);
    check_text_fill(participant.mpid, byte); check_text_fill(participant.stock, byte);
    TEST_ASSERT(participant.primary_market_maker == byte && participant.market_maker_mode == byte);
    TEST_ASSERT(participant.market_participant_state == byte);
    const auto& levels = std::get<MwcbDeclineLevels>(values[2].payload);
    TEST_ASSERT(levels.level1 == wide && levels.level2 == wide && levels.level3 == wide);
    TEST_ASSERT(std::get<MwcbStatus>(values[3].payload).breached_level == byte);
    const auto& ipo = std::get<IpoQuotingPeriodUpdate>(values[4].payload);
    check_text_fill(ipo.stock, byte);
    TEST_ASSERT(ipo.quotation_release_time == narrow && ipo.release_qualifier == byte && ipo.ipo_price == narrow);
    const auto& collar = std::get<LuldAuctionCollar>(values[5].payload);
    check_text_fill(collar.stock, byte);
    TEST_ASSERT(collar.reference_price == narrow && collar.upper_price == narrow);
    TEST_ASSERT(collar.lower_price == narrow && collar.extension == narrow);
    const auto& halt = std::get<OperationalHalt>(values[6].payload);
    check_text_fill(halt.stock, byte); TEST_ASSERT(halt.market_code == byte && halt.action == byte);
    const auto& imbalance = std::get<NetOrderImbalance>(values[7].payload);
    TEST_ASSERT(imbalance.paired_shares == wide && imbalance.imbalance_shares == wide);
    TEST_ASSERT(imbalance.imbalance_direction == byte); check_text_fill(imbalance.stock, byte);
    TEST_ASSERT(imbalance.far_price == narrow && imbalance.near_price == narrow);
    TEST_ASSERT(imbalance.current_reference_price == narrow && imbalance.cross_type == byte);
    TEST_ASSERT(imbalance.price_variation_indicator == byte);
    const auto& retail = std::get<RetailPriceImprovement>(values[8].payload);
    check_text_fill(retail.stock, byte); TEST_ASSERT(retail.interest == byte);
}
void test_full_width_zero_and_raw_codes() {
    for (const auto fill : {std::uint8_t{0}, std::uint8_t{0xff}}) {
        auto records = goldens();
        std::vector<ItchMessage> retained;
        for (auto& bytes : records) {
            const char type = static_cast<char>(bytes[0]);
            std::fill(bytes.begin() + 1, bytes.end(), fill);
            retained.push_back(decode(bytes));
            TEST_ASSERT(retained.back().header.type == type);
            const auto expected_header = fill == 0 ? std::uint16_t{0} : std::numeric_limits<std::uint16_t>::max();
            TEST_ASSERT(retained.back().header.stock_locate == expected_header);
            TEST_ASSERT(retained.back().header.tracking_number == expected_header);
            TEST_ASSERT(retained.back().header.nanoseconds_since_midnight == (fill == 0 ? 0 : 0xffffffffffffULL));
        }
        check_raw_extremes(retained, fill == 0 ? 0 : std::numeric_limits<std::uint64_t>::max(),
            fill == 0 ? 0 : std::numeric_limits<std::uint32_t>::max(), static_cast<char>(fill));
    }
    // The decoder intentionally preserves future/invalid business codes; the
    // session validator, not framing, decides whether they fit its profile.
    auto records = goldens();
    records[0][19] = '?'; records[3][11] = '?'; records[6][20] = '?';
    TEST_ASSERT(std::get<RegShoRestriction>(decode(records[0]).payload).action == '?');
    TEST_ASSERT(std::get<MwcbStatus>(decode(records[3]).payload).breached_level == '?');
    TEST_ASSERT(std::get<OperationalHalt>(decode(records[6]).payload).action == '?');
}
void test_exact_lengths_and_physically_short_inputs() {
    for (const auto& bytes : goldens()) {
        const char type = static_cast<char>(bytes[0]);
        for (std::size_t size = 0; size < bytes.size(); ++size) {
            std::unique_ptr<std::uint8_t[]> short_input;
            if (size != 0) {
                short_input = std::make_unique<std::uint8_t[]>(size);
                std::copy_n(bytes.data(), size, short_input.get());
            }
            for (const auto policy : {UnsupportedPolicy::REJECT, UnsupportedPolicy::REPORT_UNSUPPORTED}) {
                expect_error(size < Itch50Decoder::HEADER_SIZE ? ItchDecodeErrorCode::TRUNCATED_HEADER :
                             ItchDecodeErrorCode::INVALID_LENGTH, [&] {
                    (void)Itch50Decoder::decode(short_input.get(), size, policy);
                });
            }
        }
        auto excess = bytes;
        excess.push_back(0);
        for (const auto policy : {UnsupportedPolicy::REJECT, UnsupportedPolicy::REPORT_UNSUPPORTED}) {
            expect_error(ItchDecodeErrorCode::INVALID_LENGTH, [&] { (void)decode(excess, policy); });
            try {
                (void)decode(excess, policy);
                TEST_ASSERT(false);
            } catch (const ItchDecodeError& error) {
                TEST_ASSERT(error.type() == type && error.length() == bytes.size() + 1);
            }
        }
    }
    expect_error(ItchDecodeErrorCode::NULL_INPUT, [] { (void)Itch50Decoder::decode(nullptr, 50); });
    for (const char unknown : {'T','Z',static_cast<char>(0xff)}) {
        auto bytes = wire(unknown, {1,2,3});
        expect_error(ItchDecodeErrorCode::UNSUPPORTED_TYPE, [&] { (void)decode(bytes); });
        const auto report = decode(bytes, UnsupportedPolicy::REPORT_UNSUPPORTED);
        const auto& value = std::get<UnsupportedMessage>(report.payload);
        TEST_ASSERT(value.type == unknown && value.length == bytes.size());
    }
}
void test_binaryfile_interoperation() {
    const auto records = goldens();
    Bytes framed;
    std::vector<std::uint64_t> offsets;
    for (const auto& bytes : records) {
        offsets.push_back(static_cast<std::uint64_t>(framed.size()));
        framed.push_back(0);
        framed.push_back(static_cast<std::uint8_t>(bytes.size()));
        framed.insert(framed.end(), bytes.begin(), bytes.end());
    }
    framed.push_back(0); framed.push_back(0);
    for (std::size_t split = 0; split <= framed.size(); ++split) {
        BinaryFileReader reader;
        std::vector<ItchMessage> values;
        const auto sink = [&](const BinaryRecordView& record) {
            TEST_ASSERT(values.size() < records.size());
            TEST_ASSERT(record.ordinal == values.size() + 1 && record.offset == offsets[values.size()]);
            values.push_back(Itch50Decoder::decode(record.data, record.size));
        };
        reader.consume(framed.data(), split, sink);
        reader.consume(framed.data() + split, framed.size() - split, sink);
        TEST_ASSERT(reader.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
        check_literal_fields(values);
    }
    BinaryFileReader bytewise;
    std::vector<ItchMessage> values;
    for (const auto byte : framed) {
        bytewise.consume(&byte, 1, [&](const BinaryRecordView& record) {
            values.push_back(Itch50Decoder::decode(record.data, record.size));
        });
    }
    TEST_ASSERT(bytewise.finish() == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    check_literal_fields(values);
    std::istringstream input(std::string(framed.begin(), framed.end()), std::ios::in | std::ios::binary);
    BinaryFileReader streamed;
    values.clear();
    TEST_ASSERT(streamed.read(input, [&](const BinaryRecordView& record) {
        values.push_back(Itch50Decoder::decode(record.data, record.size));
    }) == CompletionStatus::COMPLETE_WITH_TERMINATOR);
    check_literal_fields(values);
}
} // namespace

int main() {
    try {
        test_literal_layouts_misalignment_and_ownership();
        test_full_width_zero_and_raw_codes();
        test_exact_lengths_and_physically_short_inputs();
        test_binaryfile_interoperation();
        std::cout << "ITCH 5.0 historical metadata decoder tests passed (synthetic bytes only; nine metadata kinds)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ITCH metadata decoder test failed: " << error.what() << '\n';
        return 1;
    }
}
