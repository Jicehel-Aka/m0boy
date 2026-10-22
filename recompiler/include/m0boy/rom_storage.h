#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace m0boy::rom {

/* Independent 512-byte sectors: no dictionary, console or CPU dependency. */
enum class Codec : uint8_t { raw, lzss, deflate };
struct Block {
    Codec codec = Codec::raw;
    std::vector<uint8_t> bytes;
};

/* Optimal bit-cost parse of the existing 8/4 LZSS format. */
std::vector<uint8_t> lzss_encode(const uint8_t* data, size_t size);
std::vector<uint8_t> deflate_encode(const uint8_t* data, size_t size);
Block encode(const uint8_t* data, size_t size, bool deflate = false);

/* M0+ costs used by every backend when trading directly-addressable ROM
 * for a decoded sector or an SD read: gm0 estimates on the META (Ristar,
 * 2026-10-22: per-instruction profile over several run lengths, boot and
 * per-frame SPI switches set apart), not device measurements; the Pokitto's
 * card path was not measured and shares them. */
inline constexpr double sd_cycles = 17254;   // a 512-byte sector from the card (SPI, card, FAT)
inline constexpr double lzss_cycles = 11397; // shared Thumb-1 decoder, 512 bytes
inline constexpr double deflate_cycles = 80000; // prefix-table C decoder, conservative
inline constexpr size_t lzss_decoder_bytes = 512; // conservative linked-code allowance
inline constexpr size_t deflate_workspace_bytes = 1032;

/* Benefit per flash byte relative to SD. A negative benefit never becomes a
 * flash candidate merely because a codec compresses well. */
double density(double reads, double sd_cycles, Codec codec, size_t bytes);

/* Whole raw ROM + all requested native code takes precedence over packing.
 * Subtraction form avoids size_t wrap on oversized cartridges/budgets. */
bool fits_raw(size_t flash, size_t runtime, size_t rom_bytes, size_t code_bytes);

/* Promote hot units to direct raw flash, paying only their actual extra
 * bytes over their compressed representation. GB uses 8-sector units;
 * GG/SMS use one sector. Zero-weight units stay compressed. */
std::vector<size_t> raw_units(const std::vector<size_t>& packed_sizes,
                              const std::vector<double>& weights, size_t sectors_per_unit,
                              size_t first_unit, size_t raw_budget, size_t extra_budget);
struct Units { std::vector<size_t> raw, packed; };
Units select_units(const std::vector<size_t>& packed_sizes, const std::vector<double>& weights,
                   size_t sectors_per_unit, size_t first_unit, size_t budget, size_t raw_budget,
                   double sd_cycles);

} // namespace m0boy::rom
