#include "sg1000.h"
#include <stdio.h>
#include <string.h>
static sg1000_t s;
#define REQUIRE(v) do{if(!(v)){fprintf(stderr,"line %d: %s\n",__LINE__,#v);return 1;}}while(0)
static void reg(unsigned n,uint8_t value){sg1000_out(&s,0xbf,value);sg1000_out(&s,0xbf,0x80|n);}
static void address(unsigned a,unsigned write){sg1000_out(&s,0xbf,a);sg1000_out(&s,0xbf,(a>>8)|(write?0x40:0));}
int main(void){
    static uint8_t rom[16384];rom[0]=0x76;rom[1]=0x42;sg1000_init(&s,rom,sizeof(rom));
    REQUIRE(sg1000_read(&s,0x4001)==0x42);sg1000_write(&s,0xc012,0x34);REQUIRE(sg1000_read(&s,0xfc12)==0x34);
    sg1000_write(&s,0,0);REQUIRE(sg1000_read(&s,0)==0x76);
    address(0x3fff,1);sg1000_out(&s,0xbe,0x12);sg1000_out(&s,0xbe,0x34);REQUIRE(s.vram[0x3fff]==0x12&&s.vram[0]==0x34);
    address(0x3fff,0);REQUIRE(sg1000_in(&s,0xbe)==0x12&&sg1000_in(&s,0xbe)==0x34);
    sg1000_out(&s,0xbf,0);REQUIRE(s.vdp_pending);(void)sg1000_in(&s,0xbf);REQUIRE(!s.vdp_pending);
    sg1000_buttons(&s,0,0x11);sg1000_buttons(&s,1,0x25);REQUIRE(sg1000_in(&s,0xdc)==0xae&&sg1000_in(&s,0xdd)==0xf6);
    sg1000_pause(&s,1);REQUIRE(s.cpu.nmi);s.cpu.nmi=0;sg1000_pause(&s,1);REQUIRE(!s.cpu.nmi);
    sg1000_pause(&s,0);sg1000_pause(&s,1);REQUIRE(s.cpu.nmi);
    sg1000_reset(&s);reg(1,0x60);
    while(s.line<192)REQUIRE(sg1000_step(&s));REQUIRE(s.cpu.irq&&(s.vdp_status&128));
    REQUIRE(sg1000_in(&s,0xbf)&128);REQUIRE(!s.cpu.irq&&!(s.vdp_status&128));
    s.vdp_status|=128;reg(1,0x40);REQUIRE(!s.cpu.irq);reg(1,0x60);REQUIRE(s.cpu.irq);
    sg1000_reset(&s);reg(1,0x40);reg(2,6);reg(3,0x80);reg(4,0);reg(5,0x36);reg(7,1);
    s.vram[0]=0x80;s.vram[0x2000]=0xf2;s.vram[0x1b00]=208;
    uint8_t pixels[256];sg1000_render_line(&s,0,pixels);REQUIRE(pixels[0]==15&&pixels[1]==2);
    /* Two opaque sprites overlap at the left edge. Lower index wins. */
    s.vram[0x1b00]=255;s.vram[0x1b01]=0;s.vram[0x1b02]=1;s.vram[0x1b03]=3;
    s.vram[0x1b04]=255;s.vram[0x1b05]=0;s.vram[0x1b06]=1;s.vram[0x1b07]=4;s.vram[0x1b08]=208;s.vram[8]=128;
    sg1000_render_line(&s,0,pixels);REQUIRE(pixels[0]==3&&(s.vdp_status&32));
    /* Omitting background pixels must preserve collision/overflow status. */
    uint8_t status=s.vdp_status;s.vdp_status=31;sg1000_render_line(&s,0,pixels);
    uint8_t raster_status=s.vdp_status;s.vdp_status=31;sg1000_render_line(&s,0,0);
    REQUIRE(s.vdp_status==raster_status);s.vdp_status=status;
    /* HALT batching must stop at raster events and retain refresh timing. */
    sg1000_reset(&s);REQUIRE(sg1000_step(&s)==4);
    while(s.line<100)REQUIRE(sg1000_step(&s));
    REQUIRE(s.cpu.cycles==22800&&s.line_cycles==0&&s.cpu.r==((22800/4)&127));
    s.cpu.nmi=1;REQUIRE(sg1000_step(&s)==11&&s.cpu.pc==0x66);
    /* Samples follow CPU time, not the host/firmware pacing clock. */
    sg1000_reset(&s);sg1000_audio_rate(&s,22050);
    uint8_t audio[512];unsigned total=0,edges=0;uint8_t previous=128;
    sg1000_out(&s,0x7f,0x8e);sg1000_out(&s,0x7f,15);sg1000_out(&s,0x7f,0x90);
    for(unsigned frame=0;frame<60;frame++){
        while(s.frames<=frame)REQUIRE(sg1000_step(&s));
        unsigned n=(s.audio_head-s.audio_tail)&511;total+=n;
        sg1000_audio_render(&s,audio,n);
        for(unsigned i=0;i<n;i++){if(previous<128&&audio[i]>128)edges++;previous=audio[i];}
    }
    REQUIRE(total==(uint64_t)s.cpu.cycles*22050/3579545);
    REQUIRE(edges>=437&&edges<=442); /* ~440 Hz for one NTSC second */
    sg1000_out(&s,0x7f,0x9f);
    while(s.frames==60)REQUIRE(sg1000_step(&s));
    unsigned n=(s.audio_head-s.audio_tail)&511;sg1000_audio_render(&s,audio,n);
    for(unsigned i=0;i<n;i++)REQUIRE(audio[i]==128);
    /* A mid-frame volume write must not retroactively mute older samples. */
    sg1000_reset(&s);sg1000_audio_rate(&s,22050);
    sg1000_out(&s,0x7f,0x8e);sg1000_out(&s,0x7f,15);sg1000_out(&s,0x7f,0x90);
    while(s.line<100)REQUIRE(sg1000_step(&s));
    unsigned audible=(s.audio_head-s.audio_tail)&511;sg1000_out(&s,0x7f,0x9f);
    while(!s.frames)REQUIRE(sg1000_step(&s));
    n=(s.audio_head-s.audio_tail)&511;sg1000_audio_render(&s,audio,n);
    REQUIRE(audible>0&&n>audible);
    for(unsigned i=0;i<audible;i++)REQUIRE(audio[i]!=128);
    for(unsigned i=audible;i<n;i++)REQUIRE(audio[i]==128);
    /* Noise reset is deterministic and periodic noise repeats after 15
     * shifts on the TI part, including the delayed output stage. */
    sg1000_reset(&s);sg1000_audio_rate(&s,22050);
    sg1000_out(&s,0x7f,0xe4);sg1000_out(&s,0x7f,0xf0);
    while(!s.frames)REQUIRE(sg1000_step(&s));
    n=(s.audio_head-s.audio_tail)&511;sg1000_audio_render(&s,audio,n);
    unsigned low=0,high=0;for(unsigned i=0;i<n;i++){low+=audio[i]<128;high+=audio[i]>128;}
    REQUIRE(low&&high);sg1000_out(&s,0x7f,0xe0);REQUIRE(s.audio_lfsr==0x10000);
    sg1000_reset(&s);REQUIRE(s.audio_head==0&&s.audio_tail==0);
    sg1000_audio_render(&s,audio,512);for(unsigned i=0;i<512;i++)REQUIRE(audio[i]==128);
    puts("SG-1000 bus, VDP ports, IRQ, input and raster: ok");return 0;
}
