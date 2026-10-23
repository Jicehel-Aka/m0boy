#include "m0boy/system.h"
#include "m0boy/sg1000.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using namespace m0boy;
    try {
        require(systems().size() == 6, "Source registry incomplete");
        require(detect_system("game.sms") == SystemId::mastersystem && parse_system("sms") == SystemId::mastersystem,
                "Master System detection failed");
        const auto& gg = system_descriptor(SystemId::gamegear);
        require(gg.cpu == CpuId::z80 && gg.work_ram_bytes == 8192 && parse_system("gg") == SystemId::gamegear,
                "Game Gear profile incorrect");
        require(detect_system("game.GG") == SystemId::gamegear, "GG detection failed");
        const auto& msx = system_descriptor(SystemId::msx);
        require(msx.cpu == CpuId::z80 && msx.reset_address == 0x4000 && parse_system("msx") == SystemId::msx &&
                    detect_system("game.MX1") == SystemId::msx && msx.supports(TargetId::meta) &&
                    msx.supports(TargetId::pokitto) && !msx.supports(TargetId::espboy),
                "MSX profile incorrect");
        const auto& sg = system_descriptor(SystemId::sg1000);
        require(sg.cpu == CpuId::z80 && sg.reset_address == 0 &&
                sg.work_ram_bytes == 1024, "SG-1000 profile incorrect");
        require(cpu_descriptor(sg.cpu).decoder_available, "Z80 decoder missing");
        for (auto target : {TargetId::desktop, TargetId::meta, TargetId::pokitto}) {
            require(sg.supports(target), "SG-1000 target missing");
            require(system_descriptor(SystemId::gameboy).supports(target), "Lost GB target");
        }
        require(detect_system("game.SG") == SystemId::sg1000, "SG detection failed");
        require(detect_system("game.sgb") == SystemId::gameboy, "SGB is a GB cartridge");
        require(!detect_system("game.bin"), "Raw binary falsely identified");
        require(!parse_cpu("arm") && !parse_system("nes"), "Unknown ids accepted");
        std::string error;
        require(!sg1000::Cartridge::from_bytes({}, error) && !error.empty(), "Empty ROM accepted");
        require(!sg1000::Cartridge::from_bytes(std::vector<uint8_t>(0xc001), error),
                "Unsupported mapper accepted");
        auto cartridge = sg1000::Cartridge::from_bytes({0xc3, 0x34, 0x12}, error);
        require(cartridge && error.empty(), "Headerless SG cartridge rejected");
        require(cartridge->read_banked(0, 0) == 0xc3 &&
                cartridge->read_banked(0, 2) == 0x12 &&
                cartridge->read_banked(0, 3) == 0xff &&
                cartridge->read_banked(1, 0) == 0xff &&
                cartridge->read_banked(0, 0xc000) == 0xff, "Static reader mapping incorrect");
        auto largest = sg1000::Cartridge::from_bytes(std::vector<uint8_t>(0xc000, 0xa5), error);
        require(largest && largest->read_banked(0, 0xbfff) == 0xa5, "48 KiB boundary rejected");
        /* a 512-byte copier header is not part of the cartridge (one rule:
         * sg1000.h; the host harnesses repeat it in m0_host_util.h) */
        require(sg1000::copier_header_skip(0x4000) == 0 && sg1000::copier_header_skip(0x4000 + 512) == 512 &&
                    sg1000::copier_header_skip(0xc000 + 512) == 512, "copier header rule incorrect");
        {
            const auto path = std::filesystem::temp_directory_path() / "m0boy_copier_header_test.sg";
            {
                std::vector<uint8_t> bytes(512, 0x5a);
                bytes.resize(0x4000 + 512, 0xa5);
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            }
            auto with_header = sg1000::Cartridge::load(path, error);
            require(with_header && with_header->bytes().size() == 0x4000 &&
                        with_header->read_banked(0, 0) == 0xa5 && with_header->read_banked(0, 0x3fff) == 0xa5,
                    "SG cartridge with a copier header misread");
            auto gg = sg1000::inspect_gamegear(path, error);
            require(gg && gg->rom_bytes == 0x4000, "Game Gear inspection keeps the copier header");
            auto sms = sg1000::inspect_mastersystem(path, error);
            require(sms && sms->rom_bytes == 0x4000 && sms->system == SystemId::mastersystem,
                    "Master System inspection keeps the copier header");
            std::filesystem::remove(path);
        }
        {   /* MSX: "AB" header, INIT inside the cartridge; the page follows INIT */
            const auto path = std::filesystem::temp_directory_path() / "m0boy_msx_test.mx1";
            auto write = [&](std::vector<uint8_t> bytes) {
                std::ofstream file(path, std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            };
            std::vector<uint8_t> rom(0x4000, 0xff);
            rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x40;
            write(rom);
            auto low = sg1000::inspect_msx(path, error);
            require(low && low->system == SystemId::msx && low->rom_bytes == 0x4000 && low->mapper == "unbanked 4000-BFFF",
                    "MSX 4000 cartridge misread");
            rom[3] = 0x80; /* INIT at 8000: a 16 KiB ROM sits in page 2 */
            write(rom);
            auto high = sg1000::inspect_msx(path, error);
            require(high && high->mapper == "unbanked 8000-BFFF", "MSX 8000 cartridge misread");
            rom[3] = 0xc0; /* INIT outside the cartridge */
            write(rom);
            require(!sg1000::inspect_msx(path, error) && !error.empty(), "MSX INIT outside the ROM accepted");
            rom[0] = 'X';
            write(rom);
            require(!sg1000::inspect_msx(path, error), "MSX cartridge without header accepted");
            write(std::vector<uint8_t>(0x8001, 0));
            require(!sg1000::inspect_msx(path, error), "MSX mapper cartridge accepted");
            std::filesystem::remove(path);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
