#include "m0boy/system.h"
#include <fstream>

namespace m0boy::snes {

/* Super NES cartridge images: the header sits at $7FC0 (LoROM) or $FFC0
 * (HiROM) of the file's first bank, some collections prepend 512 bytes.
 * The map mode nibble of the header byte says which; when both offsets
 * look like headers, the one whose checksum complement pairs (comp ^ cksum
 * == $FFFF) wins, else the map nibble, else LoROM. */
std::optional<CartridgeInfo> inspect(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot read " + path.string();
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const size_t file_bytes = size_t(file.tellg());
    if (file_bytes < 0x8000) {
        error = "SNES image too small: " + path.string();
        return std::nullopt;
    }
    const size_t skip = file_bytes % 0x8000 == 512 ? 512 : 0;
    const size_t rom_bytes = file_bytes - skip;
    std::vector<uint8_t> head(0x40);
    auto read_header = [&](size_t at, std::vector<uint8_t>& h) {
        file.clear();
        file.seekg(std::streamoff(skip + at));
        return bool(file.read(reinterpret_cast<char*>(h.data()), std::streamsize(h.size())));
    };
    bool lorom = false, hirom = false;
    if (read_header(0x7FC0, head)) {
        const uint8_t map = head[0x15] & 0x0F;
        const unsigned comp = head[0x3C] | head[0x3D] << 8;
        const unsigned cksum = head[0x3E] | head[0x3F] << 8;
        if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) lorom = true;
        else if (map == 0 || map == 2 || map == 3) lorom = true;
    }
    if (read_header(0xFFC0, head)) {
        const uint8_t map = head[0x15] & 0x0F;
        const unsigned comp = head[0x3C] | head[0x3D] << 8;
        const unsigned cksum = head[0x3E] | head[0x3F] << 8;
        if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) hirom = true;
        else if (map == 1 || map == 5) hirom = true;
    }
    /* the map nibble of the LoROM offset wins a real LoROM header that
     * also pairs at the HiROM offset (some homebrew fill both) */
    std::vector<uint8_t> lo_head(0x40);
    if (lorom && hirom && read_header(0x7FC0, lo_head)) {
        const uint8_t map = lo_head[0x15] & 0x0F;
        if (map == 1 || map == 5) { lorom = false; }
        else hirom = false;
    }
    if (!lorom && !hirom) lorom = true; /* no readable header: assume LoROM */

    CartridgeInfo cartridge;
    cartridge.system = SystemId::snes;
    std::vector<uint8_t> title(0x15);
    if (read_header(lorom ? 0x7FC0 : 0xFFC0, head))
        for (unsigned i = 0; i < 0x15; i++)
            title[i] = (head[i] >= 0x20 && head[i] < 0x7F) ? head[i] : ' ';
    cartridge.title = std::string(title.begin(), title.end());
    while (!cartridge.title.empty() && cartridge.title.back() == ' ') cartridge.title.pop_back();
    if (cartridge.title.empty()) cartridge.title = path.stem().string();
    cartridge.rom_bytes = rom_bytes;
    /* the header's RAM byte: $01-$03 are 2/8/32 KiB; the runtime's SRAM
     * window mirrors 32 KiB at most, larger declarations stay unused */
    static const uint32_t ram_table[4] = {0, 2048, 8192, 32768};
    const uint8_t ram_byte = read_header(lorom ? 0x7FC0 : 0xFFC0, head) ? head[0x18] : 0;
    cartridge.cartridge_ram_bytes = ram_byte < 4 ? ram_table[ram_byte] : 0;
    cartridge.mapper = hirom ? "HiROM" : "LoROM";
    return cartridge;
}
} // namespace m0boy::snes
