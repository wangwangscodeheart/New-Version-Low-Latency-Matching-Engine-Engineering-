#include "monitor.hpp"
#include "trading_engine.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

int main() {
    constexpr size_t pairs_per_symbol = 50000;
    constexpr size_t command_count = pairs_per_symbol * 2 * 3;
    MarketManager markets(pairs_per_symbol + 16);
    EventDispatcher dispatcher;
    Monitor monitor;
    monitor.attach(dispatcher);
    TradingEngine engine(markets, dispatcher);
    const std::array<Symbol, 3> symbols{
        Symbol("AAPL"), Symbol("TSLA"), Symbol("NVDA")};
    std::array<uint64_t, 3> sequences{0, 0, 0};

    const auto start = std::chrono::steady_clock::now();
    uint64_t order_id = 1;
    uint64_t timestamp = 1;
    for (size_t i = 0; i < pairs_per_symbol; ++i) {
        for (size_t symbol_index = 0; symbol_index < symbols.size(); ++symbol_index) {
            const Symbol& symbol = symbols[symbol_index];
            engine.submit(Timestamp(timestamp++), symbol,
                NewOrderCommand(CommandSequence(++sequences[symbol_index]),
                    OrderId(order_id), Side::BUY, from_double(100.00), Quantity(1)));
            engine.submit(Timestamp(timestamp++), symbol,
                CancelOrderCommand(CommandSequence(++sequences[symbol_index]),
                                   OrderId(order_id)));
            ++order_id;
        }
    }
    const auto finish = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finish - start).count();
    const MonitorSnapshot metrics = monitor.snapshot();

    std::cout << "V2 routed event-path benchmark\n"
              << "commands=" << command_count << '\n'
              << "seconds=" << std::fixed << std::setprecision(6) << seconds << '\n'
              << "throughput_commands_per_sec="
              << static_cast<uint64_t>(command_count / seconds) << '\n'
              << "avg_route_and_match_latency_ns="
              << std::setprecision(2) << metrics.global.average_latency_ns() << '\n'
              << "max_route_and_match_latency_ns="
              << metrics.global.max_latency_ns << '\n';
    return 0;
}
