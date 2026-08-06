#ifndef COMMANDS_HPP
#define COMMANDS_HPP

#include "types.hpp"
#include <cstdint>
#include <variant>

enum class CommandType : uint8_t {
    NEW_ORDER = 0,
    CANCEL_ORDER = 1
};

enum class TimeInForce : uint8_t { GTC = 0, IOC = 1, POST_ONLY = 2 };

inline const char* to_string(TimeInForce value) noexcept {
    switch (value) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::POST_ONLY: return "POST_ONLY";
    }
    return "UNKNOWN";
}

struct NewOrderCommand {
    CommandSequence command_sequence;
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;
    TimeInForce time_in_force;

    NewOrderCommand(CommandSequence sequence, OrderId id, Side order_side,
                    Price order_price, Quantity order_quantity,
                    TimeInForce tif = TimeInForce::GTC)
        : command_sequence(sequence), order_id(id), side(order_side),
          price(order_price), quantity(order_quantity), time_in_force(tif) {}
};

struct CancelOrderCommand {
    CommandSequence command_sequence;
    OrderId order_id;

    CancelOrderCommand(CommandSequence sequence, OrderId id)
        : command_sequence(sequence), order_id(id) {}
};

using Command = std::variant<NewOrderCommand, CancelOrderCommand>;

inline CommandType get_command_type(const Command& command) noexcept {
    return std::holds_alternative<NewOrderCommand>(command)
        ? CommandType::NEW_ORDER : CommandType::CANCEL_ORDER;
}

inline OrderId get_order_id(const Command& command) noexcept {
    return std::visit([](const auto& value) { return value.order_id; }, command);
}

inline CommandSequence get_command_sequence(const Command& command) noexcept {
    return std::visit([](const auto& value) { return value.command_sequence; }, command);
}

#endif
