#include "allocation_meter.hpp"
#include "public_feed_benchmark.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

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
#ifndef MATCHING_ENGINE_ALLOCATION_CYCLES_PER_SYMBOL
#define MATCHING_ENGINE_ALLOCATION_CYCLES_PER_SYMBOL 2500
#endif

namespace {

constexpr std::size_t CYCLES = MATCHING_ENGINE_ALLOCATION_CYCLES_PER_SYMBOL;
static_assert(CYCLES > 0, "Allocation workload must be positive");
static_assert(CYCLES <= (std::numeric_limits<std::size_t>::max)() / 1284,
              "Allocation workload byte count exceeds size_t");
static_assert(CYCLES <= (std::numeric_limits<std::uint64_t>::max)() / 1284,
              "Allocation workload byte count exceeds uint64_t");

struct Scenario {
    const char* name;
    std::size_t drain_every;
    bool diagnostics;
};

constexpr std::array<Scenario, 3> SCENARIOS{{
    {"journal_public_feed_sync", 1, false},
    {"full_v2_public_feed_sync", 1, true},
    {"full_v2_public_feed_burst16", 16, true}
}};
constexpr std::array<const char*, 4> PHASES{{
    "setup", "run", "stop_and_validation", "teardown"
}};

struct Result {
    std::array<allocation_meter::Counts, PHASES.size()> counts{};
    public_feed_benchmark::Stats stats{};
    std::uint64_t checksum = 0;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Result measure(const Scenario& scenario) {
    Result result;
    std::unique_ptr<public_feed_benchmark::Fixture> fixture;
    {
        allocation_meter::Scope scope;
        // The fixture owner's allocation belongs to setup, not the run phase.
        fixture = std::make_unique<public_feed_benchmark::Fixture>(
            CYCLES, scenario.drain_every, false, scenario.diagnostics);
        result.counts[0] = scope.finish();
    }
    {
        allocation_meter::Scope scope;
        // Identical existing mixed workload, without sampled timestamps or a
        // latency vector. Existing diagnostics retain their normal behavior.
        result.checksum = fixture->run(false, nullptr);
        result.counts[1] = scope.finish();
    }
    {
        allocation_meter::Scope scope;
        // Join workers and retain the fixture's existing full health, sequence,
        // journal/logger write-count and final empty-book assertions.
        fixture->stop();
        result.counts[2] = scope.finish();
    }
    result.stats = fixture->stats();
    {
        allocation_meter::Scope scope;
        fixture.reset();
        result.counts[3] = scope.finish();
    }
    for (const auto& counts : result.counts) {
        require(!counts.overflowed, "Allocation counter overflow; result not accepted");
    }
    const auto cycles = static_cast<std::uint64_t>(CYCLES);
    require(result.stats.commands == cycles * 24 &&
            result.stats.public_messages == cycles * 27 &&
            result.stats.wire_bytes == cycles * 1284 &&
            result.stats.visible_commands == cycles * 21 &&
            result.stats.no_visible_commands == cycles * 3,
            "Allocation probe workload counters differ; result not accepted");
    return result;
}

// Formatting happens only after every counting Scope has finished. Escape
// compiler flags and other build strings instead of assuming JSON-safe bytes.
void json_string(std::ostream& output, std::string_view value) {
    constexpr char HEX[] = "0123456789abcdef";
    output.put('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            output.put('\\');
            output.put(character);
        } else if (byte < 0x20) {
            output << "\\u00";
            output.put(HEX[byte >> 4]);
            output.put(HEX[byte & 0x0f]);
        } else {
            output.put(character);
        }
    }
    output.put('"');
}

void print_metadata() {
    std::cout << "METADATA_JSON,{\"schema_version\":1,\"git_commit\":";
    json_string(std::cout, MATCHING_ENGINE_GIT_COMMIT);
    std::cout << ",\"compiler\":";
    json_string(std::cout, MATCHING_ENGINE_COMPILER);
    std::cout << ",\"compiler_flags\":";
    json_string(std::cout, MATCHING_ENGINE_COMPILER_FLAGS);
    std::cout << ",\"build_type\":";
    json_string(std::cout, MATCHING_ENGINE_BUILD_TYPE);
    std::cout << ",\"os\":";
    json_string(std::cout, MATCHING_ENGINE_OS);
    std::cout << ",\"measurement_scope\":\"successful_cpp_new_requests\""
              << ",\"compiler_flags_scope\":\"cmake_global_release_and_directory_options_excludes_definitions_and_includes\""
              << ",\"cxx_standard\":17"
              << ",\"counter_scope\":\"successful_cpp_new_requests\""
              << ",\"thread_scope\":\"calling_thread_only\""
              << ",\"timing_reported\":false"
              << ",\"workload\":\"synthetic_mixed_visible_v1\""
              << ",\"cycles_per_symbol\":" << CYCLES
              << ",\"scenarios\":3,\"phases\":4"
              << ",\"setup_includes_fixture_owner\":true"
              << ",\"worker_threads_excluded\":true"
              << ",\"total_heap_not_measured\":true"
              << ",\"latency_samples_collected\":false"
              << ",\"workload_counters\":\"full_scenario_context_repeated_per_phase\"}\n";
}

void print_rows(const std::array<Result, SCENARIOS.size()>& results) {
    std::cout << "ALLOCATION_CSV,scenario,phase,commands,public_messages,wire_bytes,"
                 "allocation_calls,requested_bytes,aligned_calls,zero_size_calls,"
                 "overflowed,thread_scope,counter_scope,timing_reported\n";
    for (std::size_t scenario = 0; scenario < SCENARIOS.size(); ++scenario) {
        const auto& result = results[scenario];
        for (std::size_t phase = 0; phase < PHASES.size(); ++phase) {
            const auto& counts = result.counts[phase];
            // Workload counters describe the full validated scenario in each
            // row; they are not phase-local command increments to be summed.
            std::cout << "ALLOCATION_CSV," << SCENARIOS[scenario].name << ','
                      << PHASES[phase] << ',' << result.stats.commands << ','
                      << result.stats.public_messages << ',' << result.stats.wire_bytes << ','
                      << counts.allocation_calls << ',' << counts.requested_bytes << ','
                      << counts.aligned_calls << ',' << counts.zero_size_calls << ','
                      << (counts.overflowed ? 1 : 0)
                      << ",calling_thread_only,successful_cpp_new_requests,false\n";
        }
    }
}

} // namespace

int main() {
    try {
        // Stack storage and deferred output exclude formatting/collector
        // allocations. No successful rows escape a failed later scenario.
        std::array<Result, SCENARIOS.size()> results{};
        for (std::size_t index = 0; index < SCENARIOS.size(); ++index) {
            results[index] = measure(SCENARIOS[index]);
            require(index == 0 || results[index].checksum == results[0].checksum,
                    "Allocation probe scenario checksums differ; result not accepted");
        }
        print_metadata();
        print_rows(results);
        std::cout.flush();
        require(static_cast<bool>(std::cout), "Allocation probe output write/flush failed; result not accepted");
        return 0;
    } catch (const std::exception& error) {
        try { std::cerr << "Allocation probe failed (result not accepted): " << error.what() << '\n'; }
        catch (...) {}
        return 1;
    } catch (...) {
        try { std::cerr << "Allocation probe failed (result not accepted): unknown exception\n"; }
        catch (...) {}
        return 1;
    }
}
