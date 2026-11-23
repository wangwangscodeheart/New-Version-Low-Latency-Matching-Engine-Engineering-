#include "itch_verified_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;

// Entirely invented S records. Literal SHA independently obtained with the
// Windows .NET SHA256 implementation over these 86 bytes, not this project's
// Sha256 or a downloaded Nasdaq file. The complete data has zero stocks/trades.
constexpr const char* RAW_CONTROL_SHA =
    "3afc0c0eb85db019a30ff6231ea2442471c9d48ee0a518230cb207e504961787";

Bytes system_record(char phase, std::uint8_t tick) {
    return Bytes{0,12,'S',0,0,0,0,0,0,0,0,0,tick,static_cast<std::uint8_t>(phase)};
}
void append(Bytes& destination, const Bytes& source) {
    destination.insert(destination.end(), source.begin(), source.end());
}
Bytes control_file() {
    Bytes bytes;
    std::uint8_t tick = 0;
    for (const char phase : std::string_view("OSQMEC")) append(bytes, system_record(phase, tick++));
    bytes.insert(bytes.end(), {0,0});
    TEST_ASSERT(bytes.size() == 86);
    return bytes;
}
std::string as_string(const Bytes& bytes) { return {bytes.begin(), bytes.end()}; }
std::string digest_of(const Bytes& bytes) {
    Sha256 sha;
    sha.update(bytes.data(), bytes.size());
    return sha.hex_digest();
}
std::string uppercase_digest(std::string value) {
    for (char& byte : value) if (byte >= 'a' && byte <= 'f') byte = static_cast<char>(byte - 'a' + 'A');
    return value;
}
constexpr std::array<std::ios::iostate, 8> ALL_IO_MASKS{{
    std::ios::goodbit,
    std::ios::eofbit,
    std::ios::failbit,
    std::ios::badbit,
    std::ios::eofbit | std::ios::failbit,
    std::ios::eofbit | std::ios::badbit,
    std::ios::failbit | std::ios::badbit,
    std::ios::eofbit | std::ios::failbit | std::ios::badbit}};
RebuildValidation all_checks() {
    RebuildValidation validation;
    validation.validate_intraday_timestamps = true;
    validation.require_all_system_events = true;
    validation.correlate_trade_reports = true;
    return validation;
}
void assert_complete(const ItchBookRebuilder& session, std::uint64_t records = 6) {
    TEST_ASSERT(session.state() == RebuildState::COMPLETE);
    TEST_ASSERT(session.metrics().processed_records == records && session.metrics().errors == 0);
    TEST_ASSERT(session.instrument_count() == 0 && session.active_order_count() == 0);
    TEST_ASSERT(session.check_invariants());
    if (session.temporal_metrics()) TEST_ASSERT(session.temporal_metrics()->observations == records);
    if (session.trade_ledger()) TEST_ASSERT(session.trade_ledger()->size() == 0);
}
template<class Action>
void expect_stale(ItchBookRebuilder& session, Action&& action) {
    bool caught = false;
    try { action(); } catch (const std::exception&) { caught = true; }
    TEST_ASSERT(caught && session.state() == RebuildState::STALE);
    TEST_ASSERT(session.metrics().errors == 1);
    TEST_ASSERT(!session.book(1) && !session.trade_ledger() && !session.temporal_metrics());
    TEST_ASSERT(!session.metadata(1) && !session.market_wide_metadata());
    bool blocked = false;
    try { session.finish(); } catch (const std::exception&) { blocked = true; }
    TEST_ASSERT(blocked && session.metrics().errors == 1);
}
VerifiedInputResult read_bytes(const Bytes& bytes, ItchBookRebuilder& session,
                              const VerifiedInputOptions& options = {}) {
    std::istringstream input(as_string(bytes), std::ios::in | std::ios::binary);
    return VerifiedInputReader::read(input, session, options);
}

void test_raw_source_digest_and_successful_finalization() {
    const auto bytes = control_file();
    TEST_ASSERT(digest_of(bytes) == RAW_CONTROL_SHA);
    for (const auto compression : {InputCompression::AUTO, InputCompression::NONE}) {
        for (const auto mask : ALL_IO_MASKS) {
            ItchBookRebuilder session(RebuildLimits{}, all_checks());
            std::istringstream input(as_string(bytes), std::ios::in | std::ios::binary);
            input.exceptions(mask);
            VerifiedInputOptions options;
            options.compression = compression;
            const auto result = VerifiedInputReader::read(input, session, options);
            assert_complete(session);
            TEST_ASSERT(result.compression == InputCompression::NONE);
            TEST_ASSERT(result.input_bytes == bytes.size() && result.decoded_bytes == bytes.size());
            TEST_ASSERT(result.gzip_members == 0 && result.source_sha256 == RAW_CONTROL_SHA);
            TEST_ASSERT(!result.expected_sha256_verified);
        }
    }
    for (const auto& expected : {std::string(RAW_CONTROL_SHA), uppercase_digest(RAW_CONTROL_SHA)}) {
        ItchBookRebuilder session(RebuildLimits{}, all_checks());
        VerifiedInputOptions options;
        options.expected_sha256 = expected;
        options.maximum_input_bytes = bytes.size();
        options.maximum_output_bytes = bytes.size();
        const auto result = read_bytes(bytes, session, options);
        assert_complete(session);
        TEST_ASSERT(result.expected_sha256_verified && result.source_sha256 == RAW_CONTROL_SHA);
    }
}

void test_expected_digest_syntax_mismatch_and_fresh_session() {
    TEST_ASSERT(VerifiedInputReader::normalize_expected_sha256(uppercase_digest(RAW_CONTROL_SHA)) == RAW_CONTROL_SHA);
    const std::vector<std::string> invalid{
        "", "abc", std::string(63, 'a'), std::string(65, 'a'),
        std::string(64, 'g'), std::string(64, ' '), std::string(63, 'a') + "\n",
        std::string(63, 'a') + std::string(1, '\0')};
    for (const auto& value : invalid) {
        bool caught = false;
        try { (void)VerifiedInputReader::normalize_expected_sha256(value); }
        catch (const std::exception&) { caught = true; }
        TEST_ASSERT(caught);
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.expected_sha256 = value;
        expect_stale(session, [&] { (void)read_bytes(control_file(), session, options); });
        TEST_ASSERT(session.metrics().processed_records == 0);
    }
    {
        ItchBookRebuilder session(RebuildLimits{}, all_checks());
        VerifiedInputOptions options;
        options.expected_sha256 = std::string(64, '0');
        expect_stale(session, [&] { (void)read_bytes(control_file(), session, options); });
        // All payloads were delivered, but source hash comparison may not
        // certify a session just because BinaryFILE's marker was observed.
        TEST_ASSERT(session.metrics().processed_records == 6);
    }
    {
        ItchBookRebuilder session;
        const auto prefix = system_record('O', 0);
        session.consume(prefix.data(), prefix.size());
        expect_stale(session, [&] { (void)read_bytes(control_file(), session); });
    }
    for (const std::size_t prefix_size : {std::size_t{1}, std::size_t{2}, std::size_t{5}}) {
        ItchBookRebuilder session;
        const auto bytes = control_file();
        session.consume(bytes.data(), prefix_size);
        TEST_ASSERT(session.state() == RebuildState::AWAITING_START);
        const Bytes suffix(bytes.begin() + prefix_size, bytes.end());
        std::istringstream input(as_string(suffix), std::ios::in | std::ios::binary);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session); });
        // A buffered length/payload fragment is not fresh: otherwise the
        // verified source hash could exclude bytes already used by the book.
        TEST_ASSERT(input.tellg() == std::streampos{0});
    }
    {
        ItchBookRebuilder session;
        const Bytes marker{0,0};
        session.consume(marker.data(), marker.size());
        TEST_ASSERT(session.state() == RebuildState::AWAITING_START);
        std::istringstream input(as_string(control_file()), std::ios::in | std::ios::binary);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session); });
        TEST_ASSERT(input.tellg() == std::streampos{0});
    }
    {
        ItchBookRebuilder session;
        const auto bytes = control_file();
        session.consume(bytes.data(), bytes.size());
        session.finish();
        TEST_ASSERT(session.state() == RebuildState::COMPLETE);
        expect_stale(session, [&] { (void)read_bytes(bytes, session); });
    }
}

class ReadCounter : public std::streambuf {
public:
    unsigned reads = 0;
private:
    std::streamsize xsgetn(char*, std::streamsize) override {
        ++reads;
        throw std::runtime_error("synthetic input must not be read");
    }
};
void test_limits_and_malformed_raw_inputs() {
    for (unsigned choice = 0; choice < 3; ++choice) {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        if (choice == 0) options.maximum_input_bytes = 0;
        if (choice == 1) options.maximum_output_bytes = 0;
        if (choice == 2) options.maximum_gzip_members = 0;
        ReadCounter buffer;
        std::istream input(&buffer);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session, options); });
        TEST_ASSERT(buffer.reads == 0);
    }
    {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.compression = static_cast<InputCompression>(255);
        ReadCounter buffer;
        std::istream input(&buffer);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session, options); });
        TEST_ASSERT(buffer.reads == 0);
    }
    for (const bool source_limit : {true, false}) {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        if (source_limit) options.maximum_input_bytes = 85;
        else options.maximum_output_bytes = 85;
        expect_stale(session, [&] { (void)read_bytes(control_file(), session, options); });
        TEST_ASSERT(session.metrics().processed_records == 0);
    }
    for (const auto& malformed : {Bytes{}, Bytes{0}, Bytes{0,0}, Bytes{0x1f}}) {
        ItchBookRebuilder session;
        expect_stale(session, [&] { (void)read_bytes(malformed, session); });
    }
    {
        auto trailing = control_file();
        trailing.push_back(0);
        ItchBookRebuilder session;
        expect_stale(session, [&] { (void)read_bytes(trailing, session); });
    }
    {
        auto missing_marker = control_file();
        missing_marker.resize(missing_marker.size() - 2);
        ItchBookRebuilder session;
        expect_stale(session, [&] { (void)read_bytes(missing_marker, session); });
    }
}

// S controls plus10 V and260 W messages end exactly on a 4096-byte read
// boundary. That forces another physical read after valid framing/S/C so a
// late device error or swallowed reentry cannot hide behind a short EOF read.
Bytes boundary_file() {
    Bytes bytes;
    std::uint8_t tick = 0;
    for (const char phase : std::string_view("OSQME")) append(bytes, system_record(phase, tick++));
    for (unsigned i = 0; i < 10; ++i) {
        Bytes value{0,35,'V',0,0,0,0,0,0,0,0,0,0};
        value.insert(value.end(), 24, 0);
        append(bytes, value);
    }
    for (unsigned i = 0; i < 260; ++i) {
        append(bytes, Bytes{0,12,'W',0,0,0,0,0,0,0,0,0,0,'1'});
    }
    append(bytes, system_record('C', tick));
    bytes.insert(bytes.end(), {0,0});
    TEST_ASSERT(bytes.size() == 4096);
    return bytes;
}
class LateFailure : public std::streambuf {
    Bytes bytes_;
public:
    unsigned reads = 0;
    bool delivered = false;
    explicit LateFailure(Bytes bytes) : bytes_(std::move(bytes)) { TEST_ASSERT(bytes_.size() == 4096); }
private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        ++reads;
        if (delivered) throw std::runtime_error("synthetic late IO failure");
        TEST_ASSERT(requested == 4096);
        std::memcpy(destination, bytes_.data(), bytes_.size());
        delivered = true;
        return static_cast<std::streamsize>(bytes_.size());
    }
};
class SwallowedReentry : public std::streambuf {
    ItchBookRebuilder& session_;
    Bytes bytes_;
public:
    unsigned reads = 0;
    bool caught = false;
    explicit SwallowedReentry(ItchBookRebuilder& session) : session_(session), bytes_(boundary_file()) {}
private:
    std::streamsize xsgetn(char* destination, std::streamsize requested) override {
        ++reads;
        if (reads == 1) {
            TEST_ASSERT(requested == 4096);
            std::memcpy(destination, bytes_.data(), bytes_.size());
            return static_cast<std::streamsize>(bytes_.size());
        }
        try { session_.finish(); }
        catch (const std::exception&) { caught = true; }
        return 0;
    }
};
void test_late_io_and_swallowed_reentry_cannot_complete() {
    {
        ItchBookRebuilder session;
        const auto result = read_bytes(boundary_file(), session);
        assert_complete(session, 276);
        TEST_ASSERT(result.input_bytes == 4096 && result.decoded_bytes == 4096);
    }
    for (const auto mask : ALL_IO_MASKS) {
        ItchBookRebuilder session;
        LateFailure buffer(boundary_file());
        std::istream input(&buffer);
        input.exceptions(mask);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session); });
        TEST_ASSERT(buffer.delivered && buffer.reads == 2);
        TEST_ASSERT(session.metrics().processed_records == 276);
    }
    for (const auto mask : ALL_IO_MASKS) {
        ItchBookRebuilder session;
        SwallowedReentry buffer(session);
        std::istream input(&buffer);
        input.exceptions(mask);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session); });
        TEST_ASSERT(buffer.caught && buffer.reads == 2);
        TEST_ASSERT(session.metrics().processed_records == 276);
    }
}

#if MATCHING_ENGINE_HAS_ZLIB
Bytes gzip(const Bytes& raw) {
    TEST_ASSERT(raw.size() <= std::numeric_limits<uInt>::max());
    z_stream stream{};
    TEST_ASSERT(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    struct End { z_stream& stream; ~End() { (void)deflateEnd(&stream); } } end{stream};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(raw.data()));
    stream.avail_in = static_cast<uInt>(raw.size());
    Bytes result;
    std::array<std::uint8_t, 4096> output{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        status = deflate(&stream, Z_FINISH);
        TEST_ASSERT(status == Z_OK || status == Z_STREAM_END);
        result.insert(result.end(), output.begin(), output.begin() + (output.size() - stream.avail_out));
    }
    return result;
}
Bytes delayed_gzip_trailer(const Bytes& raw) {
    const auto original = gzip(raw);
    TEST_ASSERT(original.size() >= 18 && original.size() < 4102);
    const auto extra = 4102 - original.size();
    Bytes padded(original.begin(), original.begin() + 10);
    padded[3] = static_cast<std::uint8_t>(padded[3] | 4U); // FEXTRA, no filename/path.
    padded.push_back(static_cast<std::uint8_t>(extra));
    padded.push_back(static_cast<std::uint8_t>(extra >> 8U));
    padded.insert(padded.end(), extra, 0);
    padded.insert(padded.end(), original.begin() + 10, original.end());
    TEST_ASSERT(padded.size() == 4104); // First read has all data, not the8-byte trailer.
    return padded;
}
void test_gzip_source_hash_concatenation_and_budgets() {
    const auto raw = control_file();
    const auto compressed = gzip(raw);
    TEST_ASSERT(digest_of(compressed) != RAW_CONTROL_SHA);
    for (const auto format : {InputCompression::AUTO, InputCompression::GZIP}) {
        for (const auto mask : ALL_IO_MASKS) {
            ItchBookRebuilder session(RebuildLimits{}, all_checks());
            VerifiedInputOptions options;
            options.compression = format;
            options.expected_sha256 = uppercase_digest(digest_of(compressed));
            std::istringstream input(as_string(compressed), std::ios::in | std::ios::binary);
            input.exceptions(mask);
            const auto result = VerifiedInputReader::read(input, session, options);
            assert_complete(session);
            TEST_ASSERT(result.compression == InputCompression::GZIP && result.gzip_members == 1);
            TEST_ASSERT(result.input_bytes == compressed.size() && result.decoded_bytes == raw.size());
            TEST_ASSERT(result.source_sha256 == digest_of(compressed) && result.expected_sha256_verified);
        }
    }
    {
        const auto left = gzip(Bytes(raw.begin(), raw.begin() + 17));
        const auto right = gzip(Bytes(raw.begin() + 17, raw.end()));
        Bytes concatenated = left;
        append(concatenated, right);
        ItchBookRebuilder session;
        const auto result = read_bytes(concatenated, session);
        assert_complete(session);
        TEST_ASSERT(result.gzip_members == 2 && result.decoded_bytes == raw.size());
        TEST_ASSERT(result.input_bytes == concatenated.size() && result.source_sha256 == digest_of(concatenated));
        ItchBookRebuilder too_many;
        VerifiedInputOptions options;
        options.maximum_gzip_members = 1;
        expect_stale(too_many, [&] { (void)read_bytes(concatenated, too_many, options); });
        TEST_ASSERT(too_many.metrics().processed_records == 1);
    }
    {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.expected_sha256 = RAW_CONTROL_SHA; // Correct decoded hash, WRONG source hash.
        expect_stale(session, [&] { (void)read_bytes(compressed, session, options); });
        TEST_ASSERT(session.metrics().processed_records == 6);
    }
    for (const bool source_limit : {true, false}) {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        if (source_limit) options.maximum_input_bytes = compressed.size() - 1;
        else options.maximum_output_bytes = raw.size() - 1;
        expect_stale(session, [&] { (void)read_bytes(compressed, session, options); });
    }
    {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.compression = InputCompression::NONE;
        expect_stale(session, [&] { (void)read_bytes(compressed, session, options); });
    }
}
void test_gzip_errors_after_native_end_marker_still_invalidate() {
    const auto raw = control_file();
    const auto compressed = gzip(raw);
    for (std::size_t size = 0; size < compressed.size(); ++size) {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.compression = InputCompression::GZIP;
        expect_stale(session, [&] { (void)read_bytes(Bytes(compressed.begin(), compressed.begin() + size), session, options); });
    }
    {
        auto trailing = compressed;
        trailing.push_back(0x7f);
        ItchBookRebuilder session;
        expect_stale(session, [&] { (void)read_bytes(trailing, session); });
    }
    const auto delayed = delayed_gzip_trailer(raw);
    {
        ItchBookRebuilder session;
        const auto result = read_bytes(delayed, session);
        assert_complete(session);
        TEST_ASSERT(result.input_bytes == 4104 && result.decoded_bytes == 86 && result.gzip_members == 1);
    }
    for (const std::size_t trailer_field : {std::size_t{8}, std::size_t{4}}) {
        auto corrupt = delayed;
        corrupt[corrupt.size() - trailer_field] ^= 1;
        ItchBookRebuilder session;
        expect_stale(session, [&] { (void)read_bytes(corrupt, session); });
        TEST_ASSERT(session.metrics().processed_records == 6); // Prefix was delivered, still no COMPLETE.
    }
    {
        ItchBookRebuilder session;
        const Bytes no_trailer(delayed.begin(), delayed.begin() + 4096);
        expect_stale(session, [&] { (void)read_bytes(no_trailer, session); });
        TEST_ASSERT(session.metrics().processed_records == 6);
    }
    {
        ItchBookRebuilder session;
        LateFailure buffer(Bytes(delayed.begin(), delayed.begin() + 4096));
        std::istream input(&buffer);
        expect_stale(session, [&] { (void)VerifiedInputReader::read(input, session); });
        TEST_ASSERT(buffer.reads == 2 && session.metrics().processed_records == 6);
    }
}
#else
void test_disabled_gzip_is_explicitly_rejected() {
    TEST_ASSERT(!StreamingGzipDecoder::available());
    for (const auto format : {InputCompression::AUTO, InputCompression::GZIP}) {
        ItchBookRebuilder session;
        VerifiedInputOptions options;
        options.compression = format;
        expect_stale(session, [&] { (void)read_bytes(Bytes{0x1f,0x8b,8,0}, session, options); });
    }
}
#endif
} // namespace

int main() {
    try {
        test_raw_source_digest_and_successful_finalization();
        test_expected_digest_syntax_mismatch_and_fresh_session();
        test_limits_and_malformed_raw_inputs();
        test_late_io_and_swallowed_reentry_cannot_complete();
#if MATCHING_ENGINE_HAS_ZLIB
        test_gzip_source_hash_concatenation_and_budgets();
        test_gzip_errors_after_native_end_marker_still_invalidate();
        std::cout << "Verified ITCH source input tests passed (synthetic only; raw SHA and gzip enabled).\n";
#else
        test_disabled_gzip_is_explicitly_rejected();
        std::cout << "Verified ITCH source input tests passed (synthetic only; raw SHA, gzip disabled/rejection only).\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Verified ITCH input test failed: " << error.what() << '\n';
        return 1;
    }
}
