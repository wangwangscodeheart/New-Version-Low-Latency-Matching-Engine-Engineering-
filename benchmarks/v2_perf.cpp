#include "async_journal.hpp"
#include "async_logger.hpp"
#include "event_sink.hpp"
#include "monitor.hpp"
#include "trading_engine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef MATCHING_ENGINE_GIT_COMMIT
#define MATCHING_ENGINE_GIT_COMMIT "unknown"
#endif
#ifndef MATCHING_ENGINE_BUILD_TYPE
#define MATCHING_ENGINE_BUILD_TYPE "unknown"
#endif
#ifndef MATCHING_ENGINE_COMPILER
#define MATCHING_ENGINE_COMPILER "unknown"
#endif
#ifndef MATCHING_ENGINE_OS
#define MATCHING_ENGINE_OS "unknown"
#endif
#ifndef MATCHING_ENGINE_COMPILER_FLAGS
#define MATCHING_ENGINE_COMPILER_FLAGS "unknown"
#endif

namespace {
#ifndef MATCHING_ENGINE_V2_BENCHMARK_PAIRS
#define MATCHING_ENGINE_V2_BENCHMARK_PAIRS 10000
#endif
constexpr size_t PAIRS_PER_SYMBOL = MATCHING_ENGINE_V2_BENCHMARK_PAIRS;
constexpr size_t COMMAND_COUNT = PAIRS_PER_SYMBOL * 2 * 3;
constexpr size_t BATCH_RUNS = 5;

struct Result {
    std::string scenario;
    double batch_ns = 0;
    double throughput = 0;
    uint64_t median_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t p999_ns = 0;
    uint64_t max_ns = 0;
    uint64_t checksum = 0;
};

std::string format_latency(double ns) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    if (ns >= 1'000'000.0) {
        out << ns / 1'000'000.0 << " ms";
    } else if (ns >= 1'000.0) {
        out << ns / 1'000.0 << " us";
    } else {
        out << ns << " ns";
    }

    return out.str();
}

std::string format_throughput(double throughput) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);

    if (throughput >= 1'000'000.0) {
        out << throughput / 1'000'000.0 << " M ops/s";
    } else if (throughput >= 1'000.0) {
        out << throughput / 1'000.0 << " K ops/s";
    } else {
        out << throughput << " ops/s";
    }

    return out.str();
}

struct ScenarioInfo {
    std::string title;
    std::string description_en;
    std::string description_zh;
};

ScenarioInfo scenario_info(const std::string& scenario) {
    if (scenario == "core_pre_resolved_instrument") {
        return {
            "Core Matching / 核心撮合路径",
            "Pre-resolved instrument, core matching path",
            "已预解析交易品种，仅测试核心撮合路径"
        };
    }

    if (scenario == "core_plus_routing") {
        return {
            "Routing / 多品种路由",
            "Core matching with symbol routing",
            "核心撮合 + 多品种路由"
        };
    }

    if (scenario == "dispatcher") {
        return {
            "Event Dispatcher / 事件分发",
            "Trading engine with synchronous event dispatch",
            "交易引擎 + 同步事件分发"
        };
    }

    if (scenario == "dispatcher_monitor") {
        return {
            "Monitor / 监控",
            "Dispatcher with monitoring enabled",
            "事件分发 + 监控"
        };
    }

    if (scenario == "dispatcher_async_logger") {
        return {
            "Async Logger / 异步日志",
            "Dispatcher with asynchronous logging",
            "事件分发 + 异步日志"
        };
    }

    if (scenario == "dispatcher_async_journal") {
        return {
            "Async Journal / 异步 Journal",
            "Dispatcher with asynchronous journal",
            "事件分发 + 异步 Journal"
        };
    }

    if (scenario == "full_v2") {
        return {
            "Full V2 / 完整 V2 路径",
            "Monitor + async logger + async journal",
            "监控 + 异步日志 + 异步 Journal"
        };
    }

    return {
        "Unknown Scenario / 未知场景",
        scenario,
        "暂无场景说明"
    };
}

void print_human_result(const Result& result, size_t index, size_t total) {
    const ScenarioInfo info = scenario_info(result.scenario);

    std::cout
        << '[' << index << '/' << total << "] " << info.title << '\n'
        << "------------------------------------------------------------\n"
        << "  Scenario / 场景\n"
        << "    " << result.scenario << '\n'
        << "  Description / 说明\n"
        << "    EN   : " << info.description_en << '\n'
        << "    中文 : " << info.description_zh << "\n\n"
        << "  Batch Median per Op / 批次中位单操作耗时 : "
        << format_latency(result.batch_ns) << '\n'
        << "  Throughput / 吞吐量                      : "
        << format_throughput(result.throughput) << "\n\n"
        << "  Sampled Latency / 单操作采样延迟\n"
        << "    P50   : " << format_latency(result.median_ns) << '\n'
        << "    P99   : " << format_latency(result.p99_ns) << '\n'
        << "    P99.9 : " << format_latency(result.p999_ns) << '\n'
        << "    Max   : " << format_latency(result.max_ns) << "\n\n";
}

std::string environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) return "unknown";
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? value : "unknown";
#endif
}

template<typename Submit>
uint64_t run_workload(Submit&& submit, bool sample,
                      std::vector<uint64_t>* latencies = nullptr) {
    const std::array<Symbol, 3> symbols{
        Symbol("AAPL"), Symbol("TSLA"), Symbol("NVDA")};
    std::array<uint64_t, 3> sequences{0, 0, 0};
    uint64_t order_id = 1;
    uint64_t timestamp = 1;
    uint64_t checksum = 0;
    for (size_t i = 0; i < PAIRS_PER_SYMBOL; ++i) {
        for (size_t index = 0; index < symbols.size(); ++index) {
            const Symbol& symbol = symbols[index];
            const auto execute = [&](const Command& command) {
                if (sample) {
                    const auto start = std::chrono::steady_clock::now();
                    const MarketProcessResult result =
                        submit(Timestamp(timestamp++), symbol, index, command);
                    const auto finish = std::chrono::steady_clock::now();
                    latencies->push_back(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            finish - start).count()));
                    checksum = checksum * 1099511628211ULL ^
                        (get_command_sequence(command).get() << 8) ^
                        static_cast<uint64_t>(result.process_result.event_count +
                            static_cast<size_t>(result.process_result.status));
                } else {
                    const MarketProcessResult result =
                        submit(Timestamp(timestamp++), symbol, index, command);
                    checksum = checksum * 1099511628211ULL ^
                        (get_command_sequence(command).get() << 8) ^
                        static_cast<uint64_t>(result.process_result.event_count +
                            static_cast<size_t>(result.process_result.status));
                }
            };
            execute(NewOrderCommand(CommandSequence(++sequences[index]),
                OrderId(order_id), Side::BUY, from_double(100), Quantity(1)));
            execute(CancelOrderCommand(CommandSequence(++sequences[index]),
                                       OrderId(order_id)));
            ++order_id;
        }
    }
    return checksum;
}

template<typename Factory>
Result measure(const std::string& name, Factory&& factory) {
    // Separate batch and sampled runs so clock/vector overhead never contaminates
    // the throughput measurement.
    auto warmup_runner = factory("warmup");
    const uint64_t expected_checksum = warmup_runner->run(false, nullptr);
    warmup_runner->stop();

    std::vector<double> batch_times;
    batch_times.reserve(BATCH_RUNS);
    for (size_t run = 0; run < BATCH_RUNS; ++run) {
        auto batch_runner = factory("batch_" + std::to_string(run));
        const auto start = std::chrono::steady_clock::now();
        const uint64_t batch_checksum = batch_runner->run(false, nullptr);
        const auto finish = std::chrono::steady_clock::now();
        batch_runner->stop();
        if (batch_checksum != expected_checksum) {
            throw std::runtime_error("Benchmark batch states diverged");
        }
        batch_times.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count()));
    }
    std::sort(batch_times.begin(), batch_times.end());
    const double elapsed_ns = batch_times[batch_times.size() / 2];

    auto sampled_runner = factory("sampled");
    std::vector<uint64_t> latencies;
    latencies.reserve(COMMAND_COUNT);
    const uint64_t sampled_checksum = sampled_runner->run(true, &latencies);
    sampled_runner->stop();
    if (expected_checksum != sampled_checksum) {
        throw std::runtime_error("Batch and sampled benchmark states diverged");
    }
    std::sort(latencies.begin(), latencies.end());
    return Result{name, elapsed_ns / COMMAND_COUNT,
        static_cast<double>(COMMAND_COUNT) * 1e9 / elapsed_ns,
        latencies[latencies.size() / 2], latencies[latencies.size() * 99 / 100],
        latencies[latencies.size() * 999 / 1000], latencies.back(),
        expected_checksum};
}

class Runner {
public:
    virtual ~Runner() = default;
    virtual uint64_t run(bool sample, std::vector<uint64_t>* latencies) = 0;
    virtual void stop() {}
};

template<typename Submit>
class SubmitRunner : public Runner {
    Submit submit_;
public:
    explicit SubmitRunner(Submit submit) : submit_(std::move(submit)) {}
    uint64_t run(bool sample, std::vector<uint64_t>* latencies) override {
        return run_workload(submit_, sample, latencies);
    }
};

struct RoutedFixture {
    MarketManager markets{PAIRS_PER_SYMBOL + 16};
    NullEventSink sink;
    MarketProcessResult submit(Timestamp, const Symbol& symbol, size_t,
                               const Command& command) {
        return markets.process(symbol, command, sink);
    }
    void stop() noexcept {}
};

struct CoreFixture {
    MarketManager markets{PAIRS_PER_SYMBOL + 16};
    std::array<InstrumentId, 3> instruments;
    NullEventSink sink;
    CoreFixture() {
        const std::array<Symbol, 3> symbols{
            Symbol("AAPL"), Symbol("TSLA"), Symbol("NVDA")};
        for (size_t i = 0; i < symbols.size(); ++i) {
            instruments[i] = markets.resolve(symbols[i]);
        }
    }
    MarketProcessResult submit(Timestamp, const Symbol&, size_t instrument,
                               const Command& command) {
        return markets.process(instruments[instrument], command, sink);
    }
    void stop() noexcept {}
};

struct EngineFixture {
    MarketManager markets{PAIRS_PER_SYMBOL + 16};
    EventDispatcher dispatcher;
    Monitor monitor;
    std::unique_ptr<AsyncLogger> logger;
    std::unique_ptr<AsyncJournalWriter> journal;
    std::string logger_path;
    std::string journal_path;
    TradingEngine engine{markets, dispatcher};

    EngineFixture(bool with_monitor, bool with_logger, bool with_journal,
                  const std::string& suffix) {
        if (with_monitor) monitor.attach(dispatcher, &markets);
        if (with_logger) {
            logger_path = "benchmark_" + suffix + ".log";
            logger = std::make_unique<AsyncLogger>(logger_path, 262144);
            logger->attach(dispatcher);
        }
        if (with_journal) {
            journal_path = "benchmark_" + suffix + ".journal.csv";
            journal = std::make_unique<AsyncJournalWriter>(
                journal_path, 262144);
            journal->attach(dispatcher);
        }
        dispatcher.freeze();
    }
    MarketProcessResult submit(Timestamp timestamp, const Symbol& symbol, size_t,
                               const Command& command) {
        return engine.submit(timestamp, symbol, command);
    }
    void stop() {
        if (logger) logger->stop();
        if (journal) journal->stop();
        std::error_code ignored;
        if (!logger_path.empty()) std::filesystem::remove(logger_path, ignored);
        if (!journal_path.empty()) std::filesystem::remove(journal_path, ignored);
    }
};

template<typename Fixture>
std::unique_ptr<Runner> wrap(std::shared_ptr<Fixture> fixture) {
    auto submit = [fixture](Timestamp timestamp, const Symbol& symbol, size_t instrument,
                            const Command& command) {
        return fixture->submit(timestamp, symbol, instrument, command);
    };
    class FixtureRunner final : public SubmitRunner<decltype(submit)> {
        std::shared_ptr<Fixture> fixture_;
    public:
        FixtureRunner(std::shared_ptr<Fixture> fixture, decltype(submit) fn)
            : SubmitRunner<decltype(submit)>(std::move(fn)), fixture_(std::move(fixture)) {}
        void stop() override { fixture_->stop(); }
    };
    return std::make_unique<FixtureRunner>(std::move(fixture), std::move(submit));
}
}

int main()
{
    const bool raw_output = environment("MATCHING_ENGINE_RAW_OUTPUT") == "1";

    std::cout
        << "\n"
        << "============================================================\n"
        << "        V2 PERFORMANCE BENCHMARK\n"
        << "        V2 多品种撮合引擎性能基准测试\n"
        << "============================================================\n\n"
        << "[Environment / 测试环境]\n"
        << "  Git Commit        : " << MATCHING_ENGINE_GIT_COMMIT << '\n'
        << "  OS                : " << MATCHING_ENGINE_OS << '\n'
        << "  Compiler          : " << MATCHING_ENGINE_COMPILER << '\n'
        << "  Compiler Flags    : " << MATCHING_ENGINE_COMPILER_FLAGS << '\n'
        << "  Build Type        : " << MATCHING_ENGINE_BUILD_TYPE << '\n'
        << "  Warmup Operations : " << COMMAND_COUNT << '\n'
        << "  Measured Ops      : " << COMMAND_COUNT << '\n'
        << "  Process Runs      : " << BATCH_RUNS << '\n'
        << "  Sampling Clock    : steady_clock\n\n";
    if (raw_output)
    {
        std::cout << "METADATA_JSON,{\"git_commit\":\"" << MATCHING_ENGINE_GIT_COMMIT
                  << "\",\"cpu\":\"" << environment("PROCESSOR_IDENTIFIER")
                  << "\",\"logical_processors\":\"" << environment("NUMBER_OF_PROCESSORS")
                  << "\",\"os\":\"" << MATCHING_ENGINE_OS
                  << "\",\"compiler\":\"" << MATCHING_ENGINE_COMPILER
                  << "\",\"compiler_flags\":\"" << MATCHING_ENGINE_COMPILER_FLAGS
                  << "\",\"build_type\":\"" << MATCHING_ENGINE_BUILD_TYPE
                  << "\",\"events_enabled\":true"
                  << ",\"warmup_operations\":" << COMMAND_COUNT
                  << ",\"measured_operations\":" << COMMAND_COUNT
                  << ",\"process_runs\":" << BATCH_RUNS
                  << ",\"sampling_clock\":\"steady_clock\"}\n";
    }

    std::vector<Result> results;
    results.push_back(measure("core_pre_resolved_instrument", [](const std::string&) {
        return wrap(std::make_shared<CoreFixture>());
    }));
    results.push_back(measure("core_plus_routing", [](const std::string&) {
        return wrap(std::make_shared<RoutedFixture>());
    }));
    const auto engine_factory = [&](const std::string& name, bool monitor,
                                    bool logger, bool journal) {
        return wrap(std::make_shared<EngineFixture>(monitor, logger, journal, name));
    };
    results.push_back(measure("dispatcher", [&](const std::string& suffix) {
        return engine_factory("dispatcher_" + suffix, false, false, false);
    }));
    results.push_back(measure("dispatcher_monitor", [&](const std::string& suffix) {
        return engine_factory("monitor_" + suffix, true, false, false);
    }));
    results.push_back(measure("dispatcher_async_logger", [&](const std::string& suffix) {
        return engine_factory("logger_" + suffix, false, true, false);
    }));
    results.push_back(measure("dispatcher_async_journal", [&](const std::string& suffix) {
        return engine_factory("journal_" + suffix, false, false, true);
    }));
    results.push_back(measure("full_v2", [&](const std::string& suffix) {
        return engine_factory("full_" + suffix, true, true, true);
    }));


    std::cout
        << "============================================================\n"
        << " PERFORMANCE RESULTS / 性能测试结果\n"
        << "============================================================\n\n";

    for (size_t i = 0; i < results.size(); ++i)
    {
        print_human_result(results[i], i + 1, results.size());
    }

    if (raw_output)
    {
        std::cout
            << "============================================================\n"
            << " MACHINE-READABLE OUTPUT / 机器可读原始数据\n"
            << "============================================================\n";

        std::cout << "RESULT_CSV,scenario,operations,batch_avg_ns,throughput_per_sec,"
                     "sample_median_ns,sample_p99_ns,sample_p999_ns,sample_max_ns,checksum\n";
        for (const Result &result : results)
        {
            std::cout << "RESULT_CSV," << result.scenario << ',' << COMMAND_COUNT << ','
                      << std::fixed << std::setprecision(2) << result.batch_ns << ','
                      << std::setprecision(0) << result.throughput << ','
                      << result.median_ns << ',' << result.p99_ns << ','
                      << result.p999_ns << ',' << result.max_ns << ','
                      << result.checksum << '\n';
        }
    }
    return 0;
}
