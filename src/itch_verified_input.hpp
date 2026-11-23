#ifndef ITCH_VERIFIED_INPUT_HPP
#define ITCH_VERIFIED_INPUT_HPP

#include "itch_book_rebuilder.hpp"
#include "sha256.hpp"
#include "streaming_gzip.hpp"
#include <array>
#include <exception>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nasdaq_itch {

enum class InputCompression { AUTO, NONE, GZIP };
struct VerifiedInputOptions {
    InputCompression compression = InputCompression::AUTO;
    std::uint64_t maximum_input_bytes = 64ULL * 1024 * 1024 * 1024;
    std::uint64_t maximum_output_bytes = 256ULL * 1024 * 1024 * 1024;
    std::uint64_t maximum_gzip_members = 1024;
    std::optional<std::string> expected_sha256;
};
struct VerifiedInputResult {
    InputCompression compression = InputCompression::NONE;
    std::uint64_t input_bytes = 0;
    std::uint64_t decoded_bytes = 0;
    std::uint64_t gzip_members = 0;
    std::string source_sha256;
    bool expected_sha256_verified = false;
};

// Single-pass source hashing and optional gzip decoding. The digest covers
// exactly the RAW source bytes (gzip headers/trailers included), not a second
// open/read or the decompressed representation. A matching user-supplied hash
// is not authentication or a data license. No decoded files are created.
class VerifiedInputReader {
public:
    static constexpr std::size_t INPUT_CHUNK_SIZE = 4096;

    static std::string normalize_expected_sha256(std::string_view text) {
        if (text.size() != 64) throw std::invalid_argument("expected SHA-256 must contain exactly 64 hex digits");
        std::string normalized;
        normalized.reserve(64);
        for (char character : text) {
            if (character >= 'A' && character <= 'F') character = static_cast<char>(character - 'A' + 'a');
            if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
                throw std::invalid_argument("expected SHA-256 contains a non-hex character");
            }
            normalized.push_back(character);
        }
        return normalized;
    }

    static VerifiedInputResult read(std::istream& input, ItchBookRebuilder& session,
                                     const VerifiedInputOptions& options = {}) {
        VerifiedInputResult result;
        // Keep all external stream callbacks and every integrity check under
        // the existing guard. Even swallowed finish()/read() reentry cannot
        // expose COMPLETE before CRC, hash and physical-EOF checks succeed.
        session.operation([&] {
            // A partial length prefix still leaves AWAITING_START. Reject even
            // one previously consumed byte so the digest covers ALL bytes used
            // to reconstruct this session, never just a suffix of its input.
            if (session.state_ != RebuildState::AWAITING_START || session.reader_.bytes_consumed() != 0) {
                throw std::invalid_argument("verified input requires a fresh reconstruction session");
            }
            if (options.maximum_input_bytes == 0 || options.maximum_output_bytes == 0 ||
                options.maximum_gzip_members == 0 ||
                (options.compression != InputCompression::AUTO && options.compression != InputCompression::NONE &&
                 options.compression != InputCompression::GZIP)) {
                throw std::invalid_argument("invalid verified-input limits or compression mode");
            }
            std::optional<std::string> expected;
            if (options.expected_sha256) expected = normalize_expected_sha256(*options.expected_sha256);
            Sha256 digest;
            std::optional<StreamingGzipDecoder> gzip;
            bool selected = options.compression != InputCompression::AUTO;
            result.compression = options.compression == InputCompression::GZIP ? InputCompression::GZIP : InputCompression::NONE;
            const auto start_gzip = [&] {
                gzip.emplace(GzipLimits{options.maximum_output_bytes, options.maximum_gzip_members});
            };
            if (result.compression == InputCompression::GZIP) start_gzip();
            const auto emit = [&](const std::uint8_t* bytes, std::size_t count) {
                if (count > options.maximum_output_bytes - result.decoded_bytes) {
                    throw std::length_error("verified-input decoded byte limit exhausted");
                }
                session.reader_.consume(bytes, count, [&](const BinaryRecordView& record) { session.on_record(record); });
                result.decoded_bytes += static_cast<std::uint64_t>(count);
            };
            std::array<char, INPUT_CHUNK_SIZE> buffer{};
            for (;;) {
                std::exception_ptr read_exception;
                try { input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())); }
                catch (...) { read_exception = std::current_exception(); }
                if (session.state_ == RebuildState::STALE) {
                    throw std::runtime_error("reconstruction failed during input callback");
                }
                const auto count = input.gcount();
                if (count < 0 || count > static_cast<std::streamsize>(buffer.size()) || input.bad() ||
                    (!input.eof() && (input.fail() || read_exception))) {
                    throw std::runtime_error("verified-input source read failed");
                }
                if (count != 0) {
                    const auto amount = static_cast<std::size_t>(count);
                    if (amount > options.maximum_input_bytes - result.input_bytes) {
                        throw std::length_error("verified-input source byte limit exhausted");
                    }
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buffer.data());
                    digest.update(bytes, amount);
                    result.input_bytes += static_cast<std::uint64_t>(amount);
                    if (!selected) {
                        result.compression = amount >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b ?
                            InputCompression::GZIP : InputCompression::NONE;
                        selected = true;
                        if (result.compression == InputCompression::GZIP) start_gzip();
                    }
                    if (gzip) gzip->consume(bytes, amount, emit);
                    else emit(bytes, amount);
                }
                if (input.eof()) break;
                if (count != static_cast<std::streamsize>(buffer.size())) {
                    throw std::runtime_error("verified-input short read without EOF");
                }
            }
            if (gzip) {
                gzip->finish(); // Every member trailer, not just the first.
                result.gzip_members = gzip->metrics().members;
            }
            result.source_sha256 = digest.hex_digest();
            if (expected) {
                if (result.source_sha256 != *expected) throw std::runtime_error("source SHA-256 mismatch");
                result.expected_sha256_verified = true;
            }
            (void)session.reader_.finish(); // Only after input/gzip/hash success.
            session.finalize();
        });
        return result;
    }
};

} // namespace nasdaq_itch
#endif
