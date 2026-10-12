#include "m0boy/system.h"
#include "m0boy/sg1000.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using namespace m0boy;
    try {
        require(systems().size() == 2, "Source registry incomplete");
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
        require(!parse_cpu("arm") && !parse_system("sms"), "Unknown ids accepted");
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
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
