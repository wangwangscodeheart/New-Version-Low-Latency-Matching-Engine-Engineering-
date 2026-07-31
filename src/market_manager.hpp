#ifndef MARKET_MANAGER_HPP
#define MARKET_MANAGER_HPP

#include "orderbook.hpp"
#include "symbol.hpp"
#include <cstddef>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <vector>

struct InstrumentRuntimeConfig {
    size_t order_capacity = 100000;
    bool record_events = true;
    PriceLadderConfig price_ladder{};
    size_t event_reserve_multiplier = 2;
    InstrumentConfig trading_rules{};
};

enum class RoutingStatus : uint8_t {
    ROUTED = 0,
    UNKNOWN_SYMBOL = 1
};

struct MarketProcessResult {
    RoutingStatus routing_status;
    ProcessResult process_result;

    bool routed() const noexcept {
        return routing_status == RoutingStatus::ROUTED;
    }
};

// Owns one independent deterministic OrderBook per symbol. MarketManager is a
// single-writer component: callers must serialize register/process operations.
class MarketManager {
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>, SymbolHash> books_;

public:
    explicit MarketManager(size_t default_capacity = 100000) {
        InstrumentRuntimeConfig config;
        config.order_capacity = default_capacity;
        register_symbol(Symbol("AAPL"), config);
        register_symbol(Symbol("TSLA"), config);
        register_symbol(Symbol("NVDA"), config);
    }

    bool register_symbol(const Symbol& symbol,
                         const InstrumentRuntimeConfig& config) {
        if (books_.find(symbol) != books_.end()) return false;
        auto book = std::make_unique<OrderBook>(
            config.order_capacity, config.record_events, config.price_ladder,
            config.event_reserve_multiplier, config.trading_rules);
        books_.emplace(symbol, std::move(book));
        return true;
    }

    MarketProcessResult process(const Symbol& symbol, const Command& command) {
        OrderBook* book = find_book(symbol);
        if (!book) {
            return MarketProcessResult{
                RoutingStatus::UNKNOWN_SYMBOL,
                ProcessResult{ProcessStatus::SEQUENCE_REJECTED, 0, 0}};
        }
        return MarketProcessResult{RoutingStatus::ROUTED, book->process(command)};
    }

    OrderBook* find_book(const Symbol& symbol) noexcept {
        const auto it = books_.find(symbol);
        return it == books_.end() ? nullptr : it->second.get();
    }

    const OrderBook* find_book(const Symbol& symbol) const noexcept {
        const auto it = books_.find(symbol);
        return it == books_.end() ? nullptr : it->second.get();
    }

    bool contains(const Symbol& symbol) const noexcept {
        return find_book(symbol) != nullptr;
    }

    size_t symbol_count() const noexcept { return books_.size(); }

    std::vector<Symbol> symbols() const {
        std::vector<Symbol> result;
        result.reserve(books_.size());
        for (const auto& entry : books_) result.push_back(entry.first);
        std::sort(result.begin(), result.end(),
            [](const Symbol& left, const Symbol& right) {
                return left.value() < right.value();
            });
        return result;
    }
};

#endif
