#ifndef DATA_VALIDATOR_HPP
#define DATA_VALIDATOR_HPP

#include "event_dispatcher.hpp"
#include "market_data_parser.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

struct DataValidatorConfig {
    uint64_t stale_gap = 1000;
    uint64_t max_price_jump_bps = 1000; // 10%
};

class DataValidator {
    struct SymbolState {
        Timestamp last_timestamp{0};
        Price last_price{0};
        bool initialized = false;
    };

    EventDispatcher& dispatcher_;
    DataValidatorConfig config_;
    std::unordered_map<Symbol, SymbolState, SymbolHash> state_;

    void emit(Timestamp timestamp, const Symbol& symbol,
              DataQualitySeverity severity, DataQualityCode code,
              std::string message) {
        dispatcher_.publish(SystemEvent::data_quality(timestamp, symbol,
            DataQualityEvent{severity, code, std::move(message)}));
    }

public:
    explicit DataValidator(EventDispatcher& dispatcher,
                           DataValidatorConfig config = {})
        : dispatcher_(dispatcher), config_(config) {}

    void report_parse_error(size_t line, DataQualityCode code,
                            const std::string& message) {
        emit(Timestamp(0), Symbol("UNKNOWN"), DataQualitySeverity::ERROR, code,
             "line " + std::to_string(line) + ": " + message);
    }

    bool validate(const ParsedMarketData& input) {
        const MarketDataEvent& data = input.data;
        if (data.price.get() <= 0 || data.bid.get() <= 0 || data.ask.get() <= 0 ||
            data.volume.get() == 0) {
            emit(input.timestamp, input.symbol, DataQualitySeverity::ERROR,
                 DataQualityCode::INVALID_VALUE,
                 "price, bid, ask and volume must be positive");
            return false;
        }
        if (data.bid > data.ask) {
            emit(input.timestamp, input.symbol, DataQualitySeverity::ERROR,
                 DataQualityCode::CROSSED_MARKET, "bid is greater than ask");
            return false;
        }

        SymbolState& state = state_[input.symbol];
        if (state.initialized && input.timestamp < state.last_timestamp) {
            emit(input.timestamp, input.symbol, DataQualitySeverity::ERROR,
                 DataQualityCode::TIMESTAMP_BACKWARDS, "timestamp moved backwards");
            return false;
        }
        if (state.initialized &&
            input.timestamp.get() - state.last_timestamp.get() > config_.stale_gap) {
            emit(input.timestamp, input.symbol, DataQualitySeverity::WARNING,
                 DataQualityCode::STALE_SYMBOL, "market data gap exceeded threshold");
        }
        if (state.initialized) {
            const uint64_t previous = static_cast<uint64_t>(state.last_price.get());
            const uint64_t current = static_cast<uint64_t>(data.price.get());
            const uint64_t difference = current > previous
                ? current - previous : previous - current;
            const long double jump_bps = previous == 0 ? 0.0L :
                static_cast<long double>(difference) * 10000.0L /
                static_cast<long double>(previous);
            if (jump_bps > static_cast<long double>(config_.max_price_jump_bps)) {
                emit(input.timestamp, input.symbol, DataQualitySeverity::WARNING,
                     DataQualityCode::PRICE_JUMP, "price jump exceeded threshold");
            }
        }
        state.last_timestamp = input.timestamp;
        state.last_price = data.price;
        state.initialized = true;
        return true;
    }
};

#endif
