#include "m0boy/sg1000.h"
#include "m0boy/system.h"
#include "m0_msx_layout.h"
#include "m0_sega_mapper.h"
#include <fstream>

namespace m0boy::sg1000 {

std::optional<Cartridge> Cartridge::from_bytes(std::vector<uint8_t> bytes,
                                                std::string& error) {
    error.clear();
    if (bytes.empty() || bytes.size() > max_rom_bytes) {
        error = "SG-1000 unbanked cartridge must contain 1 to 49152 bytes; "
                "larger cartridges need a mapper profile";
        return std::nullopt;
    }
    Cartridge cartridge;
    cartridge.bytes_ = std::move(bytes);
    return cartridge;
}

std::optional<Cartridge> Cartridge::load(const std::filesystem::path& path,
                                          std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "Cannot read SG-1000 cartridge: " + path.string();
        return std::nullopt;
    }
    const auto size = stream.tellg();
    /* a possible 512-byte copier header is not part of the cartridge */
    if (size <= 0 || size > static_cast<std::streamoff>(max_rom_bytes + copier_header_bytes)) {
        error = "SG-1000 unbanked cartridge must contain 1 to 49152 bytes; "
                "larger cartridges need a mapper profile";
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error = "Incomplete SG-1000 cartridge read";
        return std::nullopt;
    }
    bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(copier_header_skip(bytes.size())));
    return from_bytes(std::move(bytes), error);
}

uint8_t Cartridge::read_banked(BankId bank, uint16_t address) const {
    // A static code reader, not an emulated bus. Unmapped bytes use 0xff;
    // hardware ROM mirrors and RAM are deliberately not inferred here.
    if (bank != 0 || address >= bytes_.size()) return 0xff;
    return bytes_[address];
}

std::optional<CartridgeInfo> inspect(const std::filesystem::path& path,
                                      std::string& error) {
    const auto cartridge = Cartridge::load(path, error);
    if (!cartridge) return std::nullopt;
    return CartridgeInfo{SystemId::sg1000, path.stem().string(),
                         cartridge->bytes().size(), 0, "unbanked"};
}
std::optional<CartridgeInfo> inspect_gamegear(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size < 0x400 || size > 0x400000) {
        error = "Game Gear cartridge must contain 1 KiB to 4 MiB: " + path.string();
        return std::nullopt;
    }
    const size_t rom_bytes = static_cast<size_t>(size) - copier_header_skip(static_cast<size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Cannot read cartridge: " + path.string(); return std::nullopt; }
    file.seekg(static_cast<std::streamoff>(copier_header_skip(static_cast<size_t>(size))));
    std::vector<uint8_t> bytes(rom_bytes);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        error = "Incomplete cartridge read: " + path.string(); return std::nullopt;
    }
    const unsigned mapper = m0_sega_mapper(bytes.data(), bytes.size());
    return CartridgeInfo{SystemId::gamegear, path.stem().string(), rom_bytes, 0,
                         mapper == M0_MAPPER_KOREAN150 ? "korean150" : mapper == M0_MAPPER_CODEMASTERS ? "codemasters" : "sega"};
}
std::optional<CartridgeInfo> inspect_msx(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) { error = "Cannot read MSX cartridge: " + path.string(); return std::nullopt; }
    const auto size = static_cast<size_t>(file.tellg());
    if (!size || size > M0_MSX_MAX_ROM) {
        error = "MSX cartridge must contain 1 byte to 1 MiB: " + path.string();
        return std::nullopt;
    }
    std::vector<uint8_t> bytes(size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        error = "Incomplete cartridge read: " + path.string();
        return std::nullopt;
    }
    const auto layout = m0_msx_layout(bytes.data(), bytes.size());
    if (!layout.valid) {
        error = "Not a plain MSX ROM (\"AB\" header and an INIT inside the cartridge expected): " + path.string();
        return std::nullopt;
    }
    return CartridgeInfo{SystemId::msx, path.stem().string(), size, 0,
                         layout.mapper ? m0_msx_mapper_name(layout.mapper)
                                       : layout.base == 0x8000 ? "unbanked 8000-BFFF" : "unbanked 4000-BFFF"};
}
std::optional<CartridgeInfo> inspect_mastersystem(const std::filesystem::path& path, std::string& error) {
    auto info = inspect_gamegear(path, error);
    if (!info) return info;
    info->system = SystemId::mastersystem;
    return info;
}
} // namespace m0boy::sg1000
