#include "allocation_meter.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

namespace {

using allocation_meter::Counts;
using allocation_meter::Scope;

// Explicit function calls through volatile pointers keep the exact hook
// contract tests independent of new-expression allocation elision or GCC's
// unused-allocation DCE. The actual benchmark remains fully optimized.
void* (* volatile scalar_new)(std::size_t) = &::operator new;
void* (* volatile array_new)(std::size_t) = &::operator new[];
void* (* volatile scalar_nothrow_new)(std::size_t, const std::nothrow_t&) noexcept = &::operator new;
void* (* volatile array_nothrow_new)(std::size_t, const std::nothrow_t&) noexcept = &::operator new[];
void* (* volatile scalar_aligned_new)(std::size_t, std::align_val_t) = &::operator new;
void* (* volatile array_aligned_new)(std::size_t, std::align_val_t) = &::operator new[];
void* (* volatile scalar_aligned_nothrow_new)(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept = &::operator new;
void* (* volatile array_aligned_nothrow_new)(std::size_t, std::align_val_t, const std::nothrow_t&) noexcept = &::operator new[];

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool equal(const Counts& first, const Counts& second) noexcept {
    return first.allocation_calls == second.allocation_calls &&
           first.requested_bytes == second.requested_bytes &&
           first.aligned_calls == second.aligned_calls &&
           first.zero_size_calls == second.zero_size_calls &&
           first.overflowed == second.overflowed;
}

void expect(const Counts& counts, std::uint64_t calls, std::uint64_t bytes,
            std::uint64_t aligned = 0, std::uint64_t zero = 0) {
    require(equal(counts, Counts{calls, bytes, aligned, zero, false}),
            "allocation counters differ from exact expected totals");
}

void allocate_and_free(std::size_t bytes) {
    void* value = scalar_new(bytes);
    require(value != nullptr, "throwing allocation returned null");
    ::operator delete(value);
}

void test_plain_and_nothrow_forms() {
    Scope scope;
    void* scalar = scalar_new(11);
    ::operator delete(scalar);
    void* array = array_new(17);
    ::operator delete[](array);
    void* zero_scalar = scalar_new(0);
    require(zero_scalar != nullptr, "zero-size scalar allocation returned null");
    ::operator delete(zero_scalar);
    void* zero_array = array_new(0);
    require(zero_array != nullptr, "zero-size array allocation returned null");
    ::operator delete[](zero_array);
    void* sized_scalar = scalar_new(31);
    ::operator delete(sized_scalar, std::size_t{31});
    void* sized_array = array_new(37);
    ::operator delete[](sized_array, std::size_t{37});
    void* nothrow_scalar = scalar_nothrow_new(43, std::nothrow);
    require(nothrow_scalar != nullptr, "nothrow scalar allocation failed");
    ::operator delete(nothrow_scalar, std::nothrow);
    void* nothrow_array = array_nothrow_new(47, std::nothrow);
    require(nothrow_array != nullptr, "nothrow array allocation failed");
    ::operator delete[](nothrow_array, std::nothrow);
    void* zero_nothrow_scalar = scalar_nothrow_new(0, std::nothrow);
    require(zero_nothrow_scalar != nullptr, "zero-size nothrow scalar allocation failed");
    ::operator delete(zero_nothrow_scalar, std::nothrow);
    void* zero_nothrow_array = array_nothrow_new(0, std::nothrow);
    require(zero_nothrow_array != nullptr, "zero-size nothrow array allocation failed");
    ::operator delete[](zero_nothrow_array, std::nothrow);
    expect(scope.finish(), 10, 186, 0, 4);
}

void check_alignment(void* value) {
    require(value != nullptr, "aligned allocation returned null");
    require(reinterpret_cast<std::uintptr_t>(value) % 64 == 0,
            "over-aligned allocation is incorrectly aligned");
}

void test_aligned_forms() {
    constexpr auto alignment = std::align_val_t{64};
    Scope scope;
    void* scalar = scalar_aligned_new(61, alignment);
    check_alignment(scalar);
    ::operator delete(scalar, alignment);
    void* array = array_aligned_new(65, alignment);
    check_alignment(array);
    ::operator delete[](array, alignment);
    void* sized_scalar = scalar_aligned_new(67, alignment);
    check_alignment(sized_scalar);
    ::operator delete(sized_scalar, std::size_t{67}, alignment);
    void* sized_array = array_aligned_new(71, alignment);
    check_alignment(sized_array);
    ::operator delete[](sized_array, std::size_t{71}, alignment);
    void* nothrow_scalar = scalar_aligned_nothrow_new(73, alignment, std::nothrow);
    check_alignment(nothrow_scalar);
    ::operator delete(nothrow_scalar, alignment, std::nothrow);
    void* nothrow_array = array_aligned_nothrow_new(79, alignment, std::nothrow);
    check_alignment(nothrow_array);
    ::operator delete[](nothrow_array, alignment, std::nothrow);
    void* zero_scalar = scalar_aligned_new(0, alignment);
    check_alignment(zero_scalar);
    ::operator delete(zero_scalar, alignment);
    void* zero_array = array_aligned_new(0, alignment);
    check_alignment(zero_array);
    ::operator delete[](zero_array, alignment);
    void* zero_nothrow_scalar = scalar_aligned_nothrow_new(0, alignment, std::nothrow);
    check_alignment(zero_nothrow_scalar);
    ::operator delete(zero_nothrow_scalar, alignment, std::nothrow);
    void* zero_nothrow_array = array_aligned_nothrow_new(0, alignment, std::nothrow);
    check_alignment(zero_nothrow_array);
    ::operator delete[](zero_nothrow_array, alignment, std::nothrow);
    expect(scope.finish(), 10, 416, 10, 4);
}

void test_empty_scope_and_idempotence() {
    Scope empty;
    expect(empty.finish(), 0, 0);
    allocate_and_free(5);
    expect(empty.finish(), 0, 0);

    Scope scope;
    allocate_and_free(13);
    const Counts first = scope.finish();
    allocate_and_free(23);
    require(equal(first, scope.finish()), "finish is not idempotent");
    expect(first, 1, 13);
}

void test_nested_scopes_and_out_of_order_finish() {
    Scope outer;
    allocate_and_free(3);
    {
        Scope inner;
        allocate_and_free(5);
        expect(inner.finish(), 1, 5);
        allocate_and_free(7);
        expect(inner.finish(), 1, 5);
    }
    expect(outer.finish(), 2, 10);

    Scope first;
    allocate_and_free(11);
    Scope second;
    allocate_and_free(13);
    expect(first.finish(), 1, 11);
    allocate_and_free(17);
    expect(second.finish(), 2, 30);
    allocate_and_free(19);
    expect(first.finish(), 1, 11);
    expect(second.finish(), 2, 30);
}

void test_exception_raii_cleanup() {
    struct Marker {};
    Scope outer;
    try {
        Scope inner;
        allocate_and_free(101);
        throw Marker{};
    } catch (const Marker&) {
        allocate_and_free(7);
    }
    expect(outer.finish(), 1, 7);
    // A standalone exceptional exit must also leave the thread inactive.
    try {
        Scope abandoned;
        allocate_and_free(103);
        throw Marker{};
    } catch (const Marker&) {
    }
    allocate_and_free(107);
    Scope next;
    allocate_and_free(11);
    expect(next.finish(), 1, 11);
}

void test_worker_is_excluded() {
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    // Thread construction (and its potential allocations) precedes the Scope.
    std::thread worker([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned int index = 0; index != 1000; ++index) allocate_and_free(29);
        done.store(true, std::memory_order_release);
    });
    Scope scope;
    start.store(true, std::memory_order_release);
    allocate_and_free(31);
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    const Counts counts = scope.finish();
    worker.join();
    expect(counts, 1, 31);
}

void test_worker_can_count_its_own_interval() {
    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    Counts worker_counts;
    std::thread worker([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        Scope worker_scope;
        allocate_and_free(41);
        worker_counts = worker_scope.finish();
        done.store(true, std::memory_order_release);
    });
    Scope main_scope;
    start.store(true, std::memory_order_release);
    allocate_and_free(43);
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    const Counts main_counts = main_scope.finish();
    worker.join();
    expect(main_counts, 1, 43);
    expect(worker_counts, 1, 41);
}

void test_saturating_add() {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t total = maximum - 2;
    bool overflowed = false;
    allocation_meter::detail::saturating_add(total, 2, overflowed);
    require(total == maximum && !overflowed, "exact maximum incorrectly overflows");
    allocation_meter::detail::saturating_add(total, 0, overflowed);
    require(total == maximum && !overflowed, "adding zero incorrectly overflows");
    allocation_meter::detail::saturating_add(total, 1, overflowed);
    require(total == maximum && overflowed, "overflow did not saturate");
    allocation_meter::detail::saturating_add(total, maximum, overflowed);
    require(total == maximum && overflowed, "saturation/overflow flag is not sticky");
    total = 0;
    allocation_meter::detail::saturating_add(total, 3, overflowed);
    require(total == 3 && overflowed, "a later addition cleared the overflow flag");
}

#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
enum class AllocationKind { SCALAR, ARRAY, ALIGNED_SCALAR, ALIGNED_ARRAY };
constexpr std::array<AllocationKind, 4> ALLOCATION_KINDS{{
    AllocationKind::SCALAR, AllocationKind::ARRAY,
    AllocationKind::ALIGNED_SCALAR, AllocationKind::ALIGNED_ARRAY}};
constexpr std::size_t FAILURE_TEST_BYTES = 83;
constexpr auto FAILURE_TEST_ALIGNMENT = std::align_val_t{64};

bool is_aligned(AllocationKind kind) noexcept {
    return kind == AllocationKind::ALIGNED_SCALAR || kind == AllocationKind::ALIGNED_ARRAY;
}

void* allocate_for_failure_test(AllocationKind kind, bool nothrow) {
    switch (kind) {
    case AllocationKind::SCALAR:
        return nothrow ? scalar_nothrow_new(FAILURE_TEST_BYTES, std::nothrow) :
                         scalar_new(FAILURE_TEST_BYTES);
    case AllocationKind::ARRAY:
        return nothrow ? array_nothrow_new(FAILURE_TEST_BYTES, std::nothrow) :
                         array_new(FAILURE_TEST_BYTES);
    case AllocationKind::ALIGNED_SCALAR:
        return nothrow ? scalar_aligned_nothrow_new(FAILURE_TEST_BYTES, FAILURE_TEST_ALIGNMENT, std::nothrow) :
                         scalar_aligned_new(FAILURE_TEST_BYTES, FAILURE_TEST_ALIGNMENT);
    case AllocationKind::ALIGNED_ARRAY:
        return nothrow ? array_aligned_nothrow_new(FAILURE_TEST_BYTES, FAILURE_TEST_ALIGNMENT, std::nothrow) :
                         array_aligned_new(FAILURE_TEST_BYTES, FAILURE_TEST_ALIGNMENT);
    }
    throw std::bad_alloc();
}

void release_for_failure_test(AllocationKind kind, void* value) noexcept {
    switch (kind) {
    case AllocationKind::SCALAR: ::operator delete(value); break;
    case AllocationKind::ARRAY: ::operator delete[](value); break;
    case AllocationKind::ALIGNED_SCALAR: ::operator delete(value, FAILURE_TEST_ALIGNMENT); break;
    case AllocationKind::ALIGNED_ARRAY: ::operator delete[](value, FAILURE_TEST_ALIGNMENT); break;
    }
}

class NewHandlerGuard final {
public:
    explicit NewHandlerGuard(std::new_handler handler) noexcept
        : previous_(std::set_new_handler(handler)) {}
    ~NewHandlerGuard() noexcept { (void)std::set_new_handler(previous_); }
    NewHandlerGuard(const NewHandlerGuard&) = delete;
    NewHandlerGuard& operator=(const NewHandlerGuard&) = delete;
private:
    std::new_handler previous_;
};

class FailureBudgetGuard final {
public:
    explicit FailureBudgetGuard(std::size_t failures) noexcept {
        allocation_meter::detail::set_test_failure_budget(failures);
    }
    ~FailureBudgetGuard() noexcept { allocation_meter::detail::set_test_failure_budget(0); }
    FailureBudgetGuard(const FailureBudgetGuard&) = delete;
    FailureBudgetGuard& operator=(const FailureBudgetGuard&) = delete;
};

std::size_t handler_calls = 0;
void returning_handler() { ++handler_calls; }
class HandlerBadAlloc final : public std::bad_alloc {};
void throwing_handler() {
    ++handler_calls;
    throw HandlerBadAlloc{};
}

void test_deterministic_failures_and_new_handlers() {
    const auto original_handler = std::get_new_handler();
    for (const auto kind : ALLOCATION_KINDS) {
        for (const bool nothrow : {false, true}) {
            {
                NewHandlerGuard handler(nullptr);
                FailureBudgetGuard failure(1);
                Scope scope;
                bool threw_bad_alloc = false;
                void* value = nullptr;
                try { value = allocate_for_failure_test(kind, nothrow); }
                catch (const std::bad_alloc&) { threw_bad_alloc = true; }
                const Counts counts = scope.finish();
                if (value != nullptr) release_for_failure_test(kind, value);
                require(nothrow ? (!threw_bad_alloc && value == nullptr) : threw_bad_alloc,
                        "null-handler failure did not follow throwing/nothrow contract");
                require(allocation_meter::detail::test_injected_failures() == 1,
                        "null-handler failure did not consume exactly one injected attempt");
                expect(counts, 0, 0);
            }
            {
                handler_calls = 0;
                NewHandlerGuard handler(returning_handler);
                FailureBudgetGuard failure(1);
                Scope scope;
                void* value = allocate_for_failure_test(kind, nothrow);
                const Counts counts = scope.finish();
                if (value != nullptr) release_for_failure_test(kind, value);
                require(value != nullptr && handler_calls == 1,
                        "returning new_handler must retry exactly once and succeed");
                require(allocation_meter::detail::test_injected_failures() == 1,
                        "retry failure budget did not consume exactly one attempt");
                expect(counts, 1, FAILURE_TEST_BYTES, is_aligned(kind) ? 1 : 0);
            }
            {
                handler_calls = 0;
                NewHandlerGuard handler(throwing_handler);
                FailureBudgetGuard failure(1);
                Scope scope;
                bool derived_exception_preserved = false;
                void* value = nullptr;
                try { value = allocate_for_failure_test(kind, nothrow); }
                catch (const HandlerBadAlloc&) { derived_exception_preserved = true; }
                const Counts counts = scope.finish();
                if (value != nullptr) release_for_failure_test(kind, value);
                require(handler_calls == 1 &&
                        (nothrow ? (!derived_exception_preserved && value == nullptr) : derived_exception_preserved),
                        "throwing new_handler exception or nothrow translation differs");
                require(allocation_meter::detail::test_injected_failures() == 1,
                        "throwing-handler budget did not consume exactly one attempt");
                expect(counts, 0, 0);
            }
        }
    }
    require(std::get_new_handler() == original_handler, "RAII did not restore the original new_handler");
    require(allocation_meter::detail::test_injected_failures() == 0,
            "failure-budget guard did not reset the test seam");
}
#endif

} // namespace

int main() {
    static_assert(!std::is_copy_constructible<Scope>::value, "Scope must not copy");
    static_assert(!std::is_move_constructible<Scope>::value, "Scope must not move");
    static_assert(noexcept(Scope{}), "Scope construction must not throw");
    test_plain_and_nothrow_forms();
    test_aligned_forms();
    test_empty_scope_and_idempotence();
    test_nested_scopes_and_out_of_order_finish();
    test_exception_raii_cleanup();
    test_worker_is_excluded();
    test_worker_can_count_its_own_interval();
    test_saturating_add();
#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
    test_deterministic_failures_and_new_handlers();
    std::cout << "PASS allocation_meter_tests (8 original groups; 24 deterministic failure/handler cases)\n";
#else
    std::cout << "PASS allocation_meter_tests (8 original groups; deterministic failure/handler cases NOT EXECUTED)\n";
#endif
    return EXIT_SUCCESS;
}
