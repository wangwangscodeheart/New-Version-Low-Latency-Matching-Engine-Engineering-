#ifndef PUBLIC_FEED_BENCHMARK_HPP
#define PUBLIC_FEED_BENCHMARK_HPP

#include "async_journal.hpp"
#include "async_logger.hpp"
#include "monitor.hpp"
#include "public_feed_publisher.hpp"
#include "trading_engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

// Synthetic engineering baseline for the existing in-process public loop.
// This is not an exchange-data, transport, durability or allocation benchmark.
namespace public_feed_benchmark {

struct Stats {
    uint64_t commands = 0;
    uint64_t public_messages = 0;
    uint64_t wire_bytes = 0;
    uint64_t visible_commands = 0;
    uint64_t no_visible_commands = 0;
    size_t peak_pending_bytes = 0;
    size_t peak_pending_batches = 0;
    size_t peak_pending_messages = 0;
};

namespace detail {
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

inline size_t multiply(size_t value, size_t factor) {
    if (value > std::numeric_limits<size_t>::max() / factor ||
        value > std::numeric_limits<uint64_t>::max() / factor) {
        throw std::overflow_error("Public feed benchmark workload overflow");
    }
    return value * factor;
}

struct Configuration {
    size_t cycles;
    size_t drain_every;
    size_t groups;
    size_t commands;
    size_t messages;
    size_t bytes;
    size_t visible;
    size_t record_slots;
    size_t journal_records;
    size_t log_events;

    Configuration(size_t cycles_per_symbol, size_t drain_interval)
        : cycles(cycles_per_symbol), drain_every(drain_interval), groups(0),
          commands(0), messages(0), bytes(0), visible(0), record_slots(0),
          journal_records(0), log_events(0) {
        if (cycles == 0 || drain_every == 0) {
            throw std::invalid_argument("Public feed benchmark parameters must be positive");
        }
        groups = multiply(cycles, size_t{3});
        commands = multiply(groups, size_t{8});
        messages = multiply(groups, size_t{9});
        bytes = multiply(groups, size_t{428});
        visible = multiply(groups, size_t{7});
        // At most two passive orders exist. Four reserved journal records per
        // command cover the ENTIRE workload even if the writer never runs.
        record_slots = multiply(commands, size_t{4});
        journal_records = multiply(groups, size_t{19});
        log_events = multiply(groups, size_t{26});
    }
};

// Each fixture exclusively creates one unpredictable child under the resolved
// temporary directory. Only that owned child may be removed on teardown.
class TemporaryDirectory {
    std::filesystem::path parent_;
    std::filesystem::path directory_;
    bool owned_ = false;
public:
    TemporaryDirectory()
        : parent_(std::filesystem::canonical(std::filesystem::temp_directory_path())) {
        static std::atomic<uint64_t> serial{0};
        std::random_device random;
        for (size_t attempt = 0; attempt < 100; ++attempt) {
            const std::string suffix = std::to_string(random()) + "-" +
                std::to_string(random()) + "-" +
                std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
            const auto candidate = parent_ / ("public-feed-benchmark-" + suffix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = candidate;
                owned_ = true;
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error(
                    "Cannot create public feed benchmark temporary directory", candidate, error);
            }
        }
        throw std::runtime_error("Cannot exclusively create benchmark temporary directory");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() noexcept {
        if (!owned_ || directory_.parent_path() != parent_ ||
            directory_.filename().string().find("public-feed-benchmark-") != 0) return;
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    std::string file(const char* name) const { return (directory_ / name).string(); }
};

// Do not decode twice to collect byte counts: the real gateway adapter alone
// validates and decodes each command batch.
class CountingFeedByteSink final : public FeedByteSink {
    GatewayFeedSink& adapter_;
    uint64_t accepted_bytes_ = 0;
public:
    explicit CountingFeedByteSink(GatewayFeedSink& adapter) : adapter_(adapter) {}
    bool consume(const uint8_t* bytes, size_t size) override {
        if (size > std::numeric_limits<uint64_t>::max() - accepted_bytes_) return false;
        if (!adapter_.consume(bytes, size)) return false;
        accepted_bytes_ += static_cast<uint64_t>(size);
        return true;
    }
    void invalidate() noexcept override { adapter_.invalidate(); }
    uint64_t accepted_bytes() const noexcept { return accepted_bytes_; }
};
} // namespace detail

class Fixture {
    using Clock = std::chrono::steady_clock;
    const detail::Configuration config_;
    const bool verify_each_drain_;
    const bool diagnostics_;
    detail::TemporaryDirectory temporary_;
    // Capacity 16 is intentional: this workload has at most two live orders
    // per symbol, and measures the public loop rather than production sizing.
    MarketManager markets_{16};
    EventDispatcher dispatcher_;
    AsyncJournalWriter journal_;
    MarketDataGateway gateway_{3};
    GatewayFeedSink adapter_{gateway_};
    detail::CountingFeedByteSink counting_sink_{adapter_};
    PublicFeedPublisher publisher_;
    TradingEngine engine_;
    Monitor monitor_;
    std::unique_ptr<AsyncLogger> logger_;
    Stats stats_;
    std::array<uint64_t, 3> command_sequences_{{0, 0, 0}};
    std::array<uint64_t, 3> public_sequences_{{0, 0, 0}};
    std::vector<Clock::time_point> pending_starts_;
    bool used_ = false;
    bool completed_ = false;
    bool failed_ = false;
    bool stopped_ = false;

    void shutdown() noexcept {
        if (stopped_) return;
        stopped_ = true;
        if (logger_) logger_->stop();
        journal_.stop();
    }

    void require_running_health() const {
        detail::require(!engine_.failed() && publisher_.available() && adapter_.available(),
                        "Public feed benchmark output is unavailable");
        detail::require(journal_.available() && journal_.dropped_count() == 0,
                        "Public feed benchmark journal is unhealthy");
        if (logger_) {
            detail::require(logger_->available() && logger_->dropped_count() == 0 &&
                            logger_->failed_count() == 0,
                            "Public feed benchmark logger is unhealthy");
        }
    }

    void compare_visible_books() const {
        using VisibleOrder = std::tuple<uint8_t, int64_t, uint64_t>;
        using LevelKey = std::pair<uint8_t, int64_t>;
        for (size_t index = 0; index < command_sequences_.size(); ++index) {
            const InstrumentId id(static_cast<uint32_t>(index));
            const auto source = markets_.find_book(id)->capture_state();
            const auto* local_book = gateway_.book(id);
            const auto local = local_book->capture_state();
            std::vector<VisibleOrder> source_orders;
            std::vector<VisibleOrder> local_orders;
            std::map<LevelKey, uint64_t> source_levels;
            std::map<LevelKey, uint64_t> local_levels;
            const auto collect = [&](const std::vector<BookLevelState>& levels) {
                for (const auto& level : levels) {
                    const LevelKey key{static_cast<uint8_t>(level.side), level.price.get()};
                    source_levels.emplace(key, level.total_volume.get());
                    for (const auto& order : level.orders) {
                        source_orders.emplace_back(static_cast<uint8_t>(order.side),
                            order.price.get(), order.remaining_quantity.get());
                    }
                }
            };
            collect(source.bid_levels);
            collect(source.ask_levels);
            for (const auto& order : local.orders) {
                local_orders.emplace_back(static_cast<uint8_t>(order.side),
                    order.price.get(), order.remaining.get());
                local_levels[{static_cast<uint8_t>(order.side), order.price.get()}] +=
                    order.remaining.get();
            }
            std::sort(source_orders.begin(), source_orders.end());
            std::sort(local_orders.begin(), local_orders.end());
            detail::require(source_orders == local_orders && source_levels == local_levels &&
                            source.active_order_count == local_book->order_count(),
                            "Public feed benchmark visible book projection diverged");
            for (const auto& level : source_levels) {
                const Price price(level.first.second);
                const uint64_t quantity = level.first.first == static_cast<uint8_t>(Side::BUY)
                    ? local_book->bid_quantity(price) : local_book->ask_quantity(price);
                detail::require(quantity == level.second,
                                "Public feed benchmark public price levels diverged");
            }
            detail::require(gateway_.state(id) == FeedState::HEALTHY &&
                            gateway_.last_sequence(id) == public_sequences_[index],
                            "Public feed benchmark consumer sequence diverged");
        }
    }

    void drain(bool sample, std::vector<uint64_t>* latencies) {
        detail::require(publisher_.drain(), "Public feed benchmark drain failed");
        // The endpoint is the drain return which covers each visible command,
        // before validation, bookkeeping, and vector insertion overhead.
        if (sample && !pending_starts_.empty()) {
            const auto finish = Clock::now();
            for (const auto& start : pending_starts_) {
                latencies->push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count()));
            }
            pending_starts_.clear();
        }
        stats_.wire_bytes = counting_sink_.accepted_bytes();
        detail::require(publisher_.pending_bytes() == 0 && publisher_.pending_batches() == 0 &&
                        adapter_.pending_bytes() == 0 &&
                        gateway_.metrics().applied == stats_.public_messages,
                        "Public feed benchmark drain accounting diverged");
        require_running_health();
        if (verify_each_drain_) compare_visible_books();
    }

    void verify_complete() const {
        detail::require(completed_ && !failed_, "Public feed benchmark did not complete");
        detail::require(stats_.commands == static_cast<uint64_t>(config_.commands) &&
                        stats_.public_messages == static_cast<uint64_t>(config_.messages) &&
                        stats_.wire_bytes == static_cast<uint64_t>(config_.bytes) &&
                        stats_.visible_commands == static_cast<uint64_t>(config_.visible) &&
                        stats_.no_visible_commands == static_cast<uint64_t>(config_.groups),
                        "Public feed benchmark final counters diverged");
        const auto& metrics = gateway_.metrics();
        detail::require(metrics.applied == stats_.public_messages && metrics.gaps == 0 &&
                        metrics.duplicates == 0 && metrics.out_of_order == 0 &&
                        metrics.invalid_instruments == 0 && metrics.invalid_messages == 0 &&
                        metrics.stale_messages == 0 && publisher_.available() &&
                        adapter_.available() && publisher_.pending_bytes() == 0 &&
                        publisher_.pending_batches() == 0 && adapter_.pending_bytes() == 0 &&
                        pending_starts_.empty() && !engine_.failed(),
                        "Public feed benchmark final consumer is unhealthy");
        for (size_t index = 0; index < command_sequences_.size(); ++index) {
            const InstrumentId id(static_cast<uint32_t>(index));
            const auto* source = markets_.find_book(id);
            const auto* local = gateway_.book(id);
            detail::require(source->active_order_count() == 0 && !source->best_bid() &&
                            !source->best_ask() && source->ready_for_commands() &&
                            source->last_applied_command_sequence().get() ==
                                static_cast<uint64_t>(config_.cycles) * 8 &&
                            local->order_count() == 0 &&
                            local->bid_quantity(Price(999000)) == 0 &&
                            local->bid_quantity(Price(1000000)) == 0 &&
                            local->ask_quantity(Price(999000)) == 0 &&
                            local->ask_quantity(Price(1000000)) == 0 &&
                            gateway_.state(id) == FeedState::HEALTHY &&
                            gateway_.last_sequence(id) == static_cast<uint64_t>(config_.cycles) * 9,
                            "Public feed benchmark final books are not empty and contiguous");
        }
        detail::require(journal_.accepted_command_count() == stats_.commands &&
                        journal_.accepted_count() == static_cast<uint64_t>(config_.journal_records),
                        "Public feed benchmark journal acceptance diverged");
        if (diagnostics_) {
            const auto metrics_snapshot = monitor_.snapshot(logger_.get());
            const auto& global = metrics_snapshot.global;
            const uint64_t groups = static_cast<uint64_t>(config_.groups);
            detail::require(global.orders == groups * 6 && global.trades == groups * 3 &&
                            global.cancels == groups * 3 && global.rejects == 0 &&
                            global.traded_volume == groups * 15 &&
                            global.latency_samples == stats_.commands &&
                            global.data_warnings == 0 && global.data_errors == 0 &&
                            logger_->accepted_count() == static_cast<uint64_t>(config_.log_events),
                            "Public feed benchmark diagnostics counters diverged");
        }
    }

public:
    Fixture(size_t cycles_per_symbol, size_t drain_every, bool verify_each_drain,
            bool diagnostics = false)
        : config_(cycles_per_symbol, drain_every), verify_each_drain_(verify_each_drain),
          diagnostics_(diagnostics), journal_(temporary_.file("commands.csv"), config_.record_slots),
          publisher_(markets_, counting_sink_, config_.bytes),
          engine_(markets_, dispatcher_, journal_, publisher_) {
        if (diagnostics_) {
            monitor_.attach(dispatcher_, &markets_);
            // Four events per command cover all 26 diagnostic events per cycle.
            logger_ = std::make_unique<AsyncLogger>(temporary_.file("diagnostics.csv"),
                                                  config_.record_slots);
            logger_->attach(dispatcher_);
        }
        dispatcher_.freeze();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() { shutdown(); }

    size_t operation_count() const noexcept { return config_.commands; }
    Stats stats() const { return stats_; }

    uint64_t run(bool sample, std::vector<uint64_t>* latencies) {
        if (used_ || stopped_) throw std::logic_error("Public feed benchmark fixture is single-use");
        used_ = true;
        try {
            if (sample && !latencies) {
                throw std::invalid_argument("Sampled public feed benchmark requires a latency vector");
            }
            if (sample) {
                if (config_.visible > latencies->max_size() - latencies->size()) {
                    throw std::overflow_error("Public feed benchmark latency vector overflow");
                }
                latencies->reserve(latencies->size() + config_.visible);
                pending_starts_.reserve(std::min(config_.drain_every, config_.commands));
            }
            uint64_t checksum = 0;
            uint64_t order_id = 0;
            size_t since_drain = 0;
            for (size_t cycle = 0; cycle < config_.cycles; ++cycle) {
                for (size_t index = 0; index < command_sequences_.size(); ++index) {
                    const InstrumentId id(static_cast<uint32_t>(index));
                    const uint64_t a = ++order_id;
                    const uint64_t b = ++order_id;
                    const uint64_t c = ++order_id;
                    const uint64_t d = ++order_id;
                    const uint64_t e = ++order_id;
                    const uint64_t ioc = ++order_id;
                    const auto execute = [&](const Command& command, uint64_t public_messages) {
                        if (sample && public_messages != 0) pending_starts_.push_back(Clock::now());
                        const auto result = engine_.submit(Timestamp(stats_.commands + 1), id, command);
                        detail::require(result.routed() && result.process_result.applied() &&
                                        result.process_result.event_output_complete,
                                        "Public feed benchmark command/output failed");
                        ++stats_.commands;
                        stats_.public_messages += public_messages;
                        public_sequences_[index] += public_messages;
                        if (public_messages != 0) ++stats_.visible_commands;
                        else ++stats_.no_visible_commands;
                        stats_.peak_pending_bytes = std::max(stats_.peak_pending_bytes,
                                                             publisher_.pending_bytes());
                        stats_.peak_pending_batches = std::max(stats_.peak_pending_batches,
                                                               publisher_.pending_batches());
                        const auto pending = stats_.public_messages - gateway_.metrics().applied;
                        stats_.peak_pending_messages = std::max(stats_.peak_pending_messages,
                                                                static_cast<size_t>(pending));
                        checksum = checksum * 1099511628211ULL ^
                            (get_command_sequence(command).get() << 8) ^
                            (static_cast<uint64_t>(index) << 4) ^
                            static_cast<uint64_t>(result.process_result.event_count) ^ public_messages;
                        require_running_health();
                        ++since_drain;
                        if (since_drain == config_.drain_every) {
                            drain(sample, latencies);
                            since_drain = 0;
                        }
                    };
                    const auto order = [&](uint64_t reference, Side side, int64_t price,
                                           uint64_t quantity, TimeInForce tif = TimeInForce::GTC) {
                        return Command{NewOrderCommand(CommandSequence(++command_sequences_[index]),
                            OrderId(reference), side, Price(price), Quantity(quantity), tif)};
                    };
                    const auto cancel = [&](uint64_t reference) {
                        return Command{CancelOrderCommand(CommandSequence(++command_sequences_[index]),
                                                          OrderId(reference))};
                    };
                    execute(order(a, Side::SELL, 1000000, 10), 1);
                    execute(order(b, Side::SELL, 1000000, 6), 1);
                    execute(order(c, Side::BUY, 1000000, 12), 2);
                    execute(cancel(b), 1);
                    execute(order(d, Side::BUY, 999000, 3), 1);
                    execute(order(e, Side::SELL, 999000, 5), 2);
                    execute(cancel(e), 1);
                    execute(order(ioc, Side::BUY, 999000, 1, TimeInForce::IOC), 0);
                }
            }
            // Also flush an incomplete drain interval, including its samples.
            if (since_drain != 0) drain(sample, latencies);
            completed_ = true;
            require_running_health();
            return checksum;
        } catch (...) {
            failed_ = true;
            shutdown();
            throw;
        }
    }

    void stop() {
        shutdown();
        if (!used_) return;
        verify_complete();
        detail::require(journal_.healthy() && journal_.dropped_count() == 0 &&
                        journal_.buffered_record_slots() == 0 &&
                        journal_.written_command_count() == stats_.commands &&
                        journal_.written_count() == static_cast<uint64_t>(config_.journal_records),
                        "Public feed benchmark journal did not finish healthily");
        if (logger_) {
            detail::require(logger_->healthy() && logger_->dropped_count() == 0 &&
                            logger_->failed_count() == 0 &&
                            logger_->accepted_count() == logger_->written_count(),
                            "Public feed benchmark logger did not finish healthily");
        }
    }
};
} // namespace public_feed_benchmark

#endif
