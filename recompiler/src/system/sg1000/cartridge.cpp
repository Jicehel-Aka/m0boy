#include "m0boy/sg1000.h"
#include "m0boy/system.h"
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
    if (size <= 0 || size > static_cast<std::streamoff>(max_rom_bytes)) {
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
} // namespace m0boy::sg1000
