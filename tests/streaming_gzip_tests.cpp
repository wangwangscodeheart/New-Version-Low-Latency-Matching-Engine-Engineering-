#include "streaming_gzip.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using namespace nasdaq_itch;
using Bytes = std::vector<std::uint8_t>;

template<class Action>
void expect_error(GzipErrorCode code, Action&& action) {
    bool caught = false;
    try { action(); }
    catch (const GzipError& error) { TEST_ASSERT(error.code() == code); caught = true; }
    TEST_ASSERT(caught);
}

void test_configuration() {
    static_assert(!std::is_copy_constructible_v<StreamingGzipDecoder>);
    static_assert(!std::is_move_constructible_v<StreamingGzipDecoder>);
    static_assert(StreamingGzipDecoder::OUTPUT_CHUNK_SIZE == 4096);
    for (const auto limits : {GzipLimits{0, 1}, GzipLimits{1, 0}}) {
        bool invalid = false;
        try { StreamingGzipDecoder decoder(limits); }
        catch (const std::invalid_argument&) { invalid = true; }
        TEST_ASSERT(invalid);
    }
}

#if MATCHING_ENGINE_HAS_ZLIB
// Literal, invented RFC1952 envelopes. These are NOT market-data records.
const Bytes EMPTY_GZIP{0x1f,0x8b,0x08,0x00,0,0,0,0,0,3, 0x03,0x00, 0,0,0,0, 0,0,0,0};
const Bytes ABC_GZIP{0x1f,0x8b,0x08,0x00,0,0,0,0,0,3,
    0x4b,0x4c,0x4a,0x06,0x00, 0xc2,0x41,0x24,0x35, 3,0,0,0};
const Bytes ABC{'a','b','c'};

auto collect(Bytes& result) {
    return [&](const std::uint8_t* data, std::size_t size) {
        TEST_ASSERT(data != nullptr && size > 0 && size <= StreamingGzipDecoder::OUTPUT_CHUNK_SIZE);
        result.insert(result.end(), data, data + size); // Copy during borrowed lifetime.
    };
}
const auto discard = [](const std::uint8_t*, std::size_t) {};

Bytes pattern(std::size_t count) {
    Bytes result(count);
    for (std::size_t i = 0; i < count; ++i) result[i] = static_cast<std::uint8_t>(i * 31U);
    return result;
}

// This compressor is only a fixture helper. Independent literal empty/abc
// envelopes above remain separate from the zlib-generated test envelopes.
Bytes compress_member(const Bytes& payload, bool optional_header = false) {
    TEST_ASSERT(payload.size() <= std::numeric_limits<uInt>::max());
    z_stream stream{};
    TEST_ASSERT(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    struct End { z_stream& stream; ~End() { (void)deflateEnd(&stream); } } guard{stream};
    std::array<Bytef, 3> extra{{1,2,3}};
    std::array<Bytef, 6> name{{'s','y','n','t','h',0}};
    std::array<Bytef, 8> comment{{'i','n','v','e','n','t','e',0}};
    gz_header header{};
    if (optional_header) {
        header.extra = extra.data();
        header.extra_len = static_cast<uInt>(extra.size());
        header.name = name.data();
        header.comment = comment.data();
        header.hcrc = 1;
        header.os = 255;
        TEST_ASSERT(deflateSetHeader(&stream, &header) == Z_OK);
    }
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(payload.data()));
    stream.avail_in = static_cast<uInt>(payload.size());
    Bytes result;
    std::array<std::uint8_t, 257> block{};
    for (;;) {
        stream.next_out = reinterpret_cast<Bytef*>(block.data());
        stream.avail_out = static_cast<uInt>(block.size());
        const int status = deflate(&stream, Z_FINISH);
        TEST_ASSERT(status == Z_OK || status == Z_STREAM_END);
        const auto count = block.size() - static_cast<std::size_t>(stream.avail_out);
        result.insert(result.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(count));
        if (status == Z_STREAM_END) break;
    }
    return result;
}

void require_terminal(StreamingGzipDecoder& decoder) {
    TEST_ASSERT(decoder.failed());
    const auto output = decoder.metrics().output_bytes;
    const auto members = decoder.metrics().members;
    expect_error(GzipErrorCode::FAILED_STATE, [&] { decoder.finish(); });
    expect_error(GzipErrorCode::FAILED_STATE, [&] { decoder.consume(nullptr, 0, discard); });
    TEST_ASSERT(decoder.metrics().output_bytes == output && decoder.metrics().members == members);
}

void decode_every_split(const Bytes& input, const Bytes& expected, std::uint64_t members) {
    for (std::size_t cut = 0; cut <= input.size(); ++cut) {
        StreamingGzipDecoder decoder;
        Bytes result;
        decoder.consume(input.data(), cut, collect(result));
        decoder.consume(input.data() + cut, input.size() - cut, collect(result));
        decoder.finish();
        decoder.finish(); // Validating EOF repeatedly is harmless.
        TEST_ASSERT(result == expected && decoder.metrics().output_bytes == expected.size());
        TEST_ASSERT(decoder.metrics().members == members && !decoder.failed());
    }
    StreamingGzipDecoder bytewise;
    Bytes result;
    for (const auto byte : input) bytewise.consume(&byte, 1, collect(result));
    bytewise.finish();
    TEST_ASSERT(result == expected && bytewise.metrics().members == members);
}

void test_literal_vectors_and_borrowed_output() {
    decode_every_split(EMPTY_GZIP, Bytes{}, 1);
    decode_every_split(ABC_GZIP, ABC, 1);
    // Exact output-buffer multiples must drain the trailer without needing an
    // extra input byte or manufacturing a stalled/truncated result.
    for (const std::size_t count : {std::size_t{4096}, std::size_t{8192}, std::size_t{20000}}) {
        const auto bytes = pattern(count);
        decode_every_split(compress_member(bytes), bytes, 1);
    }
    const auto payload = pattern(20000);
    Bytes retained;
    {
        StreamingGzipDecoder decoder;
        const auto envelope = compress_member(payload);
        decoder.consume(envelope.data(), envelope.size(), collect(retained));
        decoder.finish();
    }
    TEST_ASSERT(retained == payload); // Copies survive decoder/output-buffer destruction.
}

void test_optional_headers_and_concatenated_members() {
    const auto payload = pattern(5000);
    const auto optional = compress_member(payload, true);
    TEST_ASSERT(optional.size() > 31 && optional[3] == 0x1e); // EXTRA, NAME, COMMENT, FHCRC.
    decode_every_split(optional, payload, 1);
    Bytes input = EMPTY_GZIP;
    input.insert(input.end(), ABC_GZIP.begin(), ABC_GZIP.end());
    input.insert(input.end(), optional.begin(), optional.end());
    input.insert(input.end(), EMPTY_GZIP.begin(), EMPTY_GZIP.end());
    Bytes expected = ABC;
    expected.insert(expected.end(), payload.begin(), payload.end());
    decode_every_split(input, expected, 4);
    auto bad_header_crc = optional;
    constexpr std::size_t HEADER_CRC_OFFSET = 10 + 2 + 3 + 6 + 8;
    bad_header_crc[HEADER_CRC_OFFSET] ^= 1;
    StreamingGzipDecoder crc;
    expect_error(GzipErrorCode::INVALID_STREAM, [&] {
        crc.consume(bad_header_crc.data(), bad_header_crc.size(), discard);
    });
    require_terminal(crc);
}

void test_every_truncation_and_corruption() {
    for (const auto& complete : {ABC_GZIP, compress_member(pattern(8193), true)}) {
        for (std::size_t size = 0; size < complete.size(); ++size) {
            std::unique_ptr<std::uint8_t[]> short_input;
            if (size != 0) {
                short_input = std::make_unique<std::uint8_t[]>(size);
                std::copy_n(complete.data(), size, short_input.get());
            }
            StreamingGzipDecoder decoder;
            decoder.consume(short_input.get(), size, discard);
            expect_error(size == 0 ? GzipErrorCode::EMPTY_STREAM : GzipErrorCode::TRUNCATED_STREAM,
                         [&] { decoder.finish(); });
            require_terminal(decoder);
        }
    }
    for (const std::size_t offset : {std::size_t{0}, std::size_t{2}, std::size_t{3},
                                   ABC_GZIP.size() - 8, ABC_GZIP.size() - 4}) {
        auto damaged = ABC_GZIP;
        damaged[offset] ^= offset == 3 ? 0x80 : 0x01;
        StreamingGzipDecoder decoder;
        expect_error(GzipErrorCode::INVALID_STREAM, [&] {
            decoder.consume(damaged.data(), damaged.size(), discard);
        });
        require_terminal(decoder);
    }
    // A late trailer error is not allowed to turn already delivered output
    // into a validated member. Root integration must invalidate that prefix.
    StreamingGzipDecoder late;
    Bytes prefix;
    late.consume(ABC_GZIP.data(), ABC_GZIP.size() - 8, collect(prefix));
    TEST_ASSERT(prefix == ABC && late.metrics().members == 0);
    Bytes trailer(ABC_GZIP.end() - 8, ABC_GZIP.end());
    trailer[0] ^= 1;
    expect_error(GzipErrorCode::INVALID_STREAM, [&] { late.consume(trailer.data(), trailer.size(), collect(prefix)); });
    TEST_ASSERT(late.metrics().members == 0);
    require_terminal(late);
}

void test_trailing_garbage_and_gzip_only() {
    for (const Bytes& trailing : {Bytes{0}, Bytes{'x'}, Bytes{0x1f}, Bytes{0x1f,0x8b}, Bytes{'x','y','z'}}) {
        for (const bool one_call : {false, true}) {
            StreamingGzipDecoder decoder;
            Bytes delivered;
            bool rejected = false;
            try {
                if (one_call) {
                    auto both = ABC_GZIP;
                    both.insert(both.end(), trailing.begin(), trailing.end());
                    decoder.consume(both.data(), both.size(), collect(delivered));
                } else {
                    decoder.consume(ABC_GZIP.data(), ABC_GZIP.size(), collect(delivered));
                    decoder.consume(trailing.data(), trailing.size(), collect(delivered));
                }
                decoder.finish();
            } catch (const GzipError&) { rejected = true; }
            TEST_ASSERT(rejected && decoder.failed() && decoder.metrics().members == 1);
            TEST_ASSERT(delivered == ABC);
            require_terminal(decoder);
        }
    }
    // RFC1950 zlib wrapper and raw DEFLATE are not accepted as gzip.
    for (const Bytes& other : {Bytes{0x78,0x9c,0x4b,0x4c,0x4a,0x06,0x00,0x02,0x4d,0x01,0x27},
                              Bytes{0x4b,0x4c,0x4a,0x06,0x00}}) {
        StreamingGzipDecoder decoder;
        expect_error(GzipErrorCode::INVALID_STREAM, [&] { decoder.consume(other.data(), other.size(), discard); });
        require_terminal(decoder);
    }
}

void test_limits_and_state_errors() {
    StreamingGzipDecoder tiny(GzipLimits{2, 1});
    Bytes delivered;
    expect_error(GzipErrorCode::OUTPUT_LIMIT, [&] { tiny.consume(ABC_GZIP.data(), ABC_GZIP.size(), collect(delivered)); });
    TEST_ASSERT(delivered.empty() && tiny.metrics().output_bytes == 0);
    require_terminal(tiny);
    StreamingGzipDecoder exact(GzipLimits{3, 1});
    exact.consume(ABC_GZIP.data(), ABC_GZIP.size(), discard);
    exact.finish();
    TEST_ASSERT(exact.metrics().members == 1 && exact.metrics().output_bytes == 3);
    expect_error(GzipErrorCode::AFTER_FINISH, [&] { exact.consume(nullptr, 0, discard); });
    require_terminal(exact);

    StreamingGzipDecoder chunks(GzipLimits{4096, 1});
    const auto large = compress_member(pattern(9000));
    Bytes prefix;
    expect_error(GzipErrorCode::OUTPUT_LIMIT, [&] { chunks.consume(large.data(), large.size(), collect(prefix)); });
    TEST_ASSERT(prefix.size() == 4096 && chunks.metrics().output_bytes == 4096);
    require_terminal(chunks);
    for (const bool joined : {false, true}) {
        StreamingGzipDecoder decoder(GzipLimits{100, 1});
        Bytes result;
        expect_error(GzipErrorCode::MEMBER_LIMIT, [&] {
            if (joined) {
                auto both = ABC_GZIP;
                both.insert(both.end(), ABC_GZIP.begin(), ABC_GZIP.end());
                decoder.consume(both.data(), both.size(), collect(result));
            } else {
                decoder.consume(ABC_GZIP.data(), ABC_GZIP.size(), collect(result));
                decoder.consume(ABC_GZIP.data(), ABC_GZIP.size(), collect(result));
            }
        });
        TEST_ASSERT(result == ABC && decoder.metrics().members == 1);
        require_terminal(decoder);
    }
    StreamingGzipDecoder null;
    null.consume(nullptr, 0, discard);
    expect_error(GzipErrorCode::NULL_INPUT, [&] { null.consume(nullptr, 1, discard); });
    require_terminal(null);
}

void test_sink_failures_and_reentry() {
    struct SinkFailure { int token; };
    StreamingGzipDecoder throwing;
    const auto input = compress_member(pattern(9000));
    unsigned calls = 0;
    bool preserved = false;
    try {
        throwing.consume(input.data(), input.size(), [&](const std::uint8_t*, std::size_t) {
            if (++calls == 2) throw SinkFailure{123};
        });
    } catch (const SinkFailure& error) { preserved = error.token == 123; }
    TEST_ASSERT(preserved && calls == 2 && throwing.metrics().output_bytes == 4096);
    require_terminal(throwing);
    for (const bool use_finish : {false, true}) {
        for (const bool swallowed : {false, true}) {
            StreamingGzipDecoder decoder;
            bool saw_reentry = false;
            expect_error(swallowed ? GzipErrorCode::FAILED_STATE : GzipErrorCode::REENTRANT_USE, [&] {
                decoder.consume(ABC_GZIP.data(), ABC_GZIP.size(), [&](const std::uint8_t*, std::size_t) {
                    try {
                        if (use_finish) decoder.finish();
                        else decoder.consume(nullptr, 0, discard);
                    } catch (const GzipError& error) {
                        saw_reentry = error.code() == GzipErrorCode::REENTRANT_USE;
                        if (!swallowed) throw;
                    }
                });
            });
            TEST_ASSERT(saw_reentry && decoder.metrics().output_bytes == 0 && decoder.metrics().members == 0);
            require_terminal(decoder);
        }
    }
}
#endif
} // namespace

int main() {
    try {
        test_configuration();
#if MATCHING_ENGINE_HAS_ZLIB
        TEST_ASSERT(StreamingGzipDecoder::available());
        test_literal_vectors_and_borrowed_output();
        test_optional_headers_and_concatenated_members();
        test_every_truncation_and_corruption();
        test_trailing_garbage_and_gzip_only();
        test_limits_and_state_errors();
        test_sink_failures_and_reentry();
        std::cout << "Streaming gzip tests passed (zlib enabled; synthetic/literal gzip only; seven groups).\n";
#else
        TEST_ASSERT(!StreamingGzipDecoder::available());
        expect_error(GzipErrorCode::DISABLED, [] { StreamingGzipDecoder decoder; });
        std::cout << "Streaming gzip disabled-build guard passed; enabled CRC/member tests NOT EXECUTED.\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Streaming gzip test failed: " << error.what() << '\n';
        return 1;
    }
}
