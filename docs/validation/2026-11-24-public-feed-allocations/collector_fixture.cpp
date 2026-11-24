#define _CRT_SECURE_NO_WARNINGS
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

int main() {
    const char* raw = std::getenv("ALLOCATION_CAPTURE_MOCK_MODE");
    const std::string mode = raw ? raw : "valid";
    if (mode == "timeout") {
        std::cout << "partial-stdout" << std::endl;
        std::cerr << "partial-stderr" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 0;
    }
    const std::string metadata =
        "METADATA_JSON,{\"schema_version\":1,\"measurement_scope\":\"successful_cpp_new_requests\","
        "\"counter_scope\":\"successful_cpp_new_requests\",\"thread_scope\":\"calling_thread_only\","
        "\"timing_reported\":false,\"worker_threads_excluded\":true,\"total_heap_not_measured\":true,"
        "\"setup_includes_fixture_owner\":true,\"latency_samples_collected\":false,"
        "\"workload\":\"synthetic_mixed_visible_v1\",\"workload_counters\":\"full_scenario_context_repeated_per_phase\","
        "\"build_type\":\"Release\",\"cycles_per_symbol\":1,\"scenarios\":3,\"phases\":4}";
    if (mode == "bad_json") std::cout << "METADATA_JSON,{]" << '\n';
    else if (mode == "string_number") {
        auto altered = metadata;
        altered.replace(altered.find("\"schema_version\":1"), std::string("\"schema_version\":1").size(), "\"schema_version\":\"1\"");
        std::cout << altered << '\n';
    } else if (mode != "missing_json") {
        std::cout << metadata << '\n';
        if (mode == "duplicate_json") std::cout << metadata << '\n';
    }
    const std::string header = "ALLOCATION_CSV,scenario,phase,commands,public_messages,wire_bytes,allocation_calls,requested_bytes,aligned_calls,zero_size_calls,overflowed,thread_scope,counter_scope,timing_reported";
    std::cout << header << '\n';
    if (mode == "duplicate_header") std::cout << header << '\n';
    const char* scenarios[] = {"journal_public_feed_sync", "full_v2_public_feed_sync", "full_v2_public_feed_burst16"};
    const char* phases[] = {"setup", "run", "stop_and_validation", "teardown"};
    for (int scenario = 0; scenario != 3; ++scenario) {
        for (int phase = 0; phase != 4; ++phase) {
            const bool first = scenario == 0 && phase == 0;
            if (mode == "missing_row" && scenario == 2 && phase == 3) continue;
            const char* phase_name = mode == "duplicate_row" && first ? "run" : phases[phase];
            const char* commands = mode == "wrong_workload" && first ? "25" : "24";
            const char* bytes = mode == "uint64_overflow" && first ? "18446744073709551616" : "7";
            const char* aligned = mode == "invalid_ratio" && first ? "3" : "1";
            std::cout << "ALLOCATION_CSV," << scenarios[scenario] << ',' << phase_name << ',' << commands
                << ",27,1284,2," << bytes << ',' << aligned << ",0,0,calling_thread_only,successful_cpp_new_requests,false\n";
        }
    }
    std::cerr << "fixture-stderr-retained" << std::endl;
    return mode == "native_failure" ? 7 : 0;
}
