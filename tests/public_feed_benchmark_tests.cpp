#include "../benchmarks/public_feed_benchmark.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using public_feed_benchmark::Fixture;
using public_feed_benchmark::Stats;

template<typename Exception, typename Action>
void expect_exception(Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const Exception&) { caught = true; }
    TEST_ASSERT(caught);
}

Stats expected_stats(size_t cycles, size_t drain_every) {
    Stats expected;
    const size_t groups = cycles * 3;
    expected.commands = static_cast<uint64_t>(groups * 8);
    expected.public_messages = static_cast<uint64_t>(groups * 9);
    expected.wire_bytes = static_cast<uint64_t>(groups * 428);
    expected.visible_commands = static_cast<uint64_t>(groups * 7);
    expected.no_visible_commands = static_cast<uint64_t>(groups);
    const std::array<size_t, 8> bytes{{49, 49, 112, 32, 49, 105, 32, 0}};
    const std::array<size_t, 8> messages{{1, 1, 2, 1, 1, 2, 1, 0}};
    size_t queued_bytes = 0;
    size_t queued_batches = 0;
    size_t queued_messages = 0;
    size_t interval = 0;
    for (size_t command = 0; command < groups * 8; ++command) {
        const size_t step = command % bytes.size();
        queued_bytes += bytes[step];
        queued_batches += bytes[step] == 0 ? size_t{0} : size_t{1};
        queued_messages += messages[step];
        expected.peak_pending_bytes = std::max(expected.peak_pending_bytes, queued_bytes);
        expected.peak_pending_batches = std::max(expected.peak_pending_batches, queued_batches);
        expected.peak_pending_messages = std::max(expected.peak_pending_messages, queued_messages);
        if (++interval == drain_every) {
            interval = 0;
            queued_bytes = 0;
            queued_batches = 0;
            queued_messages = 0;
        }
    }
    return expected;
}

void assert_stats(const Stats& actual, const Stats& expected) {
    TEST_ASSERT(actual.commands == expected.commands);
    TEST_ASSERT(actual.public_messages == expected.public_messages);
    TEST_ASSERT(actual.wire_bytes == expected.wire_bytes);
    TEST_ASSERT(actual.visible_commands == expected.visible_commands);
    TEST_ASSERT(actual.no_visible_commands == expected.no_visible_commands);
    TEST_ASSERT(actual.peak_pending_bytes == expected.peak_pending_bytes);
    TEST_ASSERT(actual.peak_pending_batches == expected.peak_pending_batches);
    TEST_ASSERT(actual.peak_pending_messages == expected.peak_pending_messages);
}

void test_matrix_and_visible_projection() {
    for (const size_t cycles : {size_t{1}, size_t{2}, size_t{3}}) {
        uint64_t expected_checksum = 0;
        bool have_checksum = false;
        // Five and seven require tail drains; 1000 exercises a wholly deferred
        // workload. Sixteen includes the no-visible IOC in its command interval.
        for (const size_t drain_every : {size_t{1}, size_t{5}, size_t{16},
                                        size_t{7}, size_t{1000}}) {
            const Stats expected = expected_stats(cycles, drain_every);
            for (const bool diagnostics : {false, true}) {
                for (const bool sample : {false, true}) {
                    // Every drain compares visible order multisets and price
                    // levels, including intermediate partial-trade states.
                    Fixture fixture(cycles, drain_every, true, diagnostics);
                    TEST_ASSERT(fixture.operation_count() == cycles * 3 * 8);
                    assert_stats(fixture.stats(), Stats{});
                    std::vector<uint64_t> latencies;
                    const uint64_t checksum = fixture.run(sample, sample ? &latencies : nullptr);
                    fixture.stop();
                    fixture.stop();
                    assert_stats(fixture.stats(), expected);
                    TEST_ASSERT(latencies.size() == (sample ? cycles * 3 * 7 : size_t{0}));
                    if (have_checksum) TEST_ASSERT(checksum == expected_checksum);
                    else { expected_checksum = checksum; have_checksum = true; }
                }
            }
        }
    }
}

void test_sampling_append_and_verification_independence() {
    Fixture sampled(2, 5, false, true);
    Fixture verified(2, 5, true, true);
    std::vector<uint64_t> samples{12345};
    const uint64_t sampled_checksum = sampled.run(true, &samples);
    const uint64_t verified_checksum = verified.run(false, nullptr);
    sampled.stop();
    verified.stop();
    TEST_ASSERT(sampled_checksum == verified_checksum);
    TEST_ASSERT(samples.size() == 43 && samples.front() == 12345);
    assert_stats(sampled.stats(), verified.stats());
    // No assertion about elapsed wall time: clocks and machines vary.
}

void test_zero_overflow_and_single_use() {
    expect_exception<std::invalid_argument>([] { Fixture invalid(0, 1, true); });
    expect_exception<std::invalid_argument>([] { Fixture invalid(1, 0, true); });
    const size_t maximum = std::numeric_limits<size_t>::max();
    expect_exception<std::overflow_error>([&] { Fixture invalid(maximum, 1, true); });
    expect_exception<std::overflow_error>([&] { Fixture invalid(maximum / 24, 1, true); });
    // This passes the command-count check but overflows the byte budget.
    expect_exception<std::overflow_error>([&] { Fixture invalid(maximum / 1284 + 1, 1, true); });
    {
        Fixture fixture(1, maximum, true);
        std::vector<uint64_t> samples;
        fixture.run(true, &samples);
        TEST_ASSERT(samples.size() == 21);
        fixture.stop();
        expect_exception<std::logic_error>([&] { fixture.run(false, nullptr); });
    }
    {
        Fixture fixture(1, 1, true);
        fixture.run(false, nullptr);
        expect_exception<std::logic_error>([&] { fixture.run(false, nullptr); });
        fixture.stop();
    }
    {
        Fixture unused(1, 1, true, true);
        unused.stop();
        unused.stop();
        expect_exception<std::logic_error>([&] { unused.run(false, nullptr); });
    }
    {
        Fixture failed(1, 1, true, true);
        expect_exception<std::invalid_argument>([&] { failed.run(true, nullptr); });
        expect_exception<std::logic_error>([&] { failed.run(false, nullptr); });
        expect_exception<std::runtime_error>([&] { failed.stop(); });
    }
    // Destruction without stop must quiesce both owned worker threads.
    { Fixture abandoned(1, 1, true, true); }
}
} // namespace

int main() {
    try {
        test_matrix_and_visible_projection();
        test_sampling_append_and_verification_independence();
        test_zero_overflow_and_single_use();
        std::cout << "Public feed benchmark fixture tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Public feed benchmark fixture tests failed: " << error.what() << '\n';
        return 1;
    }
}
