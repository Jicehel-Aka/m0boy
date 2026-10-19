#include "m0boy/system.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>

namespace m0boy {
namespace {
constexpr std::array cpu_table = {
    CpuDescriptor{CpuId::sm83, "sm83", true},
    CpuDescriptor{CpuId::z80, "z80", true},
};
constexpr std::array gameboy_targets = {
    TargetId::desktop, TargetId::meta, TargetId::pokitto,
};
constexpr std::array espboy_targets = {TargetId::desktop, TargetId::meta, TargetId::pokitto, TargetId::espboy};
// The Sega consoles' frontends run on every destination, the ESPboy included.
constexpr std::array sega_targets = espboy_targets;
constexpr std::array system_table = {
    SystemDescriptor{SystemId::gameboy, "gameboy", CpuId::sm83, 0x0100,
                     8192, gameboy::inspect, espboy_targets},
    SystemDescriptor{SystemId::sg1000, "sg1000", CpuId::z80, 0x0000,
                     1024, sg1000::inspect, sega_targets},
    SystemDescriptor{SystemId::gamegear, "gg", CpuId::z80, 0x0000,
                     8192, sg1000::inspect_gamegear, sega_targets},
    SystemDescriptor{SystemId::mastersystem, "sms", CpuId::z80, 0x0000,
                     8192, sg1000::inspect_mastersystem, sega_targets},
};
}

std::span<const CpuDescriptor> cpus() { return cpu_table; }
std::span<const SystemDescriptor> systems() { return system_table; }

const CpuDescriptor& cpu_descriptor(CpuId id) {
    for (const auto& cpu : cpus()) if (cpu.id == id) return cpu;
    throw std::invalid_argument("Unknown CPU id");
}

const SystemDescriptor& system_descriptor(SystemId id) {
    for (const auto& system : systems()) if (system.id == id) return system;
    throw std::invalid_argument("Unknown source system id");
}

std::optional<CpuId> parse_cpu(std::string_view name) {
    for (const auto& cpu : cpus()) if (cpu.name == name) return cpu.id;
    return std::nullopt;
}

std::optional<SystemId> parse_system(std::string_view name) {
    for (const auto& system : systems()) if (system.name == name) return system.id;
    return std::nullopt;
}

std::optional<TargetId> parse_target(std::string_view name) {
    if (name == "desktop") return TargetId::desktop;
    if (name == "meta") return TargetId::meta;
    if (name == "pokitto") return TargetId::pokitto;
    if (name == "espboy") return TargetId::espboy;
    return std::nullopt;
}

bool SystemDescriptor::supports(TargetId target) const {
    return std::find(codegen_targets.begin(), codegen_targets.end(), target)
           != codegen_targets.end();
}

std::optional<SystemId> detect_system(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == ".sg") return SystemId::sg1000;
    if (ext == ".gg") return SystemId::gamegear;
    if (ext == ".sms") return SystemId::mastersystem;
    if (ext == ".gb" || ext == ".gbc" || ext == ".sgb") return SystemId::gameboy;
    return std::nullopt;
}
} // namespace m0boy
