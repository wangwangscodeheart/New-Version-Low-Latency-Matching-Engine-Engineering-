#ifndef FEED_DECODER_HPP
#define FEED_DECODER_HPP

#include "instrument_id.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <variant>
#include <vector>
#include <utility>

enum class FeedMessageType : uint16_t { ADD_ORDER = 1, CANCEL_ORDER = 2, TRADE = 3 };

struct FeedHeader {
    FeedMessageType type;
    uint16_t body_length;
    InstrumentId instrument_id;
    uint64_t sequence;
    Timestamp exchange_timestamp;
};
struct FeedAddOrder { uint64_t order_id; Price price; Quantity quantity; Side side; };
struct FeedCancelOrder { uint64_t order_id; };
struct FeedTrade {
    uint64_t trade_id;
    uint64_t passive_order_id;
    Price price;
    Quantity quantity;
};
using FeedPayload = std::variant<FeedAddOrder, FeedCancelOrder, FeedTrade>;
struct FeedMessage { FeedHeader header; FeedPayload payload; };

enum class FeedDecodeError : uint8_t {
    INVALID_LENGTH, UNKNOWN_TYPE, INVALID_SIDE, INCOMPLETE_TAIL
};

class FeedDecodeException : public std::runtime_error {
    FeedDecodeError error_;
public:
    FeedDecodeException(FeedDecodeError error, const char* message)
        : std::runtime_error(message), error_(error) {}
    FeedDecodeError error() const noexcept { return error_; }
};

class FeedDecoder {
    static constexpr size_t HEADER_SIZE = 24;
    std::vector<uint8_t> pending_;

    static uint16_t u16(const uint8_t* p) noexcept {
        return static_cast<uint16_t>((uint16_t(p[0]) << 8) | uint16_t(p[1]));
    }
    static uint32_t u32(const uint8_t* p) noexcept {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    static uint64_t u64(const uint8_t* p) noexcept {
        uint64_t result = 0;
        for (size_t i = 0; i < 8; ++i) result = (result << 8) | p[i];
        return result;
    }

    static size_t expected_length(FeedMessageType type) {
        switch (type) {
            case FeedMessageType::ADD_ORDER: return 25;
            case FeedMessageType::CANCEL_ORDER: return 8;
            case FeedMessageType::TRADE: return 32;
        }
        throw FeedDecodeException(FeedDecodeError::UNKNOWN_TYPE,
                                  "unknown feed message type");
    }

    static FeedMessage decode_one(const uint8_t* frame) {
        const auto type = static_cast<FeedMessageType>(u16(frame));
        const uint16_t body_length = u16(frame + 2);
        if (body_length != expected_length(type)) {
            throw FeedDecodeException(FeedDecodeError::INVALID_LENGTH,
                                      "invalid feed body length");
        }
        FeedHeader header{type, body_length, InstrumentId(u32(frame + 4)),
                          u64(frame + 8), Timestamp(u64(frame + 16))};
        const uint8_t* body = frame + HEADER_SIZE;
        if (type == FeedMessageType::ADD_ORDER) {
            if (body[24] > 1) {
                throw FeedDecodeException(FeedDecodeError::INVALID_SIDE,
                                          "invalid feed side");
            }
            return FeedMessage{header, FeedAddOrder{u64(body),
                Price(static_cast<int64_t>(u64(body + 8))), Quantity(u64(body + 16)),
                body[24] == 0 ? Side::BUY : Side::SELL}};
        }
        if (type == FeedMessageType::CANCEL_ORDER) {
            return FeedMessage{header, FeedCancelOrder{u64(body)}};
        }
        return FeedMessage{header, FeedTrade{u64(body), u64(body + 8),
            Price(static_cast<int64_t>(u64(body + 16))), Quantity(u64(body + 24))}};
    }

public:
    template<typename Sink>
    void consume(const uint8_t* data, size_t size, Sink&& sink) {
        pending_.insert(pending_.end(), data, data + size);
        size_t offset = 0;
        while (pending_.size() - offset >= HEADER_SIZE) {
            const uint16_t body_length = u16(pending_.data() + offset + 2);
            const auto type = static_cast<FeedMessageType>(u16(pending_.data() + offset));
            if (body_length != expected_length(type)) {
                throw FeedDecodeException(FeedDecodeError::INVALID_LENGTH,
                                          "invalid feed body length");
            }
            const size_t frame_size = HEADER_SIZE + body_length;
            if (pending_.size() - offset < frame_size) break;
            sink(decode_one(pending_.data() + offset));
            offset += frame_size;
        }
        if (offset != 0) pending_.erase(pending_.begin(), pending_.begin() + offset);
    }

    template<typename Sink>
    void consume(const std::vector<uint8_t>& bytes, Sink&& sink) {
        consume(bytes.data(), bytes.size(), std::forward<Sink>(sink));
    }
    void finish() {
        if (!pending_.empty()) {
            throw FeedDecodeException(FeedDecodeError::INCOMPLETE_TAIL,
                                      "incomplete feed tail");
        }
    }
    size_t pending_bytes() const noexcept { return pending_.size(); }
};

#endif
