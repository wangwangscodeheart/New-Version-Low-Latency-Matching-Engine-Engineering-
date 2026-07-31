#ifndef SYMBOL_HPP
#define SYMBOL_HPP

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

// Symbol belongs to the routing layer. It is deliberately not stored in each
// hot-path Order because one OrderBook represents exactly one instrument.
class Symbol {
    std::string value_;

public:
    explicit Symbol(std::string_view value) : value_(value) {
        if (value_.empty()) {
            throw std::invalid_argument("Symbol cannot be empty");
        }
    }

    const std::string& value() const noexcept { return value_; }

    bool operator==(const Symbol& other) const noexcept {
        return value_ == other.value_;
    }

    bool operator!=(const Symbol& other) const noexcept {
        return !(*this == other);
    }
};

struct SymbolHash {
    size_t operator()(const Symbol& symbol) const noexcept {
        return std::hash<std::string>{}(symbol.value());
    }
};

#endif
