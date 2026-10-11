#include "m0boy/system.h"
#include "recompiler/system/gameboy/cartridge.h"

namespace m0boy::gameboy {
std::optional<CartridgeInfo> inspect(const std::filesystem::path& path,
                                      std::string& error) {
    error.clear();
    const auto rom = gbrecomp::ROM::load(path);
    if (!rom) {
        error = "Cannot read Game Boy cartridge";
        return std::nullopt;
    }
    if (!rom->is_valid()) {
        error = rom->error();
        return std::nullopt;
    }
    const auto& header = rom->header();
    return CartridgeInfo{SystemId::gameboy, header.title, rom->size(),
                         header.ram_size_bytes, gbrecomp::mbc_type_name(header.mbc_type)};
}
} // namespace m0boy::gameboy
