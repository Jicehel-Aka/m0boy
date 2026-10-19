#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace m0boy {

enum class CpuId { sm83, z80, lua };
enum class SystemId { gameboy, sg1000, gamegear, mastersystem, pico8 };
enum class TargetId { desktop, meta, pokitto, espboy };

struct CpuDescriptor {
    CpuId id;
    std::string_view name;
    bool decoder_available;
};

// Metadata shared across cartridge formats; console-specific headers remain
// in their frontend. SG-1000 images do not pass through the Nintendo parser.
struct CartridgeInfo {
    SystemId system;
    std::string title;
    size_t rom_bytes;
    size_t cartridge_ram_bytes;
    std::string mapper;
};

using InspectCartridge = std::optional<CartridgeInfo> (*)(
    const std::filesystem::path&, std::string& error);

struct SystemDescriptor {
    SystemId id;
    std::string_view name;
    CpuId cpu;
    uint16_t reset_address;
    size_t work_ram_bytes;
    InspectCartridge inspect;
    std::span<const TargetId> codegen_targets;
    // Whether a native project generator exists for this destination.
    bool supports(TargetId target) const;
};

std::span<const CpuDescriptor> cpus();
std::span<const SystemDescriptor> systems();
const CpuDescriptor& cpu_descriptor(CpuId id);
const SystemDescriptor& system_descriptor(SystemId id);
std::optional<CpuId> parse_cpu(std::string_view name);
std::optional<SystemId> parse_system(std::string_view name);
std::optional<TargetId> parse_target(std::string_view name);
// Recognised extensions only. Raw .bin files require an explicit system.
std::optional<SystemId> detect_system(const std::filesystem::path& path);

namespace gameboy {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace pico8 {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
}
namespace sg1000 {
std::optional<CartridgeInfo> inspect(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_gamegear(const std::filesystem::path&, std::string&);
std::optional<CartridgeInfo> inspect_mastersystem(const std::filesystem::path&, std::string&);
}

} // namespace m0boy
