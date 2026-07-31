#ifndef MARKET_DATA_PARSER_HPP
#define MARKET_DATA_PARSER_HPP

#include "system_events.hpp"
#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct ParsedMarketData {
    Timestamp timestamp;
    Symbol symbol;
    MarketDataEvent data;
};

class MarketDataParseError : public std::runtime_error {
    DataQualityCode code_;
public:
    MarketDataParseError(DataQualityCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}
    DataQualityCode code() const noexcept { return code_; }
};

class MarketDataParser {
    static uint64_t parse_u64(const std::string& text, const char* field) {
        if (text.empty()) {
            throw MarketDataParseError(DataQualityCode::MISSING_FIELD,
                                       std::string("missing ") + field);
        }
        uint64_t value = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') {
                throw MarketDataParseError(DataQualityCode::PARSE_ERROR,
                                           std::string("invalid ") + field);
            }
            const uint64_t digit = static_cast<uint64_t>(ch - '0');
            if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
                throw MarketDataParseError(DataQualityCode::PARSE_ERROR,
                                           std::string("overflow in ") + field);
            }
            value = value * 10 + digit;
        }
        return value;
    }

    static Price parse_price(const std::string& text, const char* field) {
        if (text.empty()) {
            throw MarketDataParseError(DataQualityCode::MISSING_FIELD,
                                       std::string("missing ") + field);
        }
        const size_t dot = text.find('.');
        if (dot != std::string::npos && text.find('.', dot + 1) != std::string::npos) {
            throw MarketDataParseError(DataQualityCode::PARSE_ERROR,
                                       std::string("invalid ") + field);
        }
        const std::string whole_text = dot == std::string::npos
            ? text : text.substr(0, dot);
        const std::string fraction_text = dot == std::string::npos
            ? std::string() : text.substr(dot + 1);
        if (fraction_text.size() > 4) {
            throw MarketDataParseError(DataQualityCode::PARSE_ERROR,
                                       std::string("too many decimals in ") + field);
        }
        const uint64_t whole = parse_u64(whole_text, field);
        uint64_t fraction = 0;
        if (!fraction_text.empty()) fraction = parse_u64(fraction_text, field);
        for (size_t i = fraction_text.size(); i < 4; ++i) fraction *= 10;
        const uint64_t max_price = static_cast<uint64_t>(
            std::numeric_limits<int64_t>::max());
        if (whole > (max_price - fraction) / static_cast<uint64_t>(PRICE_SCALE)) {
            throw MarketDataParseError(DataQualityCode::PARSE_ERROR,
                                       std::string("overflow in ") + field);
        }
        return Price(static_cast<int64_t>(whole * PRICE_SCALE + fraction));
    }

public:
    static ParsedMarketData parse(const std::string& line) {
        std::stringstream stream(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(stream, field, ',')) fields.push_back(field);
        if (!line.empty() && line.back() == ',') fields.emplace_back();
        if (fields.size() != 6) {
            throw MarketDataParseError(DataQualityCode::MISSING_FIELD,
                                       "expected 6 CSV fields");
        }
        if (fields[1].empty()) {
            throw MarketDataParseError(DataQualityCode::MISSING_FIELD,
                                       "missing symbol");
        }
        return ParsedMarketData{
            Timestamp(parse_u64(fields[0], "timestamp")), Symbol(fields[1]),
            MarketDataEvent{parse_price(fields[2], "price"),
                Quantity(parse_u64(fields[3], "volume")),
                parse_price(fields[4], "bid"), parse_price(fields[5], "ask")}};
    }
};

#endif
