#ifndef SNAPSHOT_FILE_HPP
#define SNAPSHOT_FILE_HPP

#include "snapshot.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#endif

struct SnapshotFileData {
    uint64_t snapshot_sequence = 0;
    OrderBookSnapshot snapshot;
};

class SnapshotFile {
    static constexpr std::array<uint8_t, 8> MAGIC{{'D','M','E','S','N','A','P',0}};
    static constexpr uint32_t FILE_VERSION = 1;
    static constexpr uint64_t MAX_ORDERS = 100'000'000ULL;

    template<typename T>
    static void append(std::vector<uint8_t>& bytes, T value) {
        using U = std::make_unsigned_t<T>;
        U bits = static_cast<U>(value);
        for (size_t i = 0; i < sizeof(T); ++i) {
            bytes.push_back(static_cast<uint8_t>((bits >> (i * 8U)) & 0xffU));
        }
    }

    template<typename T>
    static T read(const std::vector<uint8_t>& bytes, size_t& offset) {
        if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) {
            throw std::runtime_error("Truncated snapshot file");
        }
        using U = std::make_unsigned_t<T>;
        U value = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            value |= static_cast<U>(bytes[offset++]) << (i * 8U);
        }
        return static_cast<T>(value);
    }

    static uint64_t checksum(const uint8_t* data, size_t size) noexcept {
        uint64_t hash = 1469598103934665603ULL;
        for (size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static void durable_flush(const std::filesystem::path& path) {
#ifdef _WIN32
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot reopen snapshot for durable flush");
        }
        const BOOL flushed = FlushFileBuffers(file);
        CloseHandle(file);
        if (!flushed) throw std::runtime_error("Snapshot durable flush failed");
#else
        const int file = ::open(path.c_str(), O_RDONLY);
        if (file < 0) throw std::runtime_error("Cannot reopen snapshot for durable flush");
        const int result = ::fsync(file);
        ::close(file);
        if (result != 0) throw std::runtime_error("Snapshot durable flush failed");
#endif
    }

    static void atomic_replace(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
#ifdef _WIN32
        if (!MoveFileExW(source.c_str(), destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Atomic snapshot replacement failed");
        }
#else
        std::filesystem::rename(source, destination);
        const std::filesystem::path parent = destination.parent_path().empty()
            ? std::filesystem::current_path() : destination.parent_path();
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory < 0) throw std::runtime_error("Cannot open snapshot directory");
        const int result = ::fsync(directory);
        ::close(directory);
        if (result != 0) throw std::runtime_error("Snapshot directory flush failed");
#endif
    }

public:
    static void save_atomic(const std::filesystem::path& path,
                            uint64_t snapshot_sequence,
                            const OrderBookSnapshot& snapshot) {
        std::vector<uint8_t> bytes;
        bytes.reserve(128 + snapshot.active_orders.size() * 49);
        bytes.insert(bytes.end(), MAGIC.begin(), MAGIC.end());
        append<uint32_t>(bytes, FILE_VERSION);
        append<uint32_t>(bytes, snapshot.snapshot_version);
        append<uint64_t>(bytes, snapshot_sequence);
        append<uint64_t>(bytes, snapshot.last_applied_command_sequence.get());
        append<int64_t>(bytes, snapshot.instrument_config.tick_size);
        append<uint64_t>(bytes, snapshot.instrument_config.lot_size);
        append<int64_t>(bytes, snapshot.instrument_config.min_price);
        append<int64_t>(bytes, snapshot.instrument_config.max_price);
        append<uint64_t>(bytes, snapshot.instrument_config.max_order_quantity);
        append<int64_t>(bytes, snapshot.price_ladder_config.min_price);
        append<int64_t>(bytes, snapshot.price_ladder_config.max_price);
        append<int64_t>(bytes, snapshot.price_ladder_config.tick);
        append<uint64_t>(bytes, static_cast<uint64_t>(snapshot.active_orders.size()));
        for (const SnapshotOrder& order : snapshot.active_orders) {
            append<uint64_t>(bytes, order.order_id.get());
            append<uint8_t>(bytes, static_cast<uint8_t>(order.side));
            append<int64_t>(bytes, order.price.get());
            append<uint64_t>(bytes, order.original_quantity.get());
            append<uint64_t>(bytes, order.remaining_quantity.get());
            append<uint64_t>(bytes, order.priority_sequence.get());
        }
        append<uint64_t>(bytes, checksum(bytes.data(), bytes.size()));

        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Cannot create snapshot temporary file");
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) throw std::runtime_error("Cannot write snapshot temporary file");
        }
        durable_flush(temporary);
        (void)load(temporary); // validate before replacing the last known-good file
        atomic_replace(temporary, path);
    }

    static SnapshotFileData load(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) throw std::runtime_error("Cannot open snapshot file");
        const std::streamoff length = input.tellg();
        if (length < 0 || length > static_cast<std::streamoff>(1ULL << 34U)) {
            throw std::runtime_error("Invalid snapshot file size");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        input.seekg(0);
        if (!bytes.empty()) {
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
        }
        if (!input || bytes.size() < MAGIC.size() + sizeof(uint64_t)) {
            throw std::runtime_error("Truncated snapshot file");
        }
        size_t checksum_offset = bytes.size() - sizeof(uint64_t);
        size_t checksum_reader = checksum_offset;
        const uint64_t stored_checksum = read<uint64_t>(bytes, checksum_reader);
        if (stored_checksum != checksum(bytes.data(), checksum_offset)) {
            throw std::runtime_error("Snapshot checksum mismatch");
        }

        size_t offset = 0;
        for (uint8_t expected : MAGIC) {
            if (read<uint8_t>(bytes, offset) != expected) {
                throw std::runtime_error("Invalid snapshot magic");
            }
        }
        if (read<uint32_t>(bytes, offset) != FILE_VERSION) {
            throw std::runtime_error("Unsupported snapshot file version");
        }
        SnapshotFileData result;
        result.snapshot.snapshot_version = read<uint32_t>(bytes, offset);
        result.snapshot_sequence = read<uint64_t>(bytes, offset);
        result.snapshot.last_applied_command_sequence = CommandSequence(read<uint64_t>(bytes, offset));
        result.snapshot.instrument_config.tick_size = read<int64_t>(bytes, offset);
        result.snapshot.instrument_config.lot_size = read<uint64_t>(bytes, offset);
        result.snapshot.instrument_config.min_price = read<int64_t>(bytes, offset);
        result.snapshot.instrument_config.max_price = read<int64_t>(bytes, offset);
        result.snapshot.instrument_config.max_order_quantity = read<uint64_t>(bytes, offset);
        result.snapshot.price_ladder_config.min_price = read<int64_t>(bytes, offset);
        result.snapshot.price_ladder_config.max_price = read<int64_t>(bytes, offset);
        result.snapshot.price_ladder_config.tick = read<int64_t>(bytes, offset);
        const uint64_t count = read<uint64_t>(bytes, offset);
        if (count > MAX_ORDERS || count > (checksum_offset - offset) / 41U) {
            throw std::runtime_error("Invalid snapshot order count");
        }
        result.snapshot.active_orders.reserve(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            result.snapshot.active_orders.push_back(SnapshotOrder{
                OrderId(read<uint64_t>(bytes, offset)),
                static_cast<Side>(read<uint8_t>(bytes, offset)),
                Price(read<int64_t>(bytes, offset)),
                Quantity(read<uint64_t>(bytes, offset)),
                Quantity(read<uint64_t>(bytes, offset)),
                PrioritySequence(read<uint64_t>(bytes, offset))});
        }
        if (offset != checksum_offset) throw std::runtime_error("Unexpected snapshot payload");
        return result;
    }
};

#endif
