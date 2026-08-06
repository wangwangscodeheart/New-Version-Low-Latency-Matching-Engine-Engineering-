#include "orderbook.hpp"
#include <iostream>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

int main() {
    try {
        OrderBook book(3, true, {}, 2);
        book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                                     Price(1000000), Quantity(2)));
        book.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                                     Price(1000000), Quantity(3)));
        book.process(NewOrderCommand(CommandSequence(3), OrderId(3), Side::BUY,
                                     Price(990000), Quantity(4)));
        CapacityMetrics full = book.capacity_metrics();
        TEST_ASSERT(full.active_orders == 3);
        TEST_ASSERT(full.peak_active_orders == 3);
        TEST_ASSERT(full.active_levels == 2);
        TEST_ASSERT(full.peak_active_levels == 2);
        TEST_ASSERT(full.index_load == 3);
        TEST_ASSERT(full.index_capacity >= full.order_capacity);
        TEST_ASSERT(full.order_capacity == 3 && full.level_capacity == 3);
        TEST_ASSERT(full.event_capacity >= 6);
        TEST_ASSERT(full.index_load_factor() > 0.0);

        book.process(CancelOrderCommand(CommandSequence(4), OrderId(1)));
        book.process(CancelOrderCommand(CommandSequence(5), OrderId(2)));
        CapacityMetrics reduced = book.capacity_metrics();
        TEST_ASSERT(reduced.active_orders == 1);
        TEST_ASSERT(reduced.active_levels == 1);
        TEST_ASSERT(reduced.peak_active_orders == 3);
        TEST_ASSERT(reduced.peak_active_levels == 2);
        TEST_ASSERT(book.check_invariants());
        std::cout << "Capacity metrics tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Capacity metrics tests failed: " << error.what() << '\n';
        return 1;
    }
}
