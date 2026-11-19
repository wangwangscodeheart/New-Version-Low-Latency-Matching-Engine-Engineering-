#include "async_logger.hpp"
#include "async_journal.hpp"
#include "audit_log.hpp"
#include "journal.hpp"
#include "market_data_generator.hpp"
#include "market_data_loader.hpp"
#include "monitor.hpp"
#include "public_feed_publisher.hpp"
#include "replay_v2.hpp"
#include "trading_engine.hpp"
#include <iostream>

int main() {
    try {
        MarketManager markets(1024);
        EventDispatcher dispatcher;
        AuditLog audit;
        Monitor monitor;
        AsyncLogger async_logger("trading_v2_async.log", 4096);
        AsyncJournalWriter async_journal("trading_v2.journal.csv", 4096);
        MarketDataGateway local_books(markets.symbol_count());
        GatewayFeedSink feed_sink(local_books);
        PublicFeedPublisher publisher(markets, feed_sink, 65536);

        audit.attach(dispatcher);
        monitor.attach(dispatcher, &markets);
        async_logger.attach(dispatcher);
        TradingEngine engine(markets, dispatcher, async_journal, publisher);
        const auto submit = [&](Timestamp timestamp, const Symbol& symbol, const Command& command) {
            const auto result = engine.submit(timestamp, symbol, command);
            if (!result.process_result.applied() || !result.process_result.event_output_complete ||
                    !publisher.drain())
                throw std::runtime_error("Command/public feed did not complete");
        };

        MarketDataGeneratorConfig generator_config;
        generator_config.ticks_per_symbol = 20;
        const std::string market_data_file = "synthetic_market_data.csv";
        const size_t generated_rows =
            MarketDataGenerator::generate_csv(market_data_file, generator_config);
        DataValidator validator(dispatcher);
        const MarketDataLoadResult market_data =
            MarketDataLoader::load(market_data_file, validator, engine);
        if (market_data.published != generated_rows || market_data.rejected != 0) {
            throw std::runtime_error("Synthetic market data did not load cleanly");
        }

        submit(Timestamp(1010), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(1), OrderId(10001), Side::SELL,
                            from_double(190.00), Quantity(100)));
        submit(Timestamp(1011), Symbol("TSLA"),
            NewOrderCommand(CommandSequence(1), OrderId(10001), Side::BUY,
                            from_double(250.00), Quantity(50)));
        submit(Timestamp(1012), Symbol("NVDA"),
            NewOrderCommand(CommandSequence(1), OrderId(10001), Side::BUY,
                            from_double(130.00), Quantity(75)));
        submit(Timestamp(1020), Symbol("AAPL"),
            NewOrderCommand(CommandSequence(2), OrderId(10002), Side::BUY,
                            from_double(190.00), Quantity(40)));
        submit(Timestamp(1030), Symbol("AAPL"),
            CancelOrderCommand(CommandSequence(3), OrderId(10001)));

        for (uint32_t i = 0; i < markets.symbol_count(); ++i) {
            const InstrumentId id(i);
            if (local_books.state(id) != FeedState::HEALTHY ||
                    local_books.book(id)->order_count() != markets.find_book(id)->active_order_count())
                throw std::runtime_error("Public/local book mismatch");
        }
        if (local_books.book(markets.resolve(Symbol("TSLA")))->bid_quantity(from_double(250.00)) != 50 ||
                local_books.book(markets.resolve(Symbol("NVDA")))->bid_quantity(from_double(130.00)) != 75)
            throw std::runtime_error("Public/local depth mismatch");
        std::cout << "Public feed: instruments=3 applied=" << local_books.metrics().applied
                  << " pending_bytes=" << publisher.pending_bytes() << " result=PASS\n";

        async_logger.stop();
        async_journal.stop();
        if (!async_logger.healthy() || async_logger.dropped_count() != 0 || async_logger.failed_count() != 0) {
            monitor.print(std::cout, &async_logger);
            throw std::runtime_error("Async event logger did not drain cleanly");
        }
        if (!async_journal.healthy()) {
            throw std::runtime_error("Async journal did not drain cleanly");
        }
        audit.save("trading_v2.audit.csv");
        monitor.print(std::cout, &async_logger);

        const auto loaded = Journal::load("trading_v2.journal.csv");
        MarketManager replay_markets(1024);
        EventDispatcher replay_dispatcher;
        TradingEngine replay_engine(replay_markets, replay_dispatcher);
        const JournalReplayResult replay = JournalReplayEngine::replay(
            loaded, replay_engine, replay_dispatcher, &markets);

        bool states_equal = replay.success();
        for (const char* value : {"AAPL", "TSLA", "NVDA"}) {
            const Symbol symbol(value);
            states_equal = states_equal &&
                markets.find_book(symbol)->capture_state() ==
                replay_markets.find_book(symbol)->capture_state();
        }

        std::cout << "Replay: commands=" << replay.replayed_commands
                  << " expected_trades=" << replay.expected_trades
                  << " actual_trades=" << replay.actual_trades
                  << " result=" << (states_equal ? "PASS" : "FAIL") << '\n';
        std::cout << "Market data: generated=" << generated_rows
                  << " published=" << market_data.published
                  << " rejected=" << market_data.rejected << '\n';
        std::cout << "Files: synthetic_market_data.csv, trading_v2.journal.csv, "
                     "trading_v2.audit.csv, trading_v2_async.log\n";
        return states_equal ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "V2 demo failed: " << error.what() << '\n';
        return 1;
    }
}
