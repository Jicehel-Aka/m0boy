#include "m0boy/system.h"
#include <fstream>

namespace m0boy::pce {

/* PC Engine HuCard images: usually raw (a multiple of 128 KiB, the CPU sees
 * the first 128 KiB unbanked and the rest through the HuC6280's TAM pages);
 * some collections prepend a 128-byte header — a file that is 128 bytes past
 * a multiple of 128 KiB carries one.  The format carries no title: the stem
 * of the file name is the best we have. */
std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot read " + path.string();
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const size_t file_bytes = size_t(file.tellg());
    if (file_bytes < 0x2000) {
        error = "PC Engine image too small: " + path.string();
        return std::nullopt;
    }
    const size_t header = file_bytes % 0x20000 == 128 ? 128 : 0;
    const size_t rom_bytes = file_bytes - header;
    if (rom_bytes < 0x2000) {
        error = "PC Engine image too small: " + path.string();
        return std::nullopt;
    }
    CartridgeInfo cartridge;
    cartridge.system = SystemId::pce;
    cartridge.title = path.stem().string();
    cartridge.rom_bytes = rom_bytes;
    cartridge.cartridge_ram_bytes = 0;
    cartridge.mapper = rom_bytes > 0x20000 ? "banked (TAM)" : "unbanked";
    return cartridge;
}
} // namespace m0boy::pce
