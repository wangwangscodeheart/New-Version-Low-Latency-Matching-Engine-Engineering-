#ifndef FEED_ENCODER_HPP
#define FEED_ENCODER_HPP

#include "feed_decoder.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>

struct EncodedFeedFrame {
    static constexpr size_t CAPACITY = 56;
    std::array<uint8_t, CAPACITY> bytes{};
    size_t length = 0;
    const uint8_t* data() const noexcept { return bytes.data(); }
    size_t size() const noexcept { return length; }
};

// Stateless producer-side codec for the existing simulated feed layout.
// It does not allocate order/trade IDs, sequences, timestamps, or publish data.
// Successful encoding uses fixed frame storage; error exceptions may allocate.
class FeedEncoder {
    static void put(EncodedFeedFrame& frame, size_t offset,
                     uint64_t value, size_t width) noexcept {
        for (size_t i = 0; i < width; ++i) {
            frame.bytes[offset + i] = static_cast<uint8_t>(value >> ((width - i - 1) * 8));
        }
    }

public:
    static constexpr size_t HEADER_SIZE = 24;

    static EncodedFeedFrame encode(const FeedMessage& message) {
        const FeedHeader& header = message.header;
        if (!header.instrument_id.valid()) throw std::invalid_argument("Invalid feed instrument");
        if (header.sequence == 0) throw std::invalid_argument("Feed sequence must be nonzero");

        size_t body_length = 0;
        switch (header.type) {
            case FeedMessageType::ADD_ORDER: body_length = 25; break;
            case FeedMessageType::CANCEL_ORDER: body_length = 8; break;
            case FeedMessageType::TRADE: body_length = 32; break;
            default: throw std::invalid_argument("Unknown feed message type");
        }
        if (header.body_length != body_length) throw std::invalid_argument("Invalid feed body length");
        const auto* add = std::get_if<FeedAddOrder>(&message.payload);
        const auto* cancel = std::get_if<FeedCancelOrder>(&message.payload);
        const auto* trade = std::get_if<FeedTrade>(&message.payload);
        if ((header.type == FeedMessageType::ADD_ORDER && !add) ||
            (header.type == FeedMessageType::CANCEL_ORDER && !cancel) ||
            (header.type == FeedMessageType::TRADE && !trade)) {
            throw std::invalid_argument("Feed type/payload mismatch");
        }
        if (add) {
            if (add->order_id == 0 || add->price.get() <= 0 || add->quantity.get() == 0 ||
                (add->side != Side::BUY && add->side != Side::SELL)) {
                throw std::invalid_argument("Invalid feed Add fields");
            }
        } else if (cancel) {
            if (cancel->order_id == 0) throw std::invalid_argument("Invalid feed Cancel order reference");
        } else if (trade->trade_id == 0 || trade->passive_order_id == 0 ||
                   trade->price.get() <= 0 || trade->quantity.get() == 0) {
            throw std::invalid_argument("Invalid feed Trade fields");
        }

        EncodedFeedFrame frame;
        frame.length = HEADER_SIZE + body_length;
        put(frame, 0, static_cast<uint16_t>(header.type), 2);
        put(frame, 2, header.body_length, 2);
        put(frame, 4, header.instrument_id.get(), 4);
        put(frame, 8, header.sequence, 8);
        put(frame, 16, header.exchange_timestamp.get(), 8);
        if (add) {
            put(frame, 24, add->order_id, 8);
            put(frame, 32, static_cast<uint64_t>(add->price.get()), 8);
            put(frame, 40, add->quantity.get(), 8);
            put(frame, 48, add->side == Side::BUY ? 0 : 1, 1);
        } else if (cancel) {
            put(frame, 24, cancel->order_id, 8);
        } else {
            put(frame, 24, trade->trade_id, 8);
            put(frame, 32, trade->passive_order_id, 8);
            put(frame, 40, static_cast<uint64_t>(trade->price.get()), 8);
            put(frame, 48, trade->quantity.get(), 8);
        }
        return frame;
    }
};

#endif
