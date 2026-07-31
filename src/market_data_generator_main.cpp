#include "market_data_generator.hpp"
#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        const std::string output = argc > 1 ? argv[1] : "synthetic_market_data.csv";
        MarketDataGeneratorConfig config;
        if (argc > 2) config.ticks_per_symbol = std::stoull(argv[2]);
        if (argc > 3) config.seed = std::stoull(argv[3]);
        const size_t rows = MarketDataGenerator::generate_csv(output, config);
        std::cout << "Generated " << rows << " synthetic rows for AAPL/TSLA/NVDA: "
                  << output << " (seed=" << config.seed << ")\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Market data generation failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
