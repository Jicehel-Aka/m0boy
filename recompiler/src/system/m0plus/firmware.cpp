#include "m0boy/system.h"

#include <array>
#include <fstream>

namespace m0boy::m0plus {
namespace {
uint32_t word(const std::array<uint8_t, 8>& v, size_t at) {
    return uint32_t(v[at]) | (uint32_t(v[at + 1]) << 8) |
           (uint32_t(v[at + 2]) << 16) | (uint32_t(v[at + 3]) << 24);
}

std::optional<CartridgeInfo> inspect(const std::filesystem::path& path,
                                     SystemId system, uint32_t flash_base,
                                     uint32_t flash_end, uint32_t ram_base,
                                     std::string& error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error = "Cannot read M0+ firmware: " + path.string();
        return std::nullopt;
    }
    if (size < 8 || size > flash_end - flash_base) {
        error = "M0+ firmware must contain a vector table and fit below the loader";
        return std::nullopt;
    }
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t, 8> vectors{};
    if (!file.read(reinterpret_cast<char*>(vectors.data()), vectors.size())) {
        error = "Cannot read M0+ vector table: " + path.string();
        return std::nullopt;
    }
    const uint32_t sp = word(vectors, 0);
    const uint32_t reset = word(vectors, 4);
    if ((sp & 3u) || sp <= ram_base || sp > ram_base + 32768u) {
        error = "M0+ firmware has an invalid initial stack pointer";
        return std::nullopt;
    }
    if (!(reset & 1u) || (reset & ~1u) < flash_base + 8u ||
        uint64_t(reset & ~1u) + 2u > uint64_t(flash_base) + size) {
        error = "M0+ firmware has a reset vector outside the image or without the Thumb bit";
        return std::nullopt;
    }
    error.clear();
    return CartridgeInfo{system, path.stem().string(), size, 0, "firmware image"};
}
} // namespace

std::optional<CartridgeInfo> inspect_meta(const std::filesystem::path& path, std::string& error) {
    return inspect(path, SystemId::meta, 0x4000, 0x40000, 0x20000000, error);
}

std::optional<CartridgeInfo> inspect_pokitto(const std::filesystem::path& path, std::string& error) {
    return inspect(path, SystemId::pokitto, 0, 0x39000, 0x10000000, error);
}
} // namespace m0boy::m0plus
