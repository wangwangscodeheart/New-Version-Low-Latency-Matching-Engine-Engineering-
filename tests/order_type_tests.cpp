#include "journal.hpp"
#include "orderbook.hpp"
#include "replay_v2.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

#define TEST_ASSERT(x) do { if (!(x)) throw std::runtime_error("assertion failed: " #x); } while(false)

int main() {
    try {
        OrderBook book(16, true);
        book.process(NewOrderCommand(CommandSequence(1), OrderId(1), Side::SELL,
                                     Price(1000000), Quantity(3)));
        book.process(NewOrderCommand(CommandSequence(2), OrderId(2), Side::SELL,
                                     Price(1010000), Quantity(5)));

        const ProcessResult post_only = book.process(NewOrderCommand(
            CommandSequence(3), OrderId(3), Side::BUY, Price(1000000), Quantity(1),
            TimeInForce::POST_ONLY));
        TEST_ASSERT(post_only.status == ProcessStatus::REJECTED);
        TEST_ASSERT(book.active_order_count() == 2);
        const auto& rejected = std::get<OrderRejectedEvent>(book.engine_events().back());
        TEST_ASSERT(rejected.reason == RejectReason::POST_ONLY_WOULD_TRADE);

        const ProcessResult resting_post_only = book.process(NewOrderCommand(
            CommandSequence(4), OrderId(4), Side::BUY, Price(990000), Quantity(2),
            TimeInForce::POST_ONLY));
        TEST_ASSERT(resting_post_only.status == ProcessStatus::APPLIED);
        TEST_ASSERT(book.active_order_count() == 3);

        const size_t begin = book.engine_events().size();
        const ProcessResult ioc = book.process(NewOrderCommand(
            CommandSequence(5), OrderId(5), Side::BUY, Price(1010000), Quantity(10),
            TimeInForce::IOC));
        TEST_ASSERT(ioc.status == ProcessStatus::APPLIED);
        TEST_ASSERT(ioc.event_count == 3); // Two trades, then residual cancel.
        TEST_ASSERT(std::holds_alternative<OrderCancelledEvent>(
            book.engine_events()[begin + 2]));
        const auto& cancelled = std::get<OrderCancelledEvent>(book.engine_events()[begin + 2]);
        TEST_ASSERT(cancelled.cancelled_quantity == Quantity(2));
        TEST_ASSERT(book.active_order_count() == 1); // Only resting post-only bid remains.
        TEST_ASSERT(book.check_invariants());

        const SystemEvent event = SystemEvent::order(Timestamp(10), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(6), OrderId(6), Side::BUY,
                            Price(900000), Quantity(1), TimeInForce::IOC));
        const auto record = JournalProjector::project(event, 1);
        TEST_ASSERT(record && record->time_in_force == TimeInForce::IOC);

        const std::string journal_path = "order_type_replay_test.csv";
        MarketManager source(16);
        EventDispatcher source_dispatcher;
        Journal journal;
        journal.attach(source_dispatcher);
        TradingEngine source_engine(source, source_dispatcher);
        source_engine.submit(Timestamp(1), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(1), OrderId(100), Side::SELL,
                            Price(1000000), Quantity(5)));
        source_engine.submit(Timestamp(2), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(2), OrderId(101), Side::BUY,
                            Price(1000000), Quantity(7), TimeInForce::IOC));
        journal.save(journal_path);
        const auto loaded = Journal::load(journal_path);
        bool loaded_ioc = false;
        for (const JournalRecord& item : loaded) {
            if (item.type == JournalRecordType::ORDER_INSERT &&
                item.order_id == OrderId(101)) {
                loaded_ioc = item.time_in_force == TimeInForce::IOC;
            }
        }
        TEST_ASSERT(loaded_ioc);
        MarketManager replayed(16);
        EventDispatcher replay_dispatcher;
        TradingEngine replay_engine(replayed, replay_dispatcher);
        const JournalReplayResult replay_result = JournalReplayEngine::replay(
            loaded, replay_engine, replay_dispatcher, &source);
        TEST_ASSERT(replay_result.success());
        std::error_code ignored;
        std::filesystem::remove(journal_path, ignored);
        std::cout << "Order type tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Order type tests failed: " << error.what() << '\n';
        return 1;
    }
}
