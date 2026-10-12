#include "sg1000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef SG_GENERATED
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
extern unsigned sg_compiled(z80_t *);
#endif
static sg1000_t machine;
static uint8_t image[SG_HEIGHT][SG_WIDTH];
static FILE *coverage,*audio;
static uint32_t audio_samples;
static int audio_error;
static void le32(FILE *f,uint32_t v){for(unsigned i=0;i<4;i++)fputc((v>>(8*i))&255,f);}
static void wav_header(FILE *f,uint32_t n){
    fwrite("RIFF",1,4,f);le32(f,36+n);fwrite("WAVEfmt ",1,8,f);le32(f,16);
    fputc(1,f);fputc(0,f);fputc(1,f);fputc(0,f);le32(f,22050);le32(f,22050);
    fputc(1,f);fputc(0,f);fputc(8,f);fputc(0,f);fwrite("data",1,4,f);le32(f,n);
}
static void frame_audio(void *ctx){
    (void)ctx;uint8_t buffer[512];
    unsigned n=(machine.audio_head-machine.audio_tail)&511;
    sg1000_audio_render(&machine,buffer,n);
    if(fwrite(buffer,1,n,audio)!=n)audio_error=1;
    audio_samples+=n;
}
static uint32_t hits[65536];
static uint16_t coverage_order[65536];
static int hot_first(const void *a,const void *b){
    unsigned x=*(const uint16_t *)a,y=*(const uint16_t *)b;
    if(hits[x]!=hits[y])return hits[x]>hits[y]?-1:1;
    return x<y?-1:x>y;
}
static void row(void *ctx,unsigned y,const uint8_t *pixels){(void)ctx;memcpy(image[y],pixels,SG_WIDTH);}
static uint32_t hash_bytes(uint32_t h,const void *data,size_t n){
    const uint8_t *p=data;while(n--)h=(h^*p++)*16777619u;return h;
}
static uint32_t run_hash(sg1000_t *s){
    uint32_t h=2166136261u;
    uint16_t registers[]={s->cpu.bc,s->cpu.de,s->cpu.hl,s->cpu.bc2,s->cpu.de2,s->cpu.hl2,s->cpu.af2,
        s->cpu.ix,s->cpu.iy,s->cpu.sp,s->cpu.pc};
    for(unsigned i=0;i<sizeof(registers)/sizeof(*registers);i++){uint8_t bytes[]={registers[i]&255,registers[i]>>8};h=hash_bytes(h,bytes,2);}
    uint8_t flags[]={s->cpu.a,s->cpu.f,s->cpu.i,s->cpu.r,s->cpu.im,s->cpu.iff1,s->cpu.iff2,s->cpu.halted,s->cpu.ei_delay,
        s->vdp_status,s->vdp_latch,s->vdp_pending,s->vdp_buffer,s->cpu.irq,s->cpu.nmi,s->cpu.fault,s->pause};
    h=hash_bytes(h,flags,sizeof(flags));
    uint64_t counters[]={s->cpu.cycles,s->frames,s->line,s->line_cycles,s->vdp_address};
    for(unsigned i=0;i<sizeof(counters)/sizeof(*counters);i++)for(unsigned b=0;b<8;b++){uint8_t v=counters[i]>>(b*8);h=hash_bytes(h,&v,1);}
    h=hash_bytes(h,s->ram,sizeof(s->ram));h=hash_bytes(h,s->vram,sizeof(s->vram));h=hash_bytes(h,s->vdp,sizeof(s->vdp));
    h=hash_bytes(h,s->buttons,sizeof(s->buttons));h=hash_bytes(h,s->psg_tone,sizeof(s->psg_tone));
    h=hash_bytes(h,s->psg_volume,sizeof(s->psg_volume));h=hash_bytes(h,&s->psg_noise,1);h=hash_bytes(h,&s->psg_latch,1);
    return hash_bytes(h,image,sizeof(image));
}
int main(int argc,char **argv){
    const char *rom_path=0,*shot=0,*cov_path=0,*wav_path=0;unsigned frames=300,buttons=0,start_frame=~0u,pause_frame=~0u;
    int interp=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--interp"))interp=1;
        else if(!strcmp(argv[i],"--frames")&&i+1<argc)frames=(unsigned)strtoul(argv[++i],0,0);
        else if(!strcmp(argv[i],"--shot")&&i+1<argc)shot=argv[++i];
        else if(!strcmp(argv[i],"--wav")&&i+1<argc)wav_path=argv[++i];
        else if(!strcmp(argv[i],"--coverage")&&i+1<argc)cov_path=argv[++i];
        else if(!strcmp(argv[i],"--buttons")&&i+1<argc)buttons=(unsigned)strtoul(argv[++i],0,0);
        else if(!strcmp(argv[i],"--start-frame")&&i+1<argc)start_frame=(unsigned)strtoul(argv[++i],0,0);
        else if(!strcmp(argv[i],"--pause-frame")&&i+1<argc)pause_frame=(unsigned)strtoul(argv[++i],0,0);
        else if(argv[i][0]!='-'&&!rom_path)rom_path=argv[i];
        else{fprintf(stderr,"Unknown or incomplete argument: %s\n",argv[i]);return 1;}
    }
    uint8_t *loaded=0;const uint8_t *rom;size_t size;
#ifdef SG_GENERATED
    rom=sg_rom;size=sg_rom_size;
    if(rom_path){fprintf(stderr,"Generated harness already contains its cartridge\n");return 1;}
#else
    (void)interp;
    if(!rom_path){fprintf(stderr,"Usage: sg1000_host cartridge.sg [--frames N] [--shot file.ppm] [--coverage file]\n");return 1;}
    FILE *file=fopen(rom_path,"rb");if(!file){perror(rom_path);return 1;}
    if(fseek(file,0,SEEK_END)||ftell(file)<=0||ftell(file)>0xc000){fclose(file);fprintf(stderr,"Unsupported cartridge size\n");return 1;}
    size=(size_t)ftell(file);rewind(file);loaded=malloc(size);
    if(!loaded||fread(loaded,1,size,file)!=size){fclose(file);free(loaded);return 1;}
    fclose(file);rom=loaded;
#endif
    if(cov_path){coverage=fopen(cov_path,"w");if(!coverage){perror(cov_path);free(loaded);return 1;}fprintf(coverage,"# m0boy.sg1000.coverage.v1\n");}
    sg1000_init(&machine,rom,size);machine.row=row;
    if(wav_path){
        audio=fopen(wav_path,"wb");if(!audio){perror(wav_path);if(coverage)fclose(coverage);free(loaded);return 1;}
        wav_header(audio,0);sg1000_audio_rate(&machine,22050);machine.frame=frame_audio;
    }
#ifdef SG_GENERATED
    if(!interp)machine.compiled=sg_compiled;
#endif
    while(machine.frames<frames&&!machine.cpu.fault){
        unsigned keys=buttons;
        if(machine.frames>=start_frame&&machine.frames<start_frame+10)keys|=16;
        sg1000_buttons(&machine,0,(uint8_t)keys);
        sg1000_pause(&machine,machine.frames==pause_frame);
        unsigned pc=machine.cpu.pc;
        if(coverage)hits[pc]++;
        if(!sg1000_step(&machine))break;
    }
    if(audio){
        /* RIFF chunks are padded to an even byte count. */
        if(audio_samples&1)fputc(0,audio);
        if(fseek(audio,0,SEEK_SET))audio_error=1;
        wav_header(audio,audio_samples);
        if(audio_samples&1){fseek(audio,4,SEEK_SET);le32(audio,36+audio_samples+1);}
        if(ferror(audio))audio_error=1;
        if(fclose(audio))audio_error=1;
        if(audio_error){perror("audio");if(coverage)fclose(coverage);free(loaded);return 1;}
    }
    if(coverage){
        unsigned n=0;for(unsigned pc=0;pc<65536;pc++)if(hits[pc])coverage_order[n++]=(uint16_t)pc;
        qsort(coverage_order,n,sizeof(*coverage_order),hot_first);
        for(unsigned i=0;i<n;i++)if(fprintf(coverage,"%04x\n",coverage_order[i])<0){fclose(coverage);free(loaded);return 1;}
    }
    if(coverage&&fclose(coverage)){perror("coverage");free(loaded);return 1;}
    if(shot){FILE *file=fopen(shot,"wb");if(!file){perror(shot);free(loaded);return 1;}
        fprintf(file,"P6\n256 192\n255\n");
        for(unsigned y=0;y<SG_HEIGHT;y++)for(unsigned x=0;x<SG_WIDTH;x++){
            uint32_t rgb=sg1000_palette[image[y][x]&15];uint8_t p[]={rgb>>16,rgb>>8,rgb};
            if(fwrite(p,1,3,file)!=3){fclose(file);free(loaded);return 1;}}
        if(fclose(file)){free(loaded);return 1;}}
    printf("frames=%u cycles=%llu pc=%04x compiled=%u interpreted=%u run_hash=%08x\n",machine.frames,
        (unsigned long long)machine.cpu.cycles,machine.cpu.pc,machine.compiled_steps,machine.interpreted_steps,run_hash(&machine));
    free(loaded);return machine.cpu.fault?1:0;
}
