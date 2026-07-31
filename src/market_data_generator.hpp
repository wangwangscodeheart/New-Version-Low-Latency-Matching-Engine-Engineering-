#ifndef MARKET_DATA_GENERATOR_HPP
#define MARKET_DATA_GENERATOR_HPP

#include "market_data_parser.hpp"
#include <fstream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

struct SyntheticSymbolConfig {
    Symbol symbol;
    Price initial_price;
};

struct MarketDataGeneratorConfig {
    uint64_t seed = 20260731;
    Timestamp first_timestamp{1'000'000};
    uint64_t timestamp_step = 100;
    size_t ticks_per_symbol = 100;
    std::vector<SyntheticSymbolConfig> symbols{
        {Symbol("AAPL"), from_double(190.00)},
        {Symbol("TSLA"), from_double(250.00)},
        {Symbol("NVDA"), from_double(130.00)}};
};

// Generates deterministic, interleaved multi-symbol quotes for demos/tests.
// The output is synthetic and must not be treated as exchange market data.
class MarketDataGenerator {
    static void write_price(std::ostream& output, Price price) {
        const int64_t raw = price.get();
        const int64_t whole = raw / PRICE_SCALE;
        const int64_t fraction = raw % PRICE_SCALE;
        output << whole << '.' << std::setw(4) << std::setfill('0') << fraction;
        output << std::setfill(' ');
    }

public:
    static size_t generate_csv(const std::string& filename,
                               const MarketDataGeneratorConfig& config = {}) {
        if (config.symbols.empty() || config.ticks_per_symbol == 0) {
            throw std::invalid_argument("Generator requires symbols and ticks");
        }
        if (config.timestamp_step == 0) {
            throw std::invalid_argument("Timestamp step must be positive");
        }

        std::ofstream output(filename, std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot create market data: " + filename);
        output << "timestamp,symbol,price,volume,bid,ask\n";

        std::mt19937_64 random(config.seed);
        std::uniform_int_distribution<int64_t> move_ticks(-8, 8);
        std::uniform_int_distribution<uint64_t> volume(1, 20);
        std::vector<Price> prices;
        prices.reserve(config.symbols.size());
        for (const auto& item : config.symbols) {
            if (item.initial_price.get() <= 10) {
                throw std::invalid_argument("Initial price is too small");
            }
            prices.push_back(item.initial_price);
        }

        size_t rows = 0;
        uint64_t timestamp = config.first_timestamp.get();
        for (size_t tick = 0; tick < config.ticks_per_symbol; ++tick) {
            for (size_t index = 0; index < config.symbols.size(); ++index) {
                prices[index] = Price(prices[index].get() + move_ticks(random));
                const Price bid(prices[index].get() - 1);
                const Price ask(prices[index].get() + 1);
                output << timestamp << ',' << config.symbols[index].symbol.value() << ',';
                write_price(output, prices[index]);
                output << ',' << volume(random) * 100 << ',';
                write_price(output, bid);
                output << ',';
                write_price(output, ask);
                output << '\n';
                timestamp += config.timestamp_step;
                ++rows;
            }
        }
        if (!output) throw std::runtime_error("Failed to write market data: " + filename);
        return rows;
    }
};

#endif
