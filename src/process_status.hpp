#ifndef PROCESS_STATUS_HPP
#define PROCESS_STATUS_HPP

#include <cstddef>
#include <cstdint>

enum class ProcessStatus : uint8_t {
    APPLIED = 0,
    REJECTED = 1,
    SEQUENCE_REJECTED = 2,
    OUTPUT_BLOCKED = 3
};

// Output acceptance is separate from application of a command to the book.
// PENDING can resume at the first unaccepted event; FAILED cannot be retried
// automatically because an exception may have made acceptance ambiguous.
enum class EventOutputState : uint8_t { READY = 0, PENDING = 1, FAILED = 2 };

struct ProcessResult {
    ProcessStatus status;
    size_t event_begin;
    size_t event_count;
    bool event_output_complete = true;

    bool applied() const noexcept { return status == ProcessStatus::APPLIED; }
};

#endif
