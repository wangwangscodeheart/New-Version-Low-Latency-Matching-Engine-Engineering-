#ifndef MARKET_DATA_REPLAY_HPP
#define MARKET_DATA_REPLAY_HPP

#include "feed_decoder.hpp"
#include "market_data_gateway.hpp"
#include <array>
#include <istream>

struct MarketDataReplayResult { uint64_t decoded = 0; uint64_t applied = 0; };

class MarketDataReplay {
public:
    static MarketDataReplayResult replay(std::istream& input,
                                         MarketDataGateway& gateway) {
        FeedDecoder decoder;
        MarketDataReplayResult result;
        std::array<uint8_t, 4096> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const size_t count = static_cast<size_t>(input.gcount());
            decoder.consume(buffer.data(), count, [&](const FeedMessage& message) {
                ++result.decoded;
                if (gateway.on_message(message)) ++result.applied;
            });
        }
        decoder.finish();
        return result;
    }
};

#endif
