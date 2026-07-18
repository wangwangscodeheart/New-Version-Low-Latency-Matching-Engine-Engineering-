#ifndef COMMANDS_HPP
#define COMMANDS_HPP

#include "types.hpp"
#include <cstdint>
#include <variant>

enum class CommandType : uint8_t {
    NEW_ORDER = 0,
    CANCEL_ORDER = 1
};

struct NewOrderCommand {
    OrderId order_id;
    Side side;
    Price price;
    Quantity quantity;

    NewOrderCommand(OrderId id, Side order_side, Price order_price, Quantity order_quantity)
        : order_id(id), side(order_side), price(order_price), quantity(order_quantity) {}
};

struct CancelOrderCommand {
    OrderId order_id;

    explicit CancelOrderCommand(OrderId id) : order_id(id) {}
};

using Command = std::variant<NewOrderCommand, CancelOrderCommand>;

inline CommandType get_command_type(const Command& command) noexcept {
    return std::holds_alternative<NewOrderCommand>(command)
        ? CommandType::NEW_ORDER : CommandType::CANCEL_ORDER;
}

inline OrderId get_order_id(const Command& command) noexcept {
    return std::visit([](const auto& value) { return value.order_id; }, command);
}

#endif
