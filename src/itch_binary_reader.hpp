#ifndef ITCH_BINARY_READER_HPP
#define ITCH_BINARY_READER_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace nasdaq_itch {

// Nasdaq BinaryFILE framing only: a two-byte big-endian payload length,
// followed by that many bytes (including the ITCH message-type byte).
// A zero length is the end-of-session marker, not a payload record.
// This reader neither decodes ITCH nor assigns exchange sequence numbers.
struct BinaryRecordView {
    const std::uint8_t* data;
    std::size_t size;
    std::uint64_t ordinal; // One-based file-local payload ordinal, NOT a sequence.
    std::uint64_t offset;  // Zero-based byte offset of the length prefix.
};

enum class EndPolicy {
    REQUIRE_TERMINATOR,
    ALLOW_BOUNDARY_EOF_ONLY
};

enum class CompletionStatus {
    COMPLETE_WITH_TERMINATOR,
    BOUNDARY_EOF_WITHOUT_TERMINATOR
};

enum class BinaryFileErrorCode {
    INVALID_CONFIGURATION,
    NULL_INPUT,
    LENGTH_EXCEEDS_LIMIT,
    TRUNCATED_PREFIX,
    TRUNCATED_PAYLOAD,
    MISSING_TERMINATOR,
    TRAILING_DATA,
    IO_ERROR,
    FAILED_STATE,
    REENTRANT_USE,
    COUNTER_OVERFLOW
};

class BinaryFileError : public std::runtime_error {
public:
    BinaryFileError(BinaryFileErrorCode code, const char* reason,
                    std::uint64_t ordinal, std::uint64_t offset)
        : std::runtime_error(std::string("Nasdaq BinaryFILE: ") + reason +
                             " (record " + std::to_string(ordinal) +
                             ", byte offset " + std::to_string(offset) + ")"),
          code_(code), ordinal_(ordinal), offset_(offset) {}

    BinaryFileErrorCode code() const noexcept { return code_; }
    std::uint64_t ordinal() const noexcept { return ordinal_; }
    std::uint64_t offset() const noexcept { return offset_; }

private:
    BinaryFileErrorCode code_;
    std::uint64_t ordinal_;
    std::uint64_t offset_;
};

// Successful framing uses fixed storage: 64 KiB payload + two prefix bytes.
// read() adds one 4 KiB stack chunk; neither API buffers the entire input.
// Error construction and the caller's stream/sink may allocate.
// A sink view is borrowed ONLY until that callback returns. Copy bytes inside
// the callback if they must survive it. A throwing sink makes the reader
// terminally failed; its exception is propagated unchanged and never retried.
// Reader identity is stable: copying/moving and public-operation re-entry
// (including from a custom stream buffer) are not supported.
class BinaryFileReader {
public:
    static constexpr std::size_t MAXIMUM_PAYLOAD_SIZE = 65535;
    static constexpr std::size_t PAYLOAD_STORAGE_SIZE = 65536;
    static constexpr std::size_t STREAM_CHUNK_SIZE = 4096;

    explicit BinaryFileReader(std::size_t maximum_payload = MAXIMUM_PAYLOAD_SIZE)
        : maximum_payload_(maximum_payload) {
        if (maximum_payload == 0 || maximum_payload > MAXIMUM_PAYLOAD_SIZE) {
            throw BinaryFileError(BinaryFileErrorCode::INVALID_CONFIGURATION,
                                  "maximum payload must be in [1, 65535]", 1, 0);
        }
    }

    BinaryFileReader(const BinaryFileReader&) = delete;
    BinaryFileReader& operator=(const BinaryFileReader&) = delete;
    BinaryFileReader(BinaryFileReader&&) = delete;
    BinaryFileReader& operator=(BinaryFileReader&&) = delete;

    template <typename Sink>
    void consume(const std::uint8_t* data, std::size_t size, Sink&& sink) {
        ensure_usable();
        consume_impl(data, size, sink);
    }

private:
    template <typename Sink>
    void consume_impl(const std::uint8_t* data, std::size_t size, Sink& sink) {
        static_assert(std::is_void_v<std::invoke_result_t<Sink&, const BinaryRecordView&>>,
                      "BinaryFILE sink must return void");
        ensure_not_failed();
        if (data == nullptr && size != 0) {
            fail(BinaryFileErrorCode::NULL_INPUT, "null input with nonzero size",
                 bytes_consumed_);
        }

        std::size_t cursor = 0;
        while (cursor < size) {
            if (terminated_) {
                fail(BinaryFileErrorCode::TRAILING_DATA, "data after end-of-session marker",
                     bytes_consumed_);
            }
            if (payload_length_ == 0) {
                if (prefix_used_ == 0) prefix_offset_ = bytes_consumed_;
                const std::size_t amount = std::min(prefix_.size() - prefix_used_, size - cursor);
                advance_bytes(amount);
                std::memcpy(prefix_.data() + prefix_used_, data + cursor, amount);
                prefix_used_ += amount;
                cursor += amount;
                if (prefix_used_ != prefix_.size()) continue;

                payload_length_ = (static_cast<std::size_t>(prefix_[0]) << 8U) |
                                  static_cast<std::size_t>(prefix_[1]);
                if (payload_length_ == 0) {
                    terminated_ = true;
                    prefix_used_ = 0;
                    continue;
                }
                if (payload_length_ > maximum_payload_) {
                    fail(BinaryFileErrorCode::LENGTH_EXCEEDS_LIMIT,
                         "payload length exceeds configured maximum", prefix_offset_);
                }
            }

            const std::size_t amount = std::min(payload_length_ - payload_used_, size - cursor);
            advance_bytes(amount);
            std::memcpy(payload_.data() + payload_used_, data + cursor, amount);
            payload_used_ += amount;
            cursor += amount;
            if (payload_used_ != payload_length_) continue;

            if (records_ == std::numeric_limits<std::uint64_t>::max()) {
                fail(BinaryFileErrorCode::COUNTER_OVERFLOW, "record counter overflow", prefix_offset_);
            }
            const BinaryRecordView view{payload_.data(), payload_length_, records_ + 1, prefix_offset_};
            in_callback_ = true;
            try {
                std::invoke(sink, view);
            } catch (...) {
                in_callback_ = false;
                failed_ = true;
                throw;
            }
            in_callback_ = false;
            // A sink cannot swallow a re-entrant-use failure and revive us.
            ensure_not_failed();
            ++records_;
            prefix_used_ = 0;
            payload_length_ = 0;
            payload_used_ = 0;
        }
    }

public:
    // Reads through physical EOF even after the marker, so later trailing bytes
    // are rejected. Ordinary EOF is accepted even if the stream exception mask
    // includes eofbit/failbit; badbit or a non-EOF read failure is never accepted.
    template <typename Sink>
    CompletionStatus read(std::istream& input, Sink&& sink,
                          EndPolicy policy = EndPolicy::REQUIRE_TERMINATOR) {
        ensure_usable();
        read_active_ = true;
        struct ReadOperationGuard {
            bool& flag;
            ~ReadOperationGuard() { flag = false; }
        } operation_guard{read_active_};
        std::array<char, STREAM_CHUNK_SIZE> chunk{};
        for (;;) {
            std::exception_ptr read_exception;
            try {
                input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            } catch (...) {
                read_exception = std::current_exception();
            }
            // A streambuf can catch and swallow its own rejected re-entry.
            // Never process its bytes or report completion after that failure.
            ensure_not_failed();
            const std::streamsize count = input.gcount();
            if (count < 0 || count > static_cast<std::streamsize>(chunk.size()) ||
                input.bad() || (!input.eof() && (input.fail() || read_exception))) {
                fail(BinaryFileErrorCode::IO_ERROR, "underlying stream read failed", bytes_consumed_);
            }
            if (count != 0) {
                consume_impl(reinterpret_cast<const std::uint8_t*>(chunk.data()),
                             static_cast<std::size_t>(count), sink);
            }
            if (input.eof()) return finish_impl(policy);
            if (count != static_cast<std::streamsize>(chunk.size())) {
                fail(BinaryFileErrorCode::IO_ERROR, "short stream read without EOF", bytes_consumed_);
            }
        }
    }

    // A lenient boundary result explicitly means the session is incomplete.
    // finish() is a validation check, not a reset: repeating it is harmless,
    // and a boundary-only check does not manufacture an end marker.
    CompletionStatus finish(EndPolicy policy = EndPolicy::REQUIRE_TERMINATOR) {
        ensure_usable();
        return finish_impl(policy);
    }

    // Only callbacks which returned successfully are counted. bytes_consumed()
    // includes prefix/payload bytes accepted before an error, not trailing data.
    std::uint64_t records() const noexcept { return records_; }
    std::uint64_t bytes_consumed() const noexcept { return bytes_consumed_; }
    bool terminated() const noexcept { return terminated_; }
    bool failed() const noexcept { return failed_; }

private:
    CompletionStatus finish_impl(EndPolicy policy) {
        ensure_not_failed();
        if (payload_length_ != 0) {
            fail(BinaryFileErrorCode::TRUNCATED_PAYLOAD, "EOF inside payload", prefix_offset_);
        }
        if (prefix_used_ != 0) {
            fail(BinaryFileErrorCode::TRUNCATED_PREFIX, "EOF inside length prefix", prefix_offset_);
        }
        if (terminated_) return CompletionStatus::COMPLETE_WITH_TERMINATOR;
        if (policy == EndPolicy::ALLOW_BOUNDARY_EOF_ONLY) {
            return CompletionStatus::BOUNDARY_EOF_WITHOUT_TERMINATOR;
        }
        fail(BinaryFileErrorCode::MISSING_TERMINATOR, "EOF without end-of-session marker", bytes_consumed_);
    }

    [[noreturn]] void fail(BinaryFileErrorCode code, const char* reason, std::uint64_t offset) {
        failed_ = true;
        const auto ordinal = records_ == std::numeric_limits<std::uint64_t>::max()
                                 ? records_ : records_ + 1;
        throw BinaryFileError(code, reason, ordinal, offset);
    }

    void ensure_usable() {
        ensure_not_failed();
        if (in_callback_ || read_active_) {
            fail(BinaryFileErrorCode::REENTRANT_USE, "re-entrant reader use", bytes_consumed_);
        }
    }

    void ensure_not_failed() {
        if (failed_) fail(BinaryFileErrorCode::FAILED_STATE, "reader is terminally failed", bytes_consumed_);
    }

    void advance_bytes(std::size_t amount) {
        if (amount > std::numeric_limits<std::uint64_t>::max() - bytes_consumed_) {
            fail(BinaryFileErrorCode::COUNTER_OVERFLOW, "byte counter overflow", bytes_consumed_);
        }
        bytes_consumed_ += static_cast<std::uint64_t>(amount);
    }

    std::array<std::uint8_t, PAYLOAD_STORAGE_SIZE> payload_{};
    std::array<std::uint8_t, 2> prefix_{};
    const std::size_t maximum_payload_;
    std::size_t prefix_used_ = 0;
    std::size_t payload_length_ = 0;
    std::size_t payload_used_ = 0;
    std::uint64_t prefix_offset_ = 0;
    std::uint64_t records_ = 0;
    std::uint64_t bytes_consumed_ = 0;
    bool terminated_ = false;
    bool failed_ = false;
    bool in_callback_ = false;
    bool read_active_ = false;
};

} // namespace nasdaq_itch

#endif
