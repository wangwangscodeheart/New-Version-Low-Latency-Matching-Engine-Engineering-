#include "../src/event_sink.hpp"
#include "../src/orderbook.hpp"
#include "../src/reference_engine.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <string>

namespace {

[[noreturn]] void fail(uint32_t seed, uint64_t sequence, const char* what) {
    std::cerr << "Differential failure: seed=" << seed
              << ", sequence=" << sequence << ", mismatch=" << what << '\n';
    throw std::runtime_error("reference/optimized engine mismatch");
}

Command generate_command(std::mt19937& random, uint64_t sequence) {
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<uint64_t> id(0, 340);
    if (action(random) < 68) {
        const uint64_t order_id = id(random);
        const Side side = action(random) < 50 ? Side::BUY : Side::SELL;
        int64_t price = 100 + static_cast<int64_t>(action(random) % 100) * 100;
        const int price_case = action(random);
        if (price_case < 3) price = 0;
        else if (price_case < 6) price = 20'100;
        else if (price_case < 12) ++price;
        uint64_t quantity = static_cast<uint64_t>((action(random) % 20) + 1) * 5;
        const int quantity_case = action(random);
        if (quantity_case < 3) quantity = 0;
        else if (quantity_case < 6) quantity = 105;
        else if (quantity_case < 12) ++quantity;
        const int tif_case = action(random);
        const TimeInForce tif = tif_case < 80 ? TimeInForce::GTC
            : tif_case < 90 ? TimeInForce::IOC : TimeInForce::POST_ONLY;
        return NewOrderCommand(CommandSequence(sequence), OrderId(order_id), side,
                               Price(price), Quantity(quantity), tif);
    }
    return CancelOrderCommand(CommandSequence(sequence), OrderId(id(random)));
}

void run_seed(uint32_t seed, uint64_t command_count) {
    constexpr size_t capacity = 256;
    const InstrumentConfig config{100, 5, 100, 20'000, 100};
    OrderBook optimized(capacity, true, {}, 2, config);
    ReferenceEngine reference(capacity, config);
    std::mt19937 random(seed);
    std::vector<EngineEvent> expected;
    VectorEventSink actual;

    for (uint64_t sequence = 1; sequence <= command_count; ++sequence) {
        const Command command = generate_command(random, sequence);
        actual.clear();
        const ProcessResult optimized_result = optimized.process(command, actual);
        const ProcessStatus reference_status = reference.process(command, expected);
        if (optimized_result.status != reference_status) fail(seed, sequence, "status");
        if (!optimized_result.event_output_complete) fail(seed, sequence, "event sink");
        if (actual.events() != expected) fail(seed, sequence, "events");
        if (!optimized.check_invariants()) fail(seed, sequence, "optimized invariant");
        if (sequence % 25 == 0 &&
            !(optimized.capture_state() == reference.capture_state())) {
            fail(seed, sequence, "book state");
        }
    }
    if (!(optimized.capture_state() == reference.capture_state())) {
        fail(seed, command_count, "final book state");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        uint32_t seeds = 25;
        uint64_t commands = 4'000;
        uint32_t seed_base = 0xD1FF0000U;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (i + 1 >= argc) throw std::runtime_error("missing differential option value");
            const uint64_t value = std::stoull(argv[++i], nullptr, 0);
            if (argument == "--seeds") seeds = static_cast<uint32_t>(value);
            else if (argument == "--commands") commands = value;
            else if (argument == "--seed-base") seed_base = static_cast<uint32_t>(value);
            else throw std::runtime_error("unknown differential option: " + argument);
        }
        if (seeds == 0 || commands == 0) {
            throw std::runtime_error("differential scale must be non-zero");
        }
        for (uint32_t offset = 1; offset <= seeds; ++offset) {
            run_seed(seed_base + offset, commands);
        }
        std::cout << "Differential tests passed: " << seeds << " seeds x "
                  << commands << " commands, seed_base=" << seed_base << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
