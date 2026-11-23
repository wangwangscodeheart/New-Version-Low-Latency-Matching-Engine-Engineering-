#ifndef ITCH50_DECODER_HPP
#define ITCH50_DECODER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <variant>

namespace nasdaq_itch {

// ITCH 5.0 common header. Neither the tracking number nor timestamp is a
// transport sequence number. The six-byte timestamp is nanoseconds since
// midnight (not Unix time); the trading date/session is supplied out of band.
struct ItchHeader {
    char type;
    std::uint16_t stock_locate;
    std::uint16_t tracking_number;
    std::uint64_t nanoseconds_since_midnight;
};

template<std::size_t N>
using FixedText = std::array<char, N>;

// Fixed-width protocol text is owned by the decoded value. Preserve punctuation
// and remove only right-padding spaces, never assume a NUL terminator.
template<std::size_t N>
std::string_view text_view(const FixedText<N>& text) noexcept {
    std::size_t length = N;
    while (length != 0 && text[length - 1] == ' ') --length;
    return {text.data(), length};
}

struct SystemEvent { char event_code; };
struct StockDirectory {
    FixedText<8> stock;
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    FixedText<2> issue_sub_type;
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_reference_price_tier;
    char etp_flag;
    std::uint32_t etp_leverage_factor;
    char inverse_indicator;
};
struct StockTradingAction {
    FixedText<8> stock;
    char trading_state;
    char reserved;
    FixedText<4> reason;
};
struct AddOrder {
    std::uint64_t order_reference;
    char side; // B or S, independent of the matching engine's Side enum.
    std::uint32_t shares;
    FixedText<8> stock;
    std::uint32_t price; // Integer price with four implied decimal places.
    std::optional<FixedText<4>> attribution; // Present only for wire type F.
};
struct OrderExecuted {
    std::uint64_t order_reference;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
};
struct OrderExecutedWithPrice {
    std::uint64_t order_reference;
    std::uint32_t executed_shares;
    std::uint64_t match_number;
    char printable;
    // May differ from the resting price. Quantity is depleted at the original
    // order's price, even when printable == N; this is not a replace command.
    std::uint32_t execution_price;
};
// X subtracts cancelled_shares; D removes the entire remaining order. Do not
// collapse these distinct public events into the simulated feed's full cancel.
struct OrderCancel { std::uint64_t order_reference; std::uint32_t cancelled_shares; };
struct OrderDelete { std::uint64_t order_reference; };
// U inherits stock and side from the old order, replaces its identity, and sets
// the new remaining quantity. It is not an independently routable new order.
struct OrderReplace {
    std::uint64_t original_order_reference;
    std::uint64_t new_order_reference;
    std::uint32_t shares;
    std::uint32_t price;
};
// P describes a non-displayed trade, not an Add/Execute for the displayed book.
// Preserve its raw reference (historically zero) and side; the side is NOT an
// inferred aggressor direction. Q and B likewise are not displayed mutations.
struct NonDisplayedTrade {
    std::uint64_t order_reference;
    char side;
    std::uint32_t shares;
    FixedText<8> stock;
    std::uint32_t price;
    std::uint64_t match_number;
};
struct CrossTrade {
    std::uint64_t shares;
    FixedText<8> stock;
    std::uint32_t cross_price;
    std::uint64_t match_number;
    char cross_type;
};
// A broken trade does not restore displayed quantity; it is a separate trade
// reporting concern and must not be interpreted as an inverse execution.
struct BrokenTrade { std::uint64_t match_number; };
// Metadata layouts for the 2020-targeted historical profile. Numeric/code
// values remain raw; profile and cross-message validation belong to the
// receiver, not the structural decoder. None are displayed-order mutations.
struct RegShoRestriction { FixedText<8> stock; char action; };
struct MarketParticipantPosition {
    FixedText<4> mpid;
    FixedText<8> stock;
    char primary_market_maker;
    char market_maker_mode;
    char market_participant_state;
};
// Price(8) integers, with eight implied decimal places. Do not convert these
// market-wide levels into Price(4), timestamps, doubles or order quantities.
struct MwcbDeclineLevels { std::uint64_t level1, level2, level3; };
struct MwcbStatus { char breached_level; };
struct IpoQuotingPeriodUpdate {
    FixedText<8> stock;
    std::uint32_t quotation_release_time; // Seconds since midnight, not u48 nanoseconds.
    char release_qualifier;
    std::uint32_t ipo_price;
};
struct LuldAuctionCollar {
    FixedText<8> stock;
    std::uint32_t reference_price, upper_price, lower_price, extension;
};
struct OperationalHalt { FixedText<8> stock; char market_code; char action; };
struct NetOrderImbalance {
    std::uint64_t paired_shares, imbalance_shares;
    char imbalance_direction;
    FixedText<8> stock;
    std::uint32_t far_price, near_price, current_reference_price;
    char cross_type;
    char price_variation_indicator;
};
struct RetailPriceImprovement { FixedText<8> stock; char interest; };
struct UnsupportedMessage { char type; std::size_t length; };

using ItchPayload = std::variant<SystemEvent, StockDirectory, StockTradingAction,
    AddOrder, OrderExecuted, OrderExecutedWithPrice, OrderCancel, OrderDelete,
    OrderReplace, NonDisplayedTrade, CrossTrade, BrokenTrade, RegShoRestriction,
    MarketParticipantPosition, MwcbDeclineLevels, MwcbStatus, IpoQuotingPeriodUpdate,
    LuldAuctionCollar, OperationalHalt, NetOrderImbalance, RetailPriceImprovement,
    UnsupportedMessage>;
struct ItchMessage { ItchHeader header; ItchPayload payload; };

enum class UnsupportedPolicy { REJECT, REPORT_UNSUPPORTED };
enum class ItchDecodeErrorCode {
    NULL_INPUT, TRUNCATED_HEADER, INVALID_LENGTH, UNSUPPORTED_TYPE,
    INVALID_SIDE, INVALID_PRINTABLE
};
class ItchDecodeError : public std::runtime_error {
public:
    ItchDecodeError(ItchDecodeErrorCode code, const char* reason,
                    char type, std::size_t length)
        : std::runtime_error(reason), code_(code), type_(type), length_(length) {}
    ItchDecodeErrorCode code() const noexcept { return code_; }
    char type() const noexcept { return type_; }
    std::size_t length() const noexcept { return length_; }
private:
    ItchDecodeErrorCode code_;
    char type_;
    std::size_t length_;
};

// Structural decoder of 22 historical ITCH 5.0 wire kinds: the 2015 baseline
// plus the pre-2020 J/h additions. This is NOT a claim of full business-profile
// validation or of validation against a real 2020 file. Default policy rejects
// unknown types; reporting mode explicitly
// surfaces coverage gaps and must not certify a complete reconstructed book.
// Numeric zero/full-width values and metadata codes are preserved for later
// business/profile validation. No message is fed into the matching engine.
class Itch50Decoder {
    static std::uint16_t u16(const std::uint8_t* bytes) noexcept {
        return static_cast<std::uint16_t>(
            (std::uint16_t{bytes[0]} << 8) | std::uint16_t{bytes[1]});
    }
    static std::uint32_t u32(const std::uint8_t* bytes) noexcept {
        return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
               (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
    }
    template<std::size_t N>
    static std::uint64_t unsigned_integer(const std::uint8_t* bytes) noexcept {
        static_assert(N <= 8, "integer width exceeds uint64_t");
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < N; ++i) value = (value << 8) | bytes[i];
        return value;
    }
    template<std::size_t N>
    static FixedText<N> text(const std::uint8_t* bytes) noexcept {
        FixedText<N> result{};
        for (std::size_t i = 0; i < N; ++i) result[i] = static_cast<char>(bytes[i]);
        return result;
    }
    static char side(const std::uint8_t* bytes, char type, std::size_t size) {
        const char value = static_cast<char>(bytes[19]);
        if (value != 'B' && value != 'S') {
            throw ItchDecodeError(ItchDecodeErrorCode::INVALID_SIDE,
                "ITCH side must be B or S", type, size);
        }
        return value;
    }
public:
    static constexpr std::size_t HEADER_SIZE = 11;

    // Sizes include the type byte and exclude the two-byte BinaryFILE prefix.
    // All 22 known historical kinds below have typed decoding branches.
    static constexpr std::size_t known_payload_length(char type) noexcept {
        switch (type) {
            case 'S': return 12;
            case 'R': return 39;
            case 'H': return 25;
            case 'Y': return 20;
            case 'L': return 26;
            case 'V': return 35;
            case 'W': return 12;
            case 'K': return 28;
            case 'J': return 35;
            case 'h': return 21;
            case 'A': return 36;
            case 'F': return 40;
            case 'E': return 31;
            case 'C': return 36;
            case 'X': return 23;
            case 'D': return 19;
            case 'U': return 35;
            case 'P': return 44;
            case 'Q': return 40;
            case 'B': return 19;
            case 'I': return 50;
            case 'N': return 20;
            default: return 0;
        }
    }

    static ItchMessage decode(const std::uint8_t* bytes, std::size_t size,
                             UnsupportedPolicy policy = UnsupportedPolicy::REJECT) {
        if (bytes == nullptr && size != 0) {
            throw ItchDecodeError(ItchDecodeErrorCode::NULL_INPUT,
                "ITCH input pointer is null", '\0', size);
        }
        if (size < HEADER_SIZE) {
            throw ItchDecodeError(ItchDecodeErrorCode::TRUNCATED_HEADER,
                "ITCH common header is incomplete", size == 0 ? '\0' : static_cast<char>(bytes[0]), size);
        }
        const char type = static_cast<char>(bytes[0]);
        const std::size_t expected = known_payload_length(type);
        if (expected != 0 && expected != size) {
            throw ItchDecodeError(ItchDecodeErrorCode::INVALID_LENGTH,
                "ITCH payload length differs from the known historical layout", type, size);
        }
        const ItchHeader header{type, u16(bytes + 1), u16(bytes + 3),
                                unsigned_integer<6>(bytes + 5)};
        switch (type) {
            case 'S': return {header, SystemEvent{static_cast<char>(bytes[11])}};
            case 'R': return {header, StockDirectory{
                text<8>(bytes + 11), static_cast<char>(bytes[19]), static_cast<char>(bytes[20]),
                u32(bytes + 21), static_cast<char>(bytes[25]), static_cast<char>(bytes[26]),
                text<2>(bytes + 27), static_cast<char>(bytes[29]), static_cast<char>(bytes[30]),
                static_cast<char>(bytes[31]), static_cast<char>(bytes[32]), static_cast<char>(bytes[33]),
                u32(bytes + 34), static_cast<char>(bytes[38])}};
            case 'H': return {header, StockTradingAction{text<8>(bytes + 11),
                static_cast<char>(bytes[19]), static_cast<char>(bytes[20]), text<4>(bytes + 21)}};
            case 'A':
            case 'F': {
                AddOrder order{unsigned_integer<8>(bytes + 11), side(bytes, type, size),
                    u32(bytes + 20), text<8>(bytes + 24), u32(bytes + 32), std::nullopt};
                if (type == 'F') order.attribution = text<4>(bytes + 36);
                return {header, order};
            }
            case 'E': return {header, OrderExecuted{unsigned_integer<8>(bytes + 11),
                u32(bytes + 19), unsigned_integer<8>(bytes + 23)}};
            case 'C': {
                const char printable = static_cast<char>(bytes[31]);
                if (printable != 'Y' && printable != 'N') {
                    throw ItchDecodeError(ItchDecodeErrorCode::INVALID_PRINTABLE,
                        "ITCH execution printable flag must be Y or N", type, size);
                }
                return {header, OrderExecutedWithPrice{unsigned_integer<8>(bytes + 11),
                    u32(bytes + 19), unsigned_integer<8>(bytes + 23), printable, u32(bytes + 32)}};
            }
            case 'X': return {header, OrderCancel{unsigned_integer<8>(bytes + 11), u32(bytes + 19)}};
            case 'D': return {header, OrderDelete{unsigned_integer<8>(bytes + 11)}};
            case 'U': return {header, OrderReplace{unsigned_integer<8>(bytes + 11),
                unsigned_integer<8>(bytes + 19), u32(bytes + 27), u32(bytes + 31)}};
            case 'P': return {header, NonDisplayedTrade{unsigned_integer<8>(bytes + 11),
                side(bytes, type, size), u32(bytes + 20), text<8>(bytes + 24),
                u32(bytes + 32), unsigned_integer<8>(bytes + 36)}};
            case 'Q': return {header, CrossTrade{unsigned_integer<8>(bytes + 11),
                text<8>(bytes + 19), u32(bytes + 27), unsigned_integer<8>(bytes + 31),
                static_cast<char>(bytes[39])}};
            case 'B': return {header, BrokenTrade{unsigned_integer<8>(bytes + 11)}};
            case 'Y': return {header, RegShoRestriction{text<8>(bytes + 11),
                static_cast<char>(bytes[19])}};
            case 'L': return {header, MarketParticipantPosition{text<4>(bytes + 11),
                text<8>(bytes + 15), static_cast<char>(bytes[23]),
                static_cast<char>(bytes[24]), static_cast<char>(bytes[25])}};
            case 'V': return {header, MwcbDeclineLevels{unsigned_integer<8>(bytes + 11),
                unsigned_integer<8>(bytes + 19), unsigned_integer<8>(bytes + 27)}};
            case 'W': return {header, MwcbStatus{static_cast<char>(bytes[11])}};
            case 'K': return {header, IpoQuotingPeriodUpdate{text<8>(bytes + 11),
                u32(bytes + 19), static_cast<char>(bytes[23]), u32(bytes + 24)}};
            case 'J': return {header, LuldAuctionCollar{text<8>(bytes + 11),
                u32(bytes + 19), u32(bytes + 23), u32(bytes + 27), u32(bytes + 31)}};
            case 'h': return {header, OperationalHalt{text<8>(bytes + 11),
                static_cast<char>(bytes[19]), static_cast<char>(bytes[20])}};
            case 'I': return {header, NetOrderImbalance{unsigned_integer<8>(bytes + 11),
                unsigned_integer<8>(bytes + 19), static_cast<char>(bytes[27]),
                text<8>(bytes + 28), u32(bytes + 36), u32(bytes + 40), u32(bytes + 44),
                static_cast<char>(bytes[48]), static_cast<char>(bytes[49])}};
            case 'N': return {header, RetailPriceImprovement{text<8>(bytes + 11),
                static_cast<char>(bytes[19])}};
            default:
                if (policy == UnsupportedPolicy::REPORT_UNSUPPORTED) {
                    return {header, UnsupportedMessage{type, size}};
                }
                throw ItchDecodeError(ItchDecodeErrorCode::UNSUPPORTED_TYPE,
                    "ITCH payload type is outside the implemented decoder coverage", type, size);
        }
    }
};

} // namespace nasdaq_itch

#endif
