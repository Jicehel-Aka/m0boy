#include "m0boy/sg1000_generator.h"
#include "m0boy/sg1000.h"
#include "m0boy/cpu/z80/decoder.h"
#include "z80.h"
#include <fstream>
#include <iomanip>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
namespace m0boy::sg1000 {
namespace fs=std::filesystem;
namespace {
uint8_t read_rom(void *ctx,uint16_t address){return static_cast<const Cartridge*>(ctx)->read_banked(0,address);}
std::string read(const fs::path& path){std::ifstream file(path);if(!file)throw std::runtime_error("Cannot read template: "+path.string());return {std::istreambuf_iterator<char>(file),{}};}
void write(const fs::path& path,const std::string& data){std::ofstream file(path);if(!file||!(file<<data))throw std::runtime_error("Cannot write generated file: "+path.string());}
void copy_template(const fs::path& from,const fs::path& to){fs::copy_file(from,to,fs::copy_options::overwrite_existing);}
std::string replace(std::string s,const std::string& what,const std::string& value){size_t pos=0;while((pos=s.find(what,pos))!=std::string::npos){s.replace(pos,what.size(),value);pos+=value.size();}return s;}
}
bool generate(const fs::path& path,const GenerateOptions& o,std::string& error){
    error.clear();auto cartridge=Cartridge::load(path,error);if(!cartridge)return false;
    try{
        const auto tpl=o.meta_templates.parent_path()/"sg1000";
        if(!fs::is_regular_file(tpl/"runtime"/"z80.c"))throw std::runtime_error("SG-1000 templates missing next to targets/meta");
        std::set<uint16_t> candidates;
        cpu::z80::Decoder decoder(*cartridge);
        if(o.coverage.empty()){
            std::queue<uint16_t> pending;pending.push(0);pending.push(0x38);pending.push(0x66);
            std::set<uint16_t> seen;
            while(!pending.empty()&&candidates.size()<o.compiled_limit){auto pc=pending.front();pending.pop();
                if(pc>=cartridge->bytes().size()||!seen.insert(pc).second)continue;
                const auto in=decoder.decode(pc);if(pc+in.length>cartridge->bytes().size())continue;
                candidates.insert(pc);
                if(in.target)pending.push(*in.target);
                if(in.flow==cpu::z80::Flow::next||in.flow==cpu::z80::Flow::call||
                   in.flow==cpu::z80::Flow::halt||in.flow==cpu::z80::Flow::repeat||in.conditional)
                    pending.push(static_cast<uint16_t>(pc+in.length));
            }
        }else for(const auto& coverage:o.coverage){
            std::ifstream file(coverage);std::string line;
            if(!file||!std::getline(file,line)||line!="# m0boy.sg1000.coverage.v1")
                throw std::runtime_error("Expected SG-1000 coverage v1: "+coverage.string());
            while(std::getline(file,line)){
                if(line.empty())continue;size_t used=0;unsigned value=std::stoul(line,&used,16);
                if(used!=line.size()||value>0xffff)throw std::runtime_error("Invalid SG-1000 coverage address");
                // Coverage files list the hottest entries first. Keep that priority
                // when the flash budget cannot compile every observed entry.
                if(value<cartridge->bytes().size()&&candidates.size()<o.compiled_limit)
                    candidates.insert((uint16_t)value);
            }
            if(!file.eof())throw std::runtime_error("Cannot read SG-1000 coverage");
        }
        // compile.sh writes its log into the project directory before generation.
        auto foreign=[&]{for(const auto& e:fs::directory_iterator(o.output))if(e.path().filename()!="compile.log")return true;return false;};
        if(fs::exists(o.output)&&!fs::exists(o.output/"sg1000-project.txt")&&foreign())
            throw std::runtime_error("Output directory is not an SG-1000 project; choose a fresh directory");
        fs::create_directories(o.output/"src");fs::create_directories(o.output/"host");
        const auto src=o.output/"src";
        const auto marker=o.output/"sg1000-project.txt";
        // Only same-destination regeneration is accepted: a project owns one
        // platform and cannot silently mix drivers from another generation.
        const std::string target=o.target==TargetId::meta?"meta":o.target==TargetId::pokitto?"pokitto":"desktop";
        if(fs::exists(marker)&&read(marker)!=target+"\n")throw std::runtime_error("Use a separate output directory for each destination");
        for(const auto& entry:fs::directory_iterator(tpl/"runtime"))if(entry.is_regular_file())copy_template(entry.path(),src/entry.path().filename());
        copy_template(tpl/"host"/"sg_host.c",o.output/"host"/"sg_host.c");
        write(o.output/"host"/"CMakeLists.txt",replace(read(tpl/"host"/"CMakeLists.txt.in"),"@NAME@",o.name));
        std::ostringstream generated;
        generated<<"#include \"sg1000.h\"\n"
            <<"static inline uint8_t sg_native_read(z80_t *c,uint16_t a){sg1000_t *s=c->user;"
            <<"if(a>=0xc000)return s->ram[a&1023];";
        const auto rom_size=cartridge->bytes().size();
        if(!(rom_size&(rom_size-1)))generated<<"a&="<<(rom_size-1)<<";";
        generated<<"return a<"<<rom_size<<"?s->rom[a]:255;}\n"
            <<"static inline void sg_native_write(z80_t *c,uint16_t a,uint8_t v){if(a>=0xc000)((sg1000_t*)c->user)->ram[a&1023]=v;}\n"
            <<"#define Z80_NATIVE_READ sg_native_read\n#define Z80_NATIVE_WRITE sg_native_write\n"
            <<"#define Z80_NATIVE_INLINE __attribute__((always_inline))\n#include \"z80_ops.h\"\n";
        z80_t reader{};reader.user=&*cartridge;reader.read=read_rom;
        std::vector<uint16_t> compiled;
        std::vector<z80_decoded_t> decoded;
        for(auto pc:candidates){if(compiled.size()>=o.compiled_limit)break;reader.pc=pc;z80_decoded_t d{};
            if(!z80_decode(&reader,&d)||pc+d.length>cartridge->bytes().size())continue;
            compiled.push_back(pc);decoded.push_back(d);
            generated<<"/* "<<decoder.decode(pc).mnemonic<<" */\nstatic unsigned n_"<<pc<<"(z80_t *c){\n"
                <<" const z80_decoded_t d={"<<d.immediate<<","<<d.length<<","<<unsigned(d.opcode)<<","<<unsigned(d.index)<<","<<unsigned(d.group)<<","<<unsigned(d.displacement)<<","<<unsigned(d.refresh)<<","<<d.extra_cycles<<"};\n"
                <<" return z80_execute(c,&d);\n}\n";
        }
        // Fall through contiguous ordinary instructions, bounded by the next
        // raster event. Port accesses stay single instructions: their audio/VDP
        // side effects must observe all preceding T-states. EI delay similarly
        // keeps single stepping until an interrupt can be serviced.
        auto ordinary=[](const z80_decoded_t& d){
            return d.group!=2 && !(d.group==0 && (d.opcode==0xd3||d.opcode==0xdb));
        };
        generated<<"unsigned sg_compiled(z80_t *c){unsigned cycles=0;sg1000_t *s=c->user;switch(c->pc){\n";
        for(size_t i=0;i<compiled.size();i++){
            auto pc=compiled[i];
            generated<<"case "<<pc<<":cycles+=n_"<<pc<<"(c);\n";
            bool chain=i+1<compiled.size() && pc+decoded[i].length==compiled[i+1]
                && decoder.decode(pc).flow==cpu::z80::Flow::next
                && ordinary(decoded[i]) && ordinary(decoded[i+1]);
            if(chain)generated<<"if(c->ei_delay||s->line_cycles+cycles>=SG_LINE_CYCLES)return cycles; /* fall through */\n";
            else generated<<"return cycles;\n";
        }
        generated<<"default:return 0;}}\n";write(src/"sg_gen.c",generated.str());
        std::ostringstream data;data<<"#include <stdint.h>\n#include <stddef.h>\nconst uint8_t sg_rom[]={\n";
        unsigned count=0;for(auto byte:cartridge->bytes()){data<<unsigned(byte)<<",";if(++count%32==0)data<<"\n";}
        data<<"\n};\nconst size_t sg_rom_size=sizeof(sg_rom);\n";write(src/"sg_rom.c",data.str());
        if(o.target!=TargetId::desktop){
            copy_template(tpl/"platform"/"sg_main.cpp",src/"sg_main.cpp");
            auto platform=o.meta_templates/"platform"/(o.target==TargetId::pokitto?"pokitto":"meta");
            for(const char* name:{"meta_target.h","meta_lcd.h","meta_clock.h"})copy_template(o.meta_templates/"platform"/"src"/name,src/name);
            copy_template(tpl/"platform"/"sg_audio.h",src/"sg_audio.h");
            auto audio_header=read(o.meta_templates/"platform/src/meta_audio.h");
            audio_header=replace(replace(audio_header,"70224u","59736u"),"4194304u","3579545u");
            write(src/"meta_audio.h",audio_header);
            const std::string audio_name=o.target==TargetId::pokitto?"pokitto_audio.c":"meta_audio.cpp";
            auto audio=read(platform/audio_name);
            audio=replace(audio,"#include \"gbl.h\"","#include \"sg_audio.h\"");
            audio=replace(replace(audio,"gbl_apu_render","sg_platform_audio_render"),"gbl_apu_set_rate","sg_platform_audio_rate");
            write(src/audio_name,audio);
            std::string version=read(o.meta_templates/(o.target==TargetId::pokitto?fs::path("platform/pokitto/VERSION"):fs::path("VERSION")));
            while(!version.empty()&&(version.back()=='\n'||version.back()=='\r'))version.pop_back();
            write(src/"gbl_config.h","#pragma once\n#define GBL_TARGET_POKITTO "+std::to_string(o.target==TargetId::pokitto)+"\n#define SG_GEN_VERSION \""+version+"\"\n");
            write(src/"gbl_profile.h","#pragma once\n#define META_LCD_DMA 1\n#define META_LCD_12BPP 0\n#define META_LCD_SPI_MHZ 24\n#define POKITTO_LCD_ASM 0\n#define POKITTO_RAM_BANKS 0\n#define POKITTO_AUDIO_ASM 0\n#define META_AUDIO_DMA 0\n");
            if(o.target==TargetId::pokitto){
                for(const char* name:{"pokitto_startup.c","pokitto_clock.c","pokitto_buttons.c","pokitto_lcd.c","pokitto_hw.h","lpc11u6x.h"})copy_template(platform/name,src/name);
                write(o.output/"pokitto.ld",replace(read(platform/"pokitto.ld.in"),"@NAME@",o.name));
                copy_template(tpl/"arm-toolchain.cmake",o.output/"arm-toolchain.cmake");copy_template(tpl/"checksum.py",o.output/"checksum.py");
                write(o.output/"CMakeLists.txt",replace(read(tpl/"CMakeLists.txt.in"),"@NAME@",o.name));
            } else {
                for(const char* name:{"meta_startup.c","meta_clock.cpp","meta_lcd.cpp","meta_lcd_pack.h","meta_lcd_queue.h","meta_dma.h","meta_dma.cpp","meta_spi.c","meta_hw.h"})copy_template(platform/name,src/name);
                std::string config=replace(replace(read(platform/"platformio.ini.in"),"@NAME@",o.name),"@COMPILER@","gcc");
                write(o.output/"platformio.ini",config);
                write(o.output/"gamebuino_meta.ld",replace(read(platform/"gamebuino_meta.ld.in"),"@NAME@",o.name));
                fs::create_directories(o.output/"boards");copy_template(platform/"boards"/"gamebuino_meta.json",o.output/"boards"/"gamebuino_meta.json");
                fs::create_directories(o.output/"toolchain");
                for(const auto& entry:fs::directory_iterator(o.meta_templates/"toolchain"))if(entry.is_regular_file())copy_template(entry.path(),o.output/"toolchain"/entry.path().filename());
            }
        }
        write(marker,target+"\n");
        write(o.output/"README.md","# SG-1000 project: "+o.name+"\n\nExperimental native Z80 instructions with interpreter fallback.\n\n"+
            "Source: SG-1000. Destination: "+target+". Compiled entries: "+std::to_string(compiled.size())+".\n\n"+
            "Host: cmake -G Ninja -S host -B host/build && ninja -C host/build\n"+
            "Run: host/build/"+o.name+"_host --frames 300 [--interp] [--shot image.ppm]\n\n"+
            (o.target==TargetId::pokitto?"Firmware: cmake -G Ninja -S . -B build -DCMAKE_TOOLCHAIN_FILE=arm-toolchain.cmake && ninja -C build\n":o.target==TargetId::meta?"Firmware: pio run\n":"")+
            "\nBaseline NTSC, unbanked cartridge, scanline TMS9918A; SN76489 tone/noise synthesis and DAC audio. No measured device speed or general compatibility claim.\n");
        return true;
    }catch(const std::exception& e){error=e.what();return false;}
}
} // namespace m0boy::sg1000
