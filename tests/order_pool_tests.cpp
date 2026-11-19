#include "orderbook.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <array>
#include <type_traits>
#include <utility>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

namespace {
static_assert(!std::is_copy_constructible_v<ObjectPool<Order>>);
static_assert(!std::is_copy_assignable_v<ObjectPool<Order>>);
static_assert(std::is_nothrow_move_constructible_v<ObjectPool<Order>>);
static_assert(!std::is_move_assignable_v<ObjectPool<Order>>);
static_assert(noexcept(std::declval<ObjectPool<Order>&>().allocate()));
static_assert(noexcept(std::declval<ObjectPool<Order>&>().deallocate(nullptr)));

void test_duplicate_return_does_not_duplicate_a_slot() {
    ObjectPool<Order> pool(2);
    Order* first = pool.allocate();
    Order* second = pool.allocate();
    TEST_ASSERT(first && second && first != second && pool.available() == 0);
    pool.deallocate(first);
    pool.deallocate(first);
    std::cout << "duplicate return: free=" << pool.available() << " capacity=" << pool.capacity() << '\n';
    TEST_ASSERT(pool.available() == 1);
    TEST_ASSERT(pool.allocate() == first && pool.allocate() == nullptr);
    pool.deallocate(first);
    pool.deallocate(second);
    TEST_ASSERT(pool.available() == 2);
}

void test_foreign_return_is_not_admitted() {
    ObjectPool<Order> pool(1), other(1);
    Order* owned = pool.allocate();
    Order* foreign = other.allocate();
    pool.deallocate(foreign);
    std::cout << "foreign return: free=" << pool.available() << " capacity=" << pool.capacity() << '\n';
    TEST_ASSERT(pool.available() == 0 && pool.allocate() == nullptr);
    pool.deallocate(owned);
    other.deallocate(foreign);
}

void test_empty_pool_and_lifo_order() {
    ObjectPool<Order> empty(0);
    TEST_ASSERT(empty.capacity() == 0 && empty.available() == 0 && !empty.allocate());
    empty.deallocate(nullptr);
    for (size_t capacity = 1; capacity <= 8; ++capacity) {
        ObjectPool<Order> pool(capacity);
        std::array<Order*, 8> pointers{};
        for (size_t index = 0; index < capacity; ++index) {
            pointers[index] = pool.allocate();
            TEST_ASSERT(pointers[index]);
            if (index) TEST_ASSERT(pointers[index] + 1 == pointers[index - 1]);
            TEST_ASSERT(pool.available() == capacity - index - 1);
        }
        TEST_ASSERT(!pool.allocate());
        for (size_t index = 0; index < capacity; ++index) pool.deallocate(pointers[index]);
        TEST_ASSERT(pool.available() == capacity);
        for (size_t index = capacity; index != 0; --index) {
            TEST_ASSERT(pool.allocate() == pointers[index - 1]);
        }
        TEST_ASSERT(!pool.allocate());
    }
    std::cout << "Empty/capacity/LIFO passed: capacities 0..8\n";
}

void test_null_stack_interior_and_one_past_returns() {
    ObjectPool<Order> pool(1);
    Order* owned = pool.allocate();
    Order stack(OrderId(9), PrioritySequence(9), Side::BUY, Price(100), Quantity(1));
    auto* interior = reinterpret_cast<Order*>(reinterpret_cast<unsigned char*>(owned) + 1);
    pool.deallocate(nullptr);
    pool.deallocate(&stack);
    pool.deallocate(interior);
    pool.deallocate(owned + 1);
    TEST_ASSERT(pool.available() == 0 && !pool.allocate());
    pool.deallocate(owned);
    TEST_ASSERT(pool.available() == 1 && pool.allocate() == owned);
    std::cout << "Invalid returns ignored: null/stack/interior/one-past\n";
}

void test_pool_move_transfers_live_slots_and_empties_source() {
    ObjectPool<Order> original(2);
    Order* live = original.allocate();
    Order* free = original.allocate();
    live->activate(OrderId(11), PrioritySequence(5), Side::BUY, Price(100), Quantity(3));
    original.deallocate(free);
    ObjectPool<Order> moved(std::move(original));
    TEST_ASSERT(original.capacity() == 0 && original.available() == 0 && !original.allocate());
    original.deallocate(live);
    TEST_ASSERT(original.available() == 0);
    TEST_ASSERT(moved.capacity() == 2 && moved.available() == 1);
    TEST_ASSERT(live->id == OrderId(11) && live->remaining_qty == Quantity(3));
    TEST_ASSERT(moved.allocate() == free && !moved.allocate());
    moved.deallocate(live);
    moved.deallocate(free);
    ObjectPool<Order> twice(std::move(moved));
    TEST_ASSERT(moved.capacity() == 0 && twice.available() == 2);
    TEST_ASSERT(twice.allocate() == free && twice.allocate() == live && !twice.allocate());
    std::cout << "Pool move passed: live pointers preserved, sources empty\n";
}

void test_repeated_slot_reuse_and_activation() {
    ObjectPool<Order> pool(7);
    std::array<Order*, 7> pointers{};
    LimitLevel marker(Price(100));
    for (uint64_t round = 0; round < 500; ++round) {
        for (size_t index = 0; index < pointers.size(); ++index) {
            Order* value = pointers[index] = pool.allocate();
            TEST_ASSERT(value);
            for (size_t previous = 0; previous < index; ++previous) {
                TEST_ASSERT(pointers[previous] != value);
            }
            value->next = value;
            value->prev = value;
            value->parent_level = &marker;
            value->activate(OrderId(round * 7 + index + 1), PrioritySequence(round + 1),
                            Side::SELL, Price(100), Quantity(2));
            TEST_ASSERT(!value->next && !value->prev && !value->parent_level);
            TEST_ASSERT(value->id == OrderId(round * 7 + index + 1) &&
                        value->priority_sequence == PrioritySequence(round + 1) &&
                        value->side == Side::SELL && value->price == Price(100) &&
                        value->original_qty == Quantity(2) && value->remaining_qty == Quantity(2));
        }
        TEST_ASSERT(pool.available() == 0 && !pool.allocate());
        // activate cleared the test-only links. Production callers must unlink
        // a real order from its FIFO/index before returning its slot.
        for (size_t index = 0; index < pointers.size(); ++index) {
            Order* value = pointers[(index * 3) % pointers.size()];
            pool.deallocate(value);
            pool.deallocate(value);
            TEST_ASSERT(pool.available() == index + 1);
        }
    }
    std::cout << "Slot reuse passed: 500 rounds, 3500 activations\n";
}

void test_book_move_then_match_cancel_and_refill() {
    OrderBook original(4);
    NullEventSink sink;
    original.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::BUY,
                                     Price(1'000'000), Quantity(2)), sink);
    original.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::BUY,
                                     Price(990'000), Quantity(1)), sink);
    const auto before = original.capture_state();
    OrderBook moved(std::move(original));
    TEST_ASSERT(moved.capture_state() == before && moved.check_invariants());
    TEST_ASSERT(!original.ready_for_commands() && original.capacity() == 0);
    TEST_ASSERT(moved.process(NewOrderCommand(CommandSequence(3), OrderId(3), Side::SELL,
        Price(1'000'000), Quantity(1)), sink).applied());
    TEST_ASSERT(moved.check_invariants());
    TEST_ASSERT(moved.process(CancelOrderCommand(CommandSequence(4), OrderId(1)), sink).applied());
    TEST_ASSERT(moved.process(CancelOrderCommand(CommandSequence(5), OrderId(2)), sink).applied());
    TEST_ASSERT(moved.active_order_count() == 0 && moved.check_invariants());
    for (uint64_t sequence = 6; sequence <= 9; ++sequence) {
        TEST_ASSERT(moved.process(NewOrderCommand(CommandSequence(sequence), OrderId(sequence),
            Side::BUY, Price(1'000'000), Quantity(1)), sink).applied());
    }
    TEST_ASSERT(moved.active_order_count() == 4 && moved.check_invariants());
    TEST_ASSERT(moved.create_snapshot().active_orders.size() == 4);
    std::cout << "Book move passed: match/cancel/refill/snapshot\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--duplicate-only") {
            test_duplicate_return_does_not_duplicate_a_slot();
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--foreign-only") {
            test_foreign_return_is_not_admitted();
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Unknown order pool test option");
        test_duplicate_return_does_not_duplicate_a_slot();
        test_foreign_return_is_not_admitted();
        test_empty_pool_and_lifo_order();
        test_null_stack_interior_and_one_past_returns();
        test_pool_move_transfers_live_slots_and_empties_source();
        test_repeated_slot_reuse_and_activation();
        test_book_move_then_match_cancel_and_refill();
        std::cout << "Order pool tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Order pool tests failed: " << error.what() << '\n';
        return 1;
    }
}
