#include "m0boy/system.h"
#include <array>
#include <fstream>
#include <sstream>

namespace m0boy::nes {
namespace {

const char* mapper_name(uint8_t mapper) {
    switch (mapper) {
        case 0: return "NROM";
        case 1: return "MMC1";
        case 2: return "UxROM";
        case 3: return "CNROM";
        case 4: return "MMC3";
        case 5: return "MMC5";
        case 7: return "AxROM";
        case 9: return "MMC2";
        case 10: return "MMC4";
        case 11: return "ColorDreams";
        case 66: return "GxROM";
        default: return nullptr;
    }
}

std::string describe_mapper(uint8_t mapper) {
    if (const char* name = mapper_name(mapper))
        return std::string(name) + " (" + std::to_string(mapper) + ")";
    return "mapper " + std::to_string(mapper);
}

} // namespace

/* iNES 1.0 (and NES 2.0, whose extra fields we do not need yet): the 16-byte
 * header "NES<0x1a>", 16 KiB PRG units, 8 KiB CHR units, the mapper number
 * split across flags 6 and 7, an optional 512-byte trainer the ROM skips. */
std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot read " + path.string();
        return std::nullopt;
    }
    std::array<uint8_t, 16> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), header.size())) {
        error = "Truncated iNES header: " + path.string();
        return std::nullopt;
    }
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1a) {
        error = "Not an iNES image (missing \"NES\\x1a\"): " + path.string();
        return std::nullopt;
    }
    const size_t prg_units = header[4], chr_units = header[5];
    if (!prg_units) {
        error = "iNES image without PRG ROM: " + path.string();
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const size_t file_bytes = size_t(file.tellg());
    const size_t trainer = (header[6] & 0x04) ? 512 : 0;
    const size_t rom_bytes = prg_units * 16384 + chr_units * 8192;
    if (file_bytes - 16 < trainer + rom_bytes) {
        std::ostringstream out;
        out << "Truncated iNES image: header announces " << rom_bytes << " ROM byte(s), "
            << (file_bytes - 16) << " present";
        error = out.str();
        return std::nullopt;
    }
    CartridgeInfo cartridge;
    cartridge.system = SystemId::nes;
    cartridge.title = path.stem().string();
    cartridge.rom_bytes = rom_bytes;
    cartridge.cartridge_ram_bytes = (header[6] & 0x02) ? 8192 : 0;
    cartridge.mapper = describe_mapper(uint8_t((header[7] & 0xf0) | (header[6] >> 4)));
    return cartridge;
}
} // namespace m0boy::nes
