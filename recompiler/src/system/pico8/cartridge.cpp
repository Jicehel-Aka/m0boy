#include "m0boy/system.h"
#include <system_error>

namespace m0boy::pico8 {

std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code code;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size < 16 || size > 1024 * 1024) {
        error = "PICO-8 cartridge (.p8 or .p8.png) not readable: " + path.string();
        return std::nullopt;
    }
    auto title = path.stem().string();
    if (title.size() > 3 && title.compare(title.size() - 3, 3, ".p8") == 0) title.resize(title.size() - 3);
    return CartridgeInfo{SystemId::pico8, title, static_cast<size_t>(size), 0, "none"};
}

} // namespace m0boy::pico8
