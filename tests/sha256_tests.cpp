#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define TEST_ASSERT(value) do { if (!(value)) throw std::runtime_error("assertion failed: " #value); } while (false)

namespace {
using nasdaq_itch::Sha256;
using Bytes = std::vector<std::uint8_t>;
const std::uint8_t* data(std::string_view text) {
    return reinterpret_cast<const std::uint8_t*>(text.data());
}
template<class Error, class Action>
void expect_error(Action&& action) {
    bool caught = false;
    try { action(); } catch (const Error&) { caught = true; }
    TEST_ASSERT(caught);
}
void check(std::string_view input, const char* expected) {
    Sha256 hash;
    hash.update(data(input), input.size());
    TEST_ASSERT(hash.hex_digest() == expected);
    TEST_ASSERT(hash.hex_digest() == expected);
    TEST_ASSERT(hash.finish() == hash.finish());
}
void test_standard_byte_vectors() {
    // RFC6234 SHA256 test1/test2/test3, plus the standard empty digest.
    // https://www.rfc-editor.org/rfc/rfc6234.html (section8.5).
    check("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 million;
    const std::array<std::uint8_t, 1000> chunk = [] {
        std::array<std::uint8_t, 1000> value{};
        value.fill('a');
        return value;
    }();
    for (unsigned i = 0; i < 1000; ++i) million.update(chunk.data(), chunk.size());
    TEST_ASSERT(million.hex_digest() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    Sha256 binary;
    const std::uint8_t octet = 0x19;
    binary.update(&octet, 1);
    // RFC6234 byte-oriented test6 (not its bit-oriented test5/7/9).
    TEST_ASSERT(binary.hex_digest() == "68aa2e2ee5dff96e3355e6c7ee373e3d6a4e17f75f9518d843709c0c9bc3e3d4");
}
struct Vector { std::size_t size; const char* hex; };
constexpr std::array<Vector, 9> boundary_vectors{{
    {55,"2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
    {56,"31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
    {63,"5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
    {64,"94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
    {65,"fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
    {127,"0fe729ff19257bd6fec853acc2ea355f6b34b58e6c0f684c3e188fcdfcd9baae"},
    {128,"0aedd4856f8eba0963627336ad5144a9a7dbe12498e6066f0165fc97d8ddee4c"},
    {129,"4f1757ae4bffbae86d775b831765b75af154d52f7deaa46dd378051a2d3ad57f"},
    {1024,"ffbad8f947474cfdd5b2bb22d7e0bf5ee8ba2b7af859d0c2bb28622db6a4be47"}
}};
Bytes pattern(std::size_t size) {
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
    return bytes;
}
void test_padding_boundaries_all_splits() {
    // Literal expected digests for byte[i]=(i*37+11)%256 were independently
    // generated using System.Security.Cryptography.SHA256, not this code or
    // a paired encoder. The RFC cases above remain the primary standard KATs.
    for (const auto& item : boundary_vectors) {
        const auto bytes = pattern(item.size);
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            Sha256 hash;
            hash.update(bytes.data(), split);
            hash.update(nullptr, 0);
            hash.update(bytes.data() + split, bytes.size() - split);
            TEST_ASSERT(hash.hex_digest() == item.hex);
        }
        Sha256 bytewise;
        for (const auto byte : bytes) bytewise.update(&byte, 1);
        TEST_ASSERT(bytewise.hex_digest() == item.hex);
    }
}
void test_chunking_and_unaligned_inputs() {
    const auto bytes = pattern(1024);
    Bytes shifted(bytes.size() + 1, 0);
    std::copy(bytes.begin(), bytes.end(), shifted.begin() + 1);
    for (const std::size_t chunk : {std::size_t{1},std::size_t{7},std::size_t{55},std::size_t{56},
                                  std::size_t{63},std::size_t{64},std::size_t{65},std::size_t{129}}) {
        Sha256 hash;
        for (std::size_t position = 0; position < bytes.size();) {
            const auto count = std::min(chunk, bytes.size() - position);
            hash.update(shifted.data() + 1 + position, count);
            position += count;
        }
        TEST_ASSERT(hash.hex_digest() == boundary_vectors.back().hex);
    }
}
void test_state_and_error_contracts() {
    Sha256 empty;
    empty.update(nullptr, 0);
    const auto digest = empty.finish();
    TEST_ASSERT(digest == empty.finish());
    TEST_ASSERT(digest[0] == 0xe3 && digest[1] == 0xb0 && digest[31] == 0x55);
    TEST_ASSERT(empty.hex_digest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_error<std::logic_error>([&] { empty.update(nullptr, 0); });
    const std::uint8_t octet = 'x';
    expect_error<std::logic_error>([&] { empty.update(&octet, 1); });
    expect_error<std::logic_error>([&] { empty.update(nullptr, 1); });
    TEST_ASSERT(empty.finish() == digest);

    Sha256 continuing;
    continuing.update(data("abc"), 3);
    expect_error<std::invalid_argument>([&] { continuing.update(nullptr, 1); });
    // A huge advertised range must fail its length check without touching the
    // one-byte range. Only the 64-bit target can represent this artificial size.
    if constexpr (std::numeric_limits<std::size_t>::max() > Sha256::MAX_INPUT_BYTES) {
        expect_error<std::length_error>([&] {
            continuing.update(&octet, static_cast<std::size_t>(Sha256::MAX_INPUT_BYTES));
        });
    }
    continuing.update(data("def"), 3);
    TEST_ASSERT(continuing.hex_digest() == "bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721");
}
} // namespace

int main() {
    try {
        test_standard_byte_vectors();
        test_padding_boundaries_all_splits();
        test_chunking_and_unaligned_inputs();
        test_state_and_error_contracts();
        std::cout << "Streaming SHA-256 tests passed (RFC6234 KATs and synthetic byte boundaries only).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Streaming SHA-256 test failed: " << exception.what() << '\n';
        return 1;
    }
}
