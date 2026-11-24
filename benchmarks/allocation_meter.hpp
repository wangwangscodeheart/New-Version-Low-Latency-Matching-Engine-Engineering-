#pragma once

#include <cstddef>
#include <cstdint>

// Executable-local C++ allocation instrumentation. Link allocation_meter.cpp
// only into the allocation-count companion, never the latency benchmark.
namespace allocation_meter {

struct Counts {
    std::uint64_t allocation_calls = 0;
    std::uint64_t requested_bytes = 0;
    std::uint64_t aligned_calls = 0;
    std::uint64_t zero_size_calls = 0;
    bool overflowed = false;
};

namespace detail {
// Implementation detail exposed for a deterministic overflow unit test.
void saturating_add(std::uint64_t& total, std::uint64_t increment,
                    bool& overflowed) noexcept;
void record_allocation(std::size_t requested_size, bool aligned) noexcept;

#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
// Test executable only: fail the next N underlying allocation attempts on this
// thread without requesting oversized blocks or provoking actual system OOM.
// Production probe builds must not define this macro.
void set_test_failure_budget(std::size_t failures) noexcept;
std::size_t test_injected_failures() noexcept;
#endif
} // namespace detail

// Counts successful replaceable C++ new/new[] calls on the creating thread.
// requested_bytes is the sum of the size arguments, not physical heap usage;
// malloc/realloc, CRT/OS-internal allocations, frees and peaks are not measured.
// A zero-size request is counted as zero bytes, even though storage is obtained.
//
// Construction starts a fresh interval without allocating. Nested intervals are
// exclusive: the innermost unfinished Scope counts, and its outer Scope resumes
// when it finishes. Finishing an outer Scope early also safely unlinks it.
// Construct, finish and destroy a Scope on the same thread. A worker thread is
// inactive unless it constructs its own Scope. Scope objects cannot be moved.
class Scope final {
public:
    Scope() noexcept;
    ~Scope() noexcept;

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    // Stops this interval; subsequent calls return the same frozen counters.
    Counts finish() noexcept;

private:
    friend void detail::record_allocation(std::size_t, bool) noexcept;

    Counts counts_{};
    Scope* previous_ = nullptr;
    bool finished_ = false;
};

} // namespace allocation_meter
