#pragma once

#include "m0boy/memory.h"
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace m0boy::sg1000 {

// Cartridges dumped by some copiers and emulators start with a 512-byte
// header (the ROM itself is then a whole number of 16 KiB pages).  One rule,
// here, used by the cartridge readers and the project generator; the host
// harnesses repeat it in C (targets/common/host/m0_host_util.h) and
// compile.sh applies it to the card copy of a streamed ROM.
inline constexpr size_t copier_header_bytes = 512;
inline constexpr size_t copier_header_skip(size_t file_bytes) {
    return (file_bytes & 0x3fffu) == copier_header_bytes ? copier_header_bytes : size_t{0};
}

// Initial frontend: unbanked ROM only. Expansion RAM and cartridge mappers
// require separate profiles rather than silently treating them as this board.
class Cartridge final : public ByteReader {
public:
    static constexpr size_t max_rom_bytes = 0xc000;
    static std::optional<Cartridge> load(const std::filesystem::path& path,
                                         std::string& error);
    static std::optional<Cartridge> from_bytes(std::vector<uint8_t> bytes,
                                               std::string& error);
    std::span<const uint8_t> bytes() const { return bytes_; }
    uint8_t read_banked(BankId bank, uint16_t address) const override;

private:
    std::vector<uint8_t> bytes_;
};

} // namespace m0boy::sg1000
