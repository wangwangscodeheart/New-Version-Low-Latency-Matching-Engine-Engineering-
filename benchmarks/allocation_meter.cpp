#include "allocation_meter.hpp"

#include <cstdlib>
#include <limits>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_meter {
namespace detail {

// Constant-initialized TLS: entering the hook never creates a counter buffer.
thread_local Scope* active_scope = nullptr;

#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
thread_local std::size_t test_failures_remaining = 0;
thread_local std::size_t test_failures_consumed = 0;

void set_test_failure_budget(std::size_t failures) noexcept {
    test_failures_remaining = failures;
    test_failures_consumed = 0;
}

std::size_t test_injected_failures() noexcept {
    return test_failures_consumed;
}

bool consume_test_failure() noexcept {
    if (test_failures_remaining == 0) return false;
    --test_failures_remaining;
    ++test_failures_consumed; // Cannot exceed the initially supplied size_t budget.
    return true;
}
#endif

void saturating_add(std::uint64_t& total, std::uint64_t increment,
                    bool& overflowed) noexcept {
    const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
    if (increment > maximum - total) {
        total = maximum;
        overflowed = true;
    } else {
        total += increment;
    }
}

void record_allocation(std::size_t requested_size, bool aligned) noexcept {
    if (active_scope == nullptr) return;
    auto& counts = active_scope->counts_;
    saturating_add(counts.allocation_calls, 1, counts.overflowed);
    if constexpr (std::numeric_limits<std::size_t>::digits >
                  std::numeric_limits<std::uint64_t>::digits) {
        if (requested_size > (std::numeric_limits<std::uint64_t>::max)()) {
            counts.requested_bytes = (std::numeric_limits<std::uint64_t>::max)();
            counts.overflowed = true;
        } else {
            saturating_add(counts.requested_bytes,
                           static_cast<std::uint64_t>(requested_size), counts.overflowed);
        }
    } else {
        saturating_add(counts.requested_bytes,
                       static_cast<std::uint64_t>(requested_size), counts.overflowed);
    }
    if (aligned) saturating_add(counts.aligned_calls, 1, counts.overflowed);
    if (requested_size == 0) saturating_add(counts.zero_size_calls, 1, counts.overflowed);
}

} // namespace detail

Scope::Scope() noexcept : previous_(detail::active_scope) {
    detail::active_scope = this;
}

Scope::~Scope() noexcept {
    (void)finish();
}

Counts Scope::finish() noexcept {
    if (!finished_) {
        if (detail::active_scope == this) {
            detail::active_scope = previous_;
        } else {
            // Explicit finish need not follow nesting order. Remove this Scope
            // from the unfinished chain so no later finish can reactivate it.
            Scope* inner = detail::active_scope;
            while (inner != nullptr && inner->previous_ != this) {
                inner = inner->previous_;
            }
            if (inner != nullptr) inner->previous_ = previous_;
        }
        previous_ = nullptr;
        finished_ = true;
    }
    return counts_;
}

} // namespace allocation_meter

namespace {

void allocation_failed() {
    const std::new_handler handler = std::get_new_handler();
    if (handler == nullptr) throw std::bad_alloc();
    // Retry if the handler returns; propagate its exception for throwing new.
    handler();
}

void* allocate(std::size_t requested_size) {
    const std::size_t physical_size = requested_size == 0 ? 1 : requested_size;
    for (;;) {
#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
        if (allocation_meter::detail::consume_test_failure()) {
            allocation_failed();
            continue;
        }
#endif
        if (void* result = std::malloc(physical_size)) {
            allocation_meter::detail::record_allocation(requested_size, false);
            return result;
        }
        allocation_failed();
    }
}

void* allocate_aligned(std::size_t requested_size, std::size_t alignment) {
    // align_val_t represents a valid power-of-two alignment. Reject malformed
    // explicit calls instead of passing them to a CRT invalid-parameter handler.
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::bad_alloc();
    const std::size_t physical_size = requested_size == 0 ? 1 : requested_size;
    const std::size_t physical_alignment =
        alignment < sizeof(void*) ? sizeof(void*) : alignment;
    for (;;) {
#if defined(ALLOCATION_METER_TEST_FAILURE_INJECTION) && ALLOCATION_METER_TEST_FAILURE_INJECTION
        if (allocation_meter::detail::consume_test_failure()) {
            allocation_failed();
            continue;
        }
#endif
        void* result = nullptr;
#ifdef _WIN32
        result = _aligned_malloc(physical_size, physical_alignment);
#else
        if (posix_memalign(&result, physical_alignment, physical_size) != 0) result = nullptr;
#endif
        if (result != nullptr) {
            allocation_meter::detail::record_allocation(requested_size, true);
            return result;
        }
        allocation_failed();
    }
}

void free_aligned(void* value) noexcept {
#ifdef _WIN32
    _aligned_free(value);
#else
    std::free(value);
#endif
}

} // namespace

// Each allocation overload reaches exactly one recording point. Forwarding via
// another public replacement would risk counting a single call twice.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return allocate(size); } catch (...) { return nullptr; }
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try { return allocate_aligned(size, static_cast<std::size_t>(alignment)); }
    catch (...) { return nullptr; }
}

void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete[](void* value, const std::nothrow_t&) noexcept { std::free(value); }
void operator delete(void* value, std::align_val_t) noexcept { free_aligned(value); }
void operator delete[](void* value, std::align_val_t) noexcept { free_aligned(value); }
void operator delete(void* value, std::size_t, std::align_val_t) noexcept { free_aligned(value); }
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept { free_aligned(value); }
void operator delete(void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(value);
}
void operator delete[](void* value, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(value);
}
