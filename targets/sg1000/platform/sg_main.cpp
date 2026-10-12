#include "sg1000.h"
#include "meta_target.h"
#include "meta_clock.h"
#include "meta_audio.h"
#include "meta_lcd.h"
#if META_TARGET_POKITTO
#include "pokitto_hw.h"
#else
#include "meta_hw.h"
#endif
extern "C" {
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
unsigned sg_compiled(z80_t *);
#if META_TARGET_POKITTO
void pokitto_sd_clock_changed(uint32_t) {}
#endif
}
extern "C" { sg1000_t machine; }
static uint16_t line_buffers[2][160];
#if !META_TARGET_POKITTO
static uint32_t line_sequences[2];
#endif
static unsigned line_slot;
static uint32_t deadline;
static int skip_video;
static const char version[] __attribute__((used))="m0boy-sg1000-" META_TARGET_NAME " " SG_GEN_VERSION;
/* Exact nearest-neighbour 256 -> 160 mapping without software division. */
static const uint8_t sample_x[160]={0,1,3,4,6,8,9,11,12,14,16,17,19,20,22,24,25,27,28,30,32,33,35,36,38,40,41,43,44,46,48,49,51,52,54,56,57,59,60,62,64,65,67,68,70,72,73,75,76,78,80,81,83,84,86,88,89,91,92,94,96,97,99,100,102,104,105,107,108,110,112,113,115,116,118,120,121,123,124,126,128,129,131,132,134,136,137,139,140,142,144,145,147,148,150,152,153,155,156,158,160,161,163,164,166,168,169,171,172,174,176,177,179,180,182,184,185,187,188,190,192,193,195,196,198,200,201,203,204,206,208,209,211,212,214,216,217,219,220,222,224,225,227,228,230,232,233,235,236,238,240,241,243,244,246,248,249,251,252,254};
static const uint16_t colors[16]={META_COLOR(0x000000),META_COLOR(0x000000),META_COLOR(0x21c842),META_COLOR(0x5edc78),META_COLOR(0x5455ed),META_COLOR(0x7d76fc),META_COLOR(0xd4524d),META_COLOR(0x42ebf5),META_COLOR(0xfc5554),META_COLOR(0xff7978),META_COLOR(0xd4c154),META_COLOR(0xe6ce80),META_COLOR(0x21b03b),META_COLOR(0xc95bba),META_COLOR(0xcccccc),META_COLOR(0xffffff)};
static unsigned display_row(unsigned y){
#if META_TARGET_POKITTO
    return (y*25)>>5; /* 192 -> 150 */
#else
    return (y*5)>>3; /* 192 -> 120 */
#endif
}
static int draw_line(void *,unsigned y){
    return !skip_video&&(!y||display_row(y)!=display_row(y-1));
}
static void row(void *,unsigned y,const uint8_t *pixels){
    unsigned dest=display_row(y);
#if !META_TARGET_POKITTO
    while((int32_t)(meta_lcd_done_seq()-line_sequences[line_slot])<0){}
#endif
    uint16_t *line_buffer=line_buffers[line_slot];
    for(unsigned x=0;x<160;x++)line_buffer[x]=colors[pixels[sample_x[x]]&15];
#if META_TARGET_POKITTO
    pokitto_lcd_row_zoom(dest+13,line_buffer);
#else
    line_sequences[line_slot]=meta_lcd_send_rows(line_buffer,(uint8_t)(dest+4),1);
    line_slot^=1;
#endif
}
static void frame(void *){
    meta_audio_frame();meta_lcd_end_frame();uint8_t keys=0,pause=0;
#if META_TARGET_POKITTO
    uint8_t raw=pokitto_buttons();
    keys=((raw&POKITTO_UP)?1:0)|((raw&POKITTO_DOWN)?2:0)|((raw&POKITTO_LEFT)?4:0)|
        ((raw&POKITTO_RIGHT)?8:0)|((raw&POKITTO_A)?16:0)|((raw&POKITTO_B)?32:0);
    pause=raw&POKITTO_C;
#else
    meta_spi_rate(12000000u);
    for(unsigned i=0;i<2&&(SERCOM4->SPI.INTFLAG.reg&SERCOM_SPI_INTFLAG_RXC);i++)(void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg=SERCOM_SPI_STATUS_BUFOVF;
    PORT->Group[1].OUTCLR.reg=1u<<3;
    uint8_t raw=(uint8_t)~meta_spi_transfer(1);
    PORT->Group[1].OUTSET.reg=1u<<3;
    keys=((raw&8)?1:0)|((raw&1)?2:0)|((raw&2)?4:0)|((raw&4)?8:0)|(raw&48);
    pause=raw&64;
    if(WDT->CTRL.reg&WDT_CTRL_ENABLE){WDT->CLEAR.reg=WDT_CLEAR_CLEAR_KEY;}
#endif
    sg1000_buttons(&machine,0,keys);sg1000_pause(&machine,pause);
    /* NTSC baseline: 59736 T-states at 3.579545 MHz; coarse frame pacing.
     * Do not wait when the core already runs behind the deadline. */
    deadline+=16688u;
    /* Drop at most one presentation when late. CPU, PSG and sprite status
     * continue at full precision so a costly LCD frame need not slow play. */
    skip_video=!skip_video&&(int32_t)(meta_micros()-deadline)>0;
    while((int32_t)(meta_micros()-deadline)<0){}
    if((int32_t)(meta_micros()-deadline)>33376)deadline=meta_micros();
}
int main(){
#if META_TARGET_POKITTO
    pokitto_buttons_init();
#else
    meta_spi_init();
    PORT->Group[1].DIRSET.reg=1u<<3;PORT->Group[1].OUTSET.reg=1u<<3;
#endif
    meta_clock_init();meta_lcd_init();meta_lcd_fill(0);
    sg1000_init(&machine,sg_rom,sg_rom_size);machine.compiled=sg_compiled;
    machine.draw_line=draw_line;machine.row=row;machine.frame=frame;meta_audio_init();deadline=meta_micros();
    /* Keep an inspectable source/target version stamp in the firmware. */
    __asm__ volatile("" : : "r"(version));
    for(;;){if(!sg1000_step(&machine))for(;;){}}
}
