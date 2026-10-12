#include "m0boy/cpu/z80/decoder.h"
#include "z80_ops.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
struct Bus : m0boy::ByteReader {
    std::array<uint8_t,65536> memory{};
    uint16_t port=0;uint8_t value=0;
    uint8_t read_banked(m0boy::BankId,uint16_t a)const override{return memory[a];}
    static uint8_t read(void *p,uint16_t a){return static_cast<Bus*>(p)->memory[a];}
    static void write(void *p,uint16_t a,uint8_t v){static_cast<Bus*>(p)->memory[a]=v;}
    static uint8_t in(void *p,uint16_t a){static_cast<Bus*>(p)->port=a;return 0xa5;}
    static void out(void *p,uint16_t a,uint8_t v){auto& b=*static_cast<Bus*>(p);b.port=a;b.value=v;}
};
static void check(bool value,const char *message){if(!value)throw std::runtime_error(message);}
int main(){try{
    Bus bus;z80_t c{};c.user=&bus;c.read=Bus::read;c.write=Bus::write;c.in=Bus::in;c.out=Bus::out;
    m0boy::cpu::z80::Decoder disasm(bus);
    for(unsigned a=0;a<256;a++)for(unsigned b=0;b<256;b++)for(unsigned carry=0;carry<2;carry++)for(unsigned sub=0;sub<2;sub++){
        unsigned got=z80_add8(&c,a,b,carry,sub);int math=sub?int(a)-int(b)-int(carry):int(a)+int(b)+int(carry);
        unsigned expected=unsigned(math)&255;
        int signed_a=a<128?int(a):int(a)-256,signed_b=b<128?int(b):int(b)-256;
        int signed_result=sub?signed_a-signed_b-int(carry):signed_a+signed_b+int(carry);
        unsigned f=(expected&0xa8)|(expected?0:64)|(math<0||math>255?1:0)|(sub?2:0)|
            (signed_result<-128||signed_result>127?4:0)|
            (sub?(int(a&15)-int(b&15)-int(carry)<0?16:0):((a&15)+(b&15)+carry>15?16:0));
        check(got==expected&&c.f==f,"Independent exhaustive 8-bit arithmetic reference mismatch");
    }
    auto bcd=[](unsigned n){return uint8_t((n/10)*16+n%10);};
    for(unsigned a=0;a<100;a++)for(unsigned b=0;b<100;b++)for(unsigned sub=0;sub<2;sub++){
        c.a=z80_add8(&c,bcd(a),bcd(b),0,sub);z80_daa(&c);
        unsigned dec=sub?(a+100-b)%100:(a+b)%100;
        check(c.a==bcd(dec)&&bool(c.f&1)==(sub?a<b:a+b>=100),"BCD correction mismatch");
    }
    for(unsigned prefix:{0u,0xcbu,0xedu,0xddu,0xfdu})for(unsigned opcode=0;opcode<256;opcode++){
        bus.memory.fill(0);unsigned at=0;if(prefix)bus.memory[at++]=prefix;bus.memory[at]=opcode;
        z80_reset(&c);c.hl=0x8000;c.sp=0xf000;
        auto instruction=disasm.decode(0);z80_decoded_t d{};
        check(z80_decode(&c,&d)&&d.length==instruction.length,"Interpreter/disassembler length drift");
        unsigned cycles=z80_step(&c);
        check(cycles==instruction.cycles||cycles==instruction.cycles_taken,"Interpreter/disassembler timing drift");
    }
    auto code=[&](std::initializer_list<uint8_t> bytes){bus.memory.fill(0);std::copy(bytes.begin(),bytes.end(),bus.memory.begin());z80_reset(&c);c.sp=0xf000;};
    code({0xdd,0x21,0x00,0x80,0xdd,0x36,0xfe,0x42,0xdd,0x66,0xfe});
    check(z80_step(&c)==14&&c.ix==0x8000,"LD IX failed");
    check(z80_step(&c)==19&&bus.memory[0x7ffe]==0x42,"Indexed immediate store order failed");
    check(z80_step(&c)==19&&c.h==0x42&&c.ix==0x8000,"Indexed H operand incorrectly became IXH");
    code({0xdd,0xcb,0x01,0x00});c.ix=0x8000;bus.memory[0x8001]=0x80;
    check(z80_step(&c)==23&&bus.memory[0x8001]==1&&c.b==1&&c.f==1&&c.r==2,"DDCB rotate/copy/refresh failed");
    code({0xdd,0xed,0xb0});c.hl=0x8000;c.de=0x9000;c.bc=2;bus.memory[0x8000]=0x42;bus.memory[0x8001]=0x24;
    check(z80_step(&c)==25&&c.pc==1&&c.bc==1,"Repeated block should drop ignored prefix");
    check(z80_step(&c)==16&&c.pc==3&&c.bc==0&&bus.memory[0x9000]==0x42&&bus.memory[0x9001]==0x24,"LDIR state failed");
    code({0xfb,0x00,0x76});c.irq=1;c.im=1;
    check(z80_step(&c)==4&&c.pc==1&&c.iff1,"EI failed");
    check(z80_step(&c)==4&&c.pc==2,"EI inhibition does not cover following instruction");
    check(z80_step(&c)==13&&c.pc==0x38&&!c.iff1&&bus.memory[0xeffe]==2,"IRQ acceptance/stack failed");
    code({0x76});check(z80_step(&c)==4&&c.pc==1&&c.halted,"HALT entry failed");
    check(z80_step(&c)==4&&c.pc==1&&c.r==2,"HALT cycles/refresh failed");
    c.iff1=1;c.nmi=1;bus.memory[0x66]=0xed;bus.memory[0x67]=0x45;
    check(z80_step(&c)==11&&c.pc==0x66&&!c.iff1&&c.iff2&&!c.halted,"NMI/HALT failed");
    check(z80_step(&c)==14&&c.pc==1&&c.iff1,"RETN restoration failed");
    code({0});c.i=0x80;c.im=2;c.iff1=c.iff2=c.irq=1;bus.memory[0x80ff]=0x34;bus.memory[0x8100]=0x12;
    check(z80_step(&c)==19&&c.pc==0x1234,"IM2 vector read failed");
    code({0x3e,0x12,0xd3,0x7f,0xdb,0xbe});z80_step(&c);z80_step(&c);
    check(bus.port==0x127f&&bus.value==0x12,"Immediate OUT lacks high A byte");z80_step(&c);
    check(bus.port==0x12be&&c.a==0xa5,"Immediate IN failed");
    code({0xff});c.pc=0xffff;bus.memory[0xffff]=0x18;bus.memory[0]=0xfe;
    check(z80_step(&c)==12&&c.pc==0xffff,"Relative PC wrap failed");
    bus.memory.fill(0xdd);z80_reset(&c);check(!z80_step(&c)&&c.fault,"Prefix-only memory should fault");
    std::cout<<"Z80 arithmetic, prefixes, timing, interrupts and bus: ok\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
