#ifndef STREAMING_GZIP_HPP
#define STREAMING_GZIP_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>

#ifndef MATCHING_ENGINE_HAS_ZLIB
#define MATCHING_ENGINE_HAS_ZLIB 0
#endif
#if MATCHING_ENGINE_HAS_ZLIB
#include <zlib.h>
#endif

namespace nasdaq_itch {

struct GzipLimits {
    std::uint64_t maximum_output_bytes = 256ULL * 1024 * 1024 * 1024;
    std::uint64_t maximum_members = 1024;
};
struct GzipMetrics {
    std::uint64_t output_bytes = 0; // Only callbacks which returned successfully.
    std::uint64_t members = 0; // Complete, CRC/ISIZE-validated and delivered members.
};
enum class GzipErrorCode {
    DISABLED, INITIALIZATION, INVALID_STREAM, TRUNCATED_STREAM, EMPTY_STREAM,
    NULL_INPUT, OUTPUT_LIMIT, MEMBER_LIMIT, COUNTER_OVERFLOW, FAILED_STATE,
    REENTRANT_USE, AFTER_FINISH, STALLED
};
class GzipError : public std::runtime_error {
public:
    GzipError(GzipErrorCode code, const char* reason)
        : std::runtime_error(reason), code_(code) {}
    GzipErrorCode code() const noexcept { return code_; }
private:
    GzipErrorCode code_;
};

// Optional gzip-only streaming decoder. Application output uses a fixed 4 KiB
// array; zlib still allocates its inflate state/window. This is neither a total
// heap budget nor zero-allocation decompression. The caller owns input storage
// until consume returns, and sink output is borrowed only during the callback.
//
// Every concatenated member is reset/validated separately. Unlike gzread, no
// trailing garbage is ignored and physical EOF without a full trailer fails.
// Output may have been delivered before a later CRC/truncation error: callers
// must invalidate downstream state and must not certify it before finish().
// Failures are sticky; sink exceptions propagate unchanged and are not retried.
// Single owner only, no copy/move, concurrency or public-operation reentry.
class StreamingGzipDecoder {
public:
    static constexpr std::size_t OUTPUT_CHUNK_SIZE = 4096;
    static constexpr bool available() noexcept { return MATCHING_ENGINE_HAS_ZLIB != 0; }

    explicit StreamingGzipDecoder(GzipLimits limits = {}) : limits_(limits) {
        if (limits.maximum_output_bytes == 0 || limits.maximum_members == 0) {
            throw std::invalid_argument("gzip limits must be positive");
        }
#if MATCHING_ENGINE_HAS_ZLIB
        const int result = inflateInit2(&stream_, 15 + 16); // gzip only, not zlib/raw auto-detect.
        if (result != Z_OK) {
            if (stream_.state != nullptr) (void)inflateEnd(&stream_);
            throw GzipError(GzipErrorCode::INITIALIZATION, "gzip inflate initialization failed");
        }
        initialized_ = true;
#else
        throw GzipError(GzipErrorCode::DISABLED, "gzip support disabled in this build");
#endif
    }
    ~StreamingGzipDecoder() noexcept {
#if MATCHING_ENGINE_HAS_ZLIB
        if (initialized_) (void)inflateEnd(&stream_);
#endif
    }
    StreamingGzipDecoder(const StreamingGzipDecoder&) = delete;
    StreamingGzipDecoder& operator=(const StreamingGzipDecoder&) = delete;
    StreamingGzipDecoder(StreamingGzipDecoder&&) = delete;
    StreamingGzipDecoder& operator=(StreamingGzipDecoder&&) = delete;

    template<class Sink>
    void consume(const std::uint8_t* data, std::size_t size, Sink&& sink) {
        static_assert(std::is_void_v<std::invoke_result_t<Sink&, const std::uint8_t*, std::size_t>>,
                      "gzip sink must return void");
        ensure_usable();
        if (finished_) fail(GzipErrorCode::AFTER_FINISH, "gzip input after successful finish");
        if (data == nullptr && size != 0) fail(GzipErrorCode::NULL_INPUT, "null gzip input with nonzero size");
        busy_ = true;
        struct Guard { bool& flag; ~Guard() { flag = false; } } guard{busy_};
        try {
#if MATCHING_ENGINE_HAS_ZLIB
            consume_impl(data, size, sink);
#else
            (void)data;
            (void)size;
            (void)sink;
            fail(GzipErrorCode::DISABLED, "gzip support disabled in this build");
#endif
        } catch (...) {
            failed_ = true;
#if MATCHING_ENGINE_HAS_ZLIB
            // Do not retain a pointer into the caller's input after failure.
            stream_.next_in = Z_NULL;
            stream_.avail_in = 0;
#endif
            throw;
        }
    }

    // Marks logical EOF. Repeat finish is harmless, but any subsequent consume
    // (including empty input) is rejected. A failed instance cannot be revived.
    void finish() {
        ensure_usable();
        if (finished_) return;
#if MATCHING_ENGINE_HAS_ZLIB
        if (metrics_.members == 0 && !saw_input_) {
            fail(GzipErrorCode::EMPTY_STREAM, "empty input is not a gzip member");
        }
        if (!at_member_end_) fail(GzipErrorCode::TRUNCATED_STREAM, "gzip EOF before complete member trailer");
        finished_ = true;
#else
        fail(GzipErrorCode::DISABLED, "gzip support disabled in this build");
#endif
    }

    const GzipMetrics& metrics() const noexcept { return metrics_; }
    bool failed() const noexcept { return failed_; }

private:
    [[noreturn]] void fail(GzipErrorCode code, const char* reason) {
        failed_ = true;
        throw GzipError(code, reason); // No payload/header text is dumped.
    }
    void ensure_usable() {
        if (failed_) fail(GzipErrorCode::FAILED_STATE, "gzip decoder is terminally failed");
        if (busy_) fail(GzipErrorCode::REENTRANT_USE, "re-entrant gzip decoder use");
    }

#if MATCHING_ENGINE_HAS_ZLIB
    void start_next_member() {
        if (metrics_.members >= limits_.maximum_members) {
            fail(GzipErrorCode::MEMBER_LIMIT, "gzip member limit exhausted");
        }
        if (inflateReset2(&stream_, 15 + 16) != Z_OK) {
            fail(GzipErrorCode::INITIALIZATION, "gzip member reset failed");
        }
        at_member_end_ = false;
    }
    template<class Sink>
    void consume_impl(const std::uint8_t* data, std::size_t size, Sink& sink) {
        std::size_t cursor = 0;
        while (cursor < size) {
            if (at_member_end_) start_next_member();
            saw_input_ = true;
            // zlib avail_in is uInt even where application size_t is 64-bit.
            const auto span = std::min(size - cursor,
                static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
            stream_.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data + cursor));
            stream_.avail_in = static_cast<uInt>(span);
            for (;;) {
                const uInt before_input = stream_.avail_in;
                stream_.next_out = reinterpret_cast<Bytef*>(output_.data());
                stream_.avail_out = static_cast<uInt>(output_.size());
                const int result = inflate(&stream_, Z_NO_FLUSH);
                const auto consumed = static_cast<std::size_t>(before_input - stream_.avail_in);
                const auto produced = output_.size() - static_cast<std::size_t>(stream_.avail_out);
                cursor += consumed;
                if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                    fail(GzipErrorCode::INVALID_STREAM, "gzip format, header, CRC or ISIZE validation failed");
                }
                if (produced > limits_.maximum_output_bytes - metrics_.output_bytes) {
                    fail(GzipErrorCode::OUTPUT_LIMIT, "gzip decoded output limit exhausted");
                }
                if (produced != 0) {
                    std::invoke(sink, output_.data(), produced);
                    if (failed_) fail(GzipErrorCode::FAILED_STATE, "gzip sink swallowed a rejected reentry");
                    metrics_.output_bytes += static_cast<std::uint64_t>(produced);
                }
                if (result == Z_STREAM_END) {
                    if (metrics_.members == std::numeric_limits<std::uint64_t>::max()) {
                        fail(GzipErrorCode::COUNTER_OVERFLOW, "gzip member counter overflow");
                    }
                    ++metrics_.members;
                    at_member_end_ = true;
                    break;
                }
                if (consumed == 0 && produced == 0) {
                    if (stream_.avail_in != 0) fail(GzipErrorCode::STALLED, "gzip decoder made no progress with input");
                    break; // Needs more input; Z_BUF_ERROR itself is not fatal.
                }
                if (stream_.avail_in == 0 && stream_.avail_out != 0) break;
                // A full output chunk must be drained even when input is now
                // empty; zlib can still have pending output/end bits buffered.
            }
        }
        stream_.next_in = Z_NULL;
        stream_.avail_in = 0;
    }
    z_stream stream_{};
    bool initialized_ = false;
    bool at_member_end_ = false;
    bool saw_input_ = false;
    std::array<std::uint8_t, OUTPUT_CHUNK_SIZE> output_{};
#endif
    const GzipLimits limits_;
    GzipMetrics metrics_;
    bool busy_ = false;
    bool failed_ = false;
    bool finished_ = false;
};

} // namespace nasdaq_itch
#endif
