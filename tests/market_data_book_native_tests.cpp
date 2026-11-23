#include "market_data_book.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
void assert_order(const MarketDataBook& book, uint64_t id, int64_t price,
                  uint64_t remaining, Side side) {
    const auto* found = book.find_order(id);
    TEST_ASSERT(found != nullptr);
    TEST_ASSERT(found->price == Price(price) && found->remaining == Quantity(remaining));
    TEST_ASSERT(found->side == side);
}
template<typename Action>
void expect_rejected_unchanged(MarketDataBook& book, Action&& action) {
    TEST_ASSERT(book.check_invariants());
    const auto before = book.capture_state();
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    TEST_ASSERT(!action());
    TEST_ASSERT(book.capture_state() == before);
    TEST_ASSERT(book.best_bid() == bid && book.best_ask() == ask);
    TEST_ASSERT(book.check_invariants());
}
void test_add_lookup_and_bbo() {
    MarketDataBook book;
    TEST_ASSERT(book.check_invariants() && book.order_count() == 0);
    TEST_ASSERT(!book.best_bid() && !book.best_ask());
    TEST_ASSERT(book.find_order(0) == nullptr && book.find_order(100) == nullptr);
    TEST_ASSERT(book.add_order(1, Price(100), Quantity(10), Side::BUY));
    TEST_ASSERT(book.add_order(2, Price(105), Quantity(4), Side::BUY));
    TEST_ASSERT(book.add_order(3, Price(100), Quantity(7), Side::BUY));
    TEST_ASSERT(book.add_order(4, Price(120), Quantity(8), Side::SELL));
    TEST_ASSERT(book.add_order(5, Price(110), Quantity(9), Side::SELL));
    TEST_ASSERT(book.order_count() == 5 && book.check_invariants());
    TEST_ASSERT(book.best_bid() == Price(105) && book.best_ask() == Price(110));
    TEST_ASSERT(book.bid_quantity(Price(100)) == 17 && book.ask_quantity(Price(100)) == 0);
    assert_order(book, 1, 100, 10, Side::BUY);
    assert_order(book, 5, 110, 9, Side::SELL);
    expect_rejected_unchanged(book, [&] { return book.add_order(0, Price(100), Quantity(1), Side::BUY); });
    expect_rejected_unchanged(book, [&] { return book.add_order(1, Price(100), Quantity(1), Side::SELL); });
    expect_rejected_unchanged(book, [&] { return book.add_order(6, Price(0), Quantity(1), Side::BUY); });
    expect_rejected_unchanged(book, [&] { return book.add_order(6, Price(-1), Quantity(1), Side::BUY); });
    expect_rejected_unchanged(book, [&] { return book.add_order(6, Price(100), Quantity(0), Side::BUY); });
    expect_rejected_unchanged(book, [&] {
        return book.add_order(6, Price(100), Quantity(1), static_cast<Side>(255));
    });
    TEST_ASSERT(book.erase_order(2));
    TEST_ASSERT(book.best_bid() == Price(100) && book.bid_quantity(Price(105)) == 0);
    TEST_ASSERT(book.erase_order(5));
    TEST_ASSERT(book.best_ask() == Price(120) && book.ask_quantity(Price(110)) == 0);
    TEST_ASSERT(book.check_invariants());
}
void test_partial_reduction_and_exact_removal() {
    MarketDataBook book;
    TEST_ASSERT(book.add_order(1, Price(100), Quantity(10), Side::BUY));
    TEST_ASSERT(book.add_order(2, Price(100), Quantity(7), Side::BUY));
    TEST_ASSERT(book.add_order(3, Price(110), Quantity(9), Side::SELL));
    TEST_ASSERT(book.reduce_order(1, Quantity(4)));
    assert_order(book, 1, 100, 6, Side::BUY);
    TEST_ASSERT(book.bid_quantity(Price(100)) == 13 && book.check_invariants());
    expect_rejected_unchanged(book, [&] { return book.reduce_order(1, Quantity(7)); });
    expect_rejected_unchanged(book, [&] { return book.reduce_order(1, Quantity(0)); });
    expect_rejected_unchanged(book, [&] { return book.reduce_order(0, Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.reduce_order(999, Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.erase_order(999); });
    TEST_ASSERT(book.erase_order(2));
    TEST_ASSERT(book.find_order(2) == nullptr && book.bid_quantity(Price(100)) == 6);
    TEST_ASSERT(book.reduce_order(1, Quantity(6)));
    TEST_ASSERT(book.find_order(1) == nullptr && book.bid_quantity(Price(100)) == 0);
    TEST_ASSERT(!book.best_bid() && book.best_ask() == Price(110));
    TEST_ASSERT(book.reduce_order(3, Quantity(8)));
    assert_order(book, 3, 110, 1, Side::SELL);
    TEST_ASSERT(book.ask_quantity(Price(110)) == 1);
    TEST_ASSERT(book.reduce_order(3, Quantity(1)));
    TEST_ASSERT(book.order_count() == 0 && !book.best_bid() && !book.best_ask());
    TEST_ASSERT(book.check_invariants());
}
void test_replacement_total_size_and_inherited_side() {
    MarketDataBook book;
    TEST_ASSERT(book.add_order(1, Price(100), Quantity(10), Side::BUY));
    TEST_ASSERT(book.add_order(2, Price(100), Quantity(5), Side::BUY));
    TEST_ASSERT(book.add_order(3, Price(90), Quantity(3), Side::BUY));
    TEST_ASSERT(book.replace_order(1, 10, Price(100), Quantity(7)));
    TEST_ASSERT(book.find_order(1) == nullptr && book.order_count() == 3);
    assert_order(book, 10, 100, 7, Side::BUY);
    TEST_ASSERT(book.bid_quantity(Price(100)) == 12 && book.check_invariants());
    TEST_ASSERT(book.replace_order(10, 11, Price(105), Quantity(4)));
    TEST_ASSERT(book.find_order(10) == nullptr && book.bid_quantity(Price(100)) == 5);
    TEST_ASSERT(book.bid_quantity(Price(105)) == 4 && book.best_bid() == Price(105));
    assert_order(book, 11, 105, 4, Side::BUY);
    TEST_ASSERT(book.replace_order(11, 12, Price(100), Quantity(8)));
    TEST_ASSERT(book.bid_quantity(Price(105)) == 0 && book.bid_quantity(Price(100)) == 13);
    TEST_ASSERT(book.best_bid() == Price(100) && book.check_invariants());

    TEST_ASSERT(book.add_order(20, Price(110), Quantity(9), Side::SELL));
    TEST_ASSERT(book.add_order(21, Price(120), Quantity(6), Side::SELL));
    TEST_ASSERT(book.replace_order(20, 22, Price(120), Quantity(4)));
    TEST_ASSERT(book.ask_quantity(Price(110)) == 0 && book.ask_quantity(Price(120)) == 10);
    TEST_ASSERT(book.best_ask() == Price(120));
    assert_order(book, 22, 120, 4, Side::SELL);
    TEST_ASSERT(book.replace_order(22, 23, Price(115), Quantity(2)));
    TEST_ASSERT(book.ask_quantity(Price(120)) == 6 && book.ask_quantity(Price(115)) == 2);
    assert_order(book, 23, 115, 2, Side::SELL);
    TEST_ASSERT(book.best_ask() == Price(115) && book.check_invariants());
    TEST_ASSERT(book.reduce_order(23, Quantity(1)));
    TEST_ASSERT(book.replace_order(23, 24, Price(115), Quantity(8)));
    // After the reduction the old quantity was one: replacement sets eight,
    // not one-plus-eight, original-nine-plus-eight, or an execution delta.
    assert_order(book, 24, 115, 8, Side::SELL);
    TEST_ASSERT(book.ask_quantity(Price(115)) == 8 && book.check_invariants());
}
void test_invalid_replacements_are_atomic() {
    MarketDataBook book;
    TEST_ASSERT(book.add_order(1, Price(100), Quantity(10), Side::BUY));
    TEST_ASSERT(book.add_order(2, Price(110), Quantity(4), Side::SELL));
    expect_rejected_unchanged(book, [&] { return book.replace_order(0, 3, Price(101), Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.replace_order(999, 3, Price(101), Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.replace_order(1, 0, Price(101), Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.replace_order(1, 1, Price(101), Quantity(1)); });
    expect_rejected_unchanged(book, [&] { return book.replace_order(1, 1, Price(100), Quantity(10)); });
    expect_rejected_unchanged(book, [&] { return book.replace_order(1, 2, Price(101), Quantity(1)); });
    for (const auto price : {int64_t{0}, int64_t{-1}, std::numeric_limits<int64_t>::min()}) {
        expect_rejected_unchanged(book, [&] { return book.replace_order(1, 3, Price(price), Quantity(1)); });
    }
    expect_rejected_unchanged(book, [&] { return book.replace_order(1, 3, Price(101), Quantity(0)); });
    TEST_ASSERT(book.replace_order(1, 3, Price(101), Quantity(1)));
    // A previously removed reference may be used by this generic book API;
    // session-level reference reuse policies belong to the native adapter.
    TEST_ASSERT(book.replace_order(3, 1, Price(100), Quantity(10)));
    assert_order(book, 1, 100, 10, Side::BUY);
    TEST_ASSERT(book.check_invariants());
}
void test_full_width_quantities_and_aggregate_boundaries() {
    const auto maximum = std::numeric_limits<uint64_t>::max();
    MarketDataBook same_price;
    TEST_ASSERT(same_price.add_order(1, Price(100), Quantity(maximum - 5), Side::BUY));
    TEST_ASSERT(same_price.add_order(2, Price(100), Quantity(5), Side::BUY));
    TEST_ASSERT(same_price.bid_quantity(Price(100)) == maximum);
    // Must subtract the old quantity BEFORE checking replacement overflow.
    TEST_ASSERT(same_price.replace_order(1, 3, Price(100), Quantity(maximum - 6)));
    TEST_ASSERT(same_price.bid_quantity(Price(100)) == maximum - 1);
    expect_rejected_unchanged(same_price, [&] {
        return same_price.replace_order(3, 4, Price(100), Quantity(maximum - 4));
    });
    TEST_ASSERT(same_price.replace_order(3, 4, Price(100), Quantity(maximum - 5)));
    TEST_ASSERT(same_price.bid_quantity(Price(100)) == maximum);
    TEST_ASSERT(same_price.replace_order(2, 5, Price(100), Quantity(4)));
    TEST_ASSERT(same_price.bid_quantity(Price(100)) == maximum - 1);
    TEST_ASSERT(same_price.check_invariants());

    MarketDataBook new_price;
    TEST_ASSERT(new_price.add_order(10, Price(110), Quantity(maximum - 1), Side::BUY));
    TEST_ASSERT(new_price.add_order(11, Price(100), Quantity(2), Side::BUY));
    expect_rejected_unchanged(new_price, [&] {
        return new_price.replace_order(11, 12, Price(110), Quantity(2));
    });
    TEST_ASSERT(new_price.replace_order(11, 12, Price(110), Quantity(1)));
    TEST_ASSERT(new_price.bid_quantity(Price(110)) == maximum && new_price.bid_quantity(Price(100)) == 0);
    TEST_ASSERT(new_price.add_order(20, Price(110), Quantity(maximum), Side::SELL));
    TEST_ASSERT(new_price.ask_quantity(Price(110)) == maximum && new_price.check_invariants());

    MarketDataBook full;
    TEST_ASSERT(full.add_order(maximum, Price(std::numeric_limits<int64_t>::max()), Quantity(maximum), Side::SELL));
    TEST_ASSERT(full.best_ask() == Price(std::numeric_limits<int64_t>::max()));
    TEST_ASSERT(full.replace_order(maximum, 1, Price(1), Quantity(maximum)));
    TEST_ASSERT(full.find_order(maximum) == nullptr && full.best_ask() == Price(1));
    assert_order(full, 1, 1, maximum, Side::SELL);
    TEST_ASSERT(full.reduce_order(1, Quantity(maximum)));
    TEST_ASSERT(full.order_count() == 0 && full.check_invariants());
}
void test_legacy_feed_behavior_and_restore() {
    MarketDataBook book;
    TEST_ASSERT(book.apply(FeedAddOrder{1, Price(100), Quantity(10), Side::BUY}));
    TEST_ASSERT(book.apply(FeedAddOrder{2, Price(110), Quantity(5), Side::SELL}));
    // Simulated FeedTrade still requires the passive order's price. Native C
    // has a different execution-price field and therefore uses reduce_order.
    expect_rejected_unchanged(book, [&] {
        return book.apply(FeedTrade{1, 1, Price(101), Quantity(3)});
    });
    expect_rejected_unchanged(book, [&] {
        return book.apply(FeedTrade{0, 1, Price(100), Quantity(3)});
    });
    TEST_ASSERT(book.apply(FeedTrade{1, 1, Price(100), Quantity(3)}));
    assert_order(book, 1, 100, 7, Side::BUY);
    TEST_ASSERT(book.reduce_order(1, Quantity(2)));
    TEST_ASSERT(book.replace_order(1, 3, Price(102), Quantity(8)));
    const auto state = book.capture_state();
    auto restored = MarketDataBook::restore(state);
    TEST_ASSERT(restored.capture_state() == state && restored.check_invariants());
    TEST_ASSERT(restored.best_bid() == Price(102) && restored.best_ask() == Price(110));
    assert_order(restored, 3, 102, 8, Side::BUY);
    TEST_ASSERT(restored.apply(FeedCancelOrder{3}));
    TEST_ASSERT(restored.apply(FeedTrade{2, 2, Price(110), Quantity(5)}));
    TEST_ASSERT(restored.order_count() == 0 && restored.check_invariants());
}
void test_repeated_identity_changes_preserve_all_orders() {
    MarketDataBook book;
    for (uint64_t i = 1; i <= 256; ++i) {
        TEST_ASSERT(book.add_order(i, Price(100), Quantity(i), Side::BUY));
    }
    uint64_t expected = (256 * 257) / 2;
    for (uint64_t i = 1; i <= 256; ++i) {
        TEST_ASSERT(book.replace_order(i, i + 1000, Price(101), Quantity(i + 1)));
        expected -= i;
        TEST_ASSERT(book.bid_quantity(Price(100)) == expected);
        TEST_ASSERT(book.find_order(i) == nullptr);
        assert_order(book, i + 1000, 101, i + 1, Side::BUY);
        TEST_ASSERT(book.order_count() == 256 && book.check_invariants());
    }
    TEST_ASSERT(book.bid_quantity(Price(100)) == 0 && book.best_bid() == Price(101));
    TEST_ASSERT(book.bid_quantity(Price(101)) == ((256 * 257) / 2) + 256);
}
} // namespace

int main() {
    try {
        test_add_lookup_and_bbo();
        test_partial_reduction_and_exact_removal();
        test_replacement_total_size_and_inherited_side();
        test_invalid_replacements_are_atomic();
        test_full_width_quantities_and_aggregate_boundaries();
        test_legacy_feed_behavior_and_restore();
        test_repeated_identity_changes_preserve_all_orders();
        std::cout << "MarketDataBook native lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MarketDataBook native lifecycle test failed: " << error.what() << '\n';
        return 1;
    }
}
