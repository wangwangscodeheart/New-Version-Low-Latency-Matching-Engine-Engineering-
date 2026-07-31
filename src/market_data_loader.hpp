#ifndef MARKET_DATA_LOADER_HPP
#define MARKET_DATA_LOADER_HPP

#include "data_validator.hpp"
#include "trading_engine.hpp"
#include <fstream>
#include <stdexcept>
#include <string>

struct MarketDataLoadResult {
    size_t rows = 0;
    size_t published = 0;
    size_t rejected = 0;
};

class MarketDataLoader {
public:
    static MarketDataLoadResult load(const std::string& filename,
                                     DataValidator& validator,
                                     TradingEngine& engine) {
        std::ifstream file(filename);
        if (!file) throw std::runtime_error("Cannot open market data: " + filename);
        std::string line;
        size_t line_number = 0;
        if (!std::getline(file, line)) {
            throw std::runtime_error("Market data file is empty: " + filename);
        }
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != "timestamp,symbol,price,volume,bid,ask") {
            throw std::runtime_error("Invalid market data CSV header");
        }

        MarketDataLoadResult result;
        while (std::getline(file, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            ++result.rows;
            try {
                const ParsedMarketData parsed = MarketDataParser::parse(line);
                if (validator.validate(parsed)) {
                    engine.publish_market_data(parsed.timestamp, parsed.symbol, parsed.data);
                    ++result.published;
                } else {
                    ++result.rejected;
                }
            } catch (const MarketDataParseError& error) {
                validator.report_parse_error(line_number, error.code(), error.what());
                ++result.rejected;
            }
        }
        return result;
    }
};

#endif
