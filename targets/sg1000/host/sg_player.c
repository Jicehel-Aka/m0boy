/* Native SG-1000 player: same tested bus/CPU/VDP/PSG as generated projects. */
#include "sg1000.h"
#include <SDL.h>
#include "sg_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SG_EMBEDDED_ROM
#include "sg_embedded_rom.h"
#endif
static uint8_t cart_ram[SG_CART_RAM_SIZE];
static uint32_t pixels[SG_WIDTH*SG_HEIGHT];
static uint8_t sound[8192];
static unsigned head,tail;
static SDL_AudioDeviceID device;
static double speed_percent(unsigned frames,uint64_t elapsed,uint64_t frequency){
    return elapsed?(double)frames*59736.0/3579545.0*(double)frequency/(double)elapsed*100.0:0.0;
}
static void row(void *ctx,unsigned y,const uint8_t *p){
    (void)ctx;for(unsigned x=0;x<SG_WIDTH;x++)pixels[y*SG_WIDTH+x]=0xff000000u|sg1000_palette[p[x]&15];
}
static void audio_callback(void *ctx,Uint8 *stream,int length){
    (void)ctx;while(length--){
        if(tail==head)*stream++=128;
        else{*stream++=sound[tail];tail=(tail+1)&8191;}
    }
}
static void frame(void *ctx){
    (void)ctx;machine.stop=1;uint8_t buffer[512];unsigned n=sg1000_audio_available();
    sg1000_audio_render(buffer,n);
    if(!device)return;
    SDL_LockAudioDevice(device);
    for(unsigned i=0;i<n;i++){
        unsigned next=(head+1)&8191;if(next==tail)break;
        sound[head]=buffer[i];head=next;
    }
    SDL_UnlockAudioDevice(device);
}
static int screenshot(const char *path){
    FILE *f=fopen(path,"wb");if(!f)return 0;fprintf(f,"P6\n256 192\n255\n");
    for(unsigned i=0;i<SG_WIDTH*SG_HEIGHT;i++){
        uint8_t rgb[]={pixels[i]>>16,pixels[i]>>8,pixels[i]};
        if(fwrite(rgb,1,3,f)!=3){fclose(f);return 0;}
    }
    return fclose(f)==0;
}
static char *drop_cartridge(SDL_Window *window,SDL_Renderer *renderer){
    SDL_SetWindowTitle(window,"SG-1000 — déposez une cartouche .sg dans la fenêtre");
    SDL_RenderClear(renderer);SDL_RenderPresent(renderer);
    SDL_EventState(SDL_DROPFILE,SDL_ENABLE);
    SDL_Event event;
    while(SDL_WaitEvent(&event)){
        if(event.type==SDL_DROPFILE)return event.drop.file; /* SDL-owned allocation */
        if(event.type==SDL_QUIT||(event.type==SDL_KEYDOWN&&event.key.keysym.sym==SDLK_ESCAPE))return 0;
    }
    return 0;
}
static void usage(void){
    puts("Usage: sg1000_play [cartridge.sg] [--frames N] [--start-frame N] [--shot image.ppm] [--realtime]\n"
#ifdef SG_EMBEDDED_ROM
         "Without a cartridge argument, starts the bundled game.\n"
#else
         "Without a cartridge, drop a .sg file into the window.\n"
#endif
         "Arrows: move. Space/Enter: button 1 (A/Start). Ctrl: button 2 (B). *: Select/Pause. Escape: quit.");
}
int main(int argc,char **argv){
    const char *rom_path=0,*shot=0;unsigned limit=0,start=~0u;int realtime=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--help")||!strcmp(argv[i],"-h")){usage();return 0;}
        else if(!strcmp(argv[i],"--realtime"))realtime=1;
        else if(!strcmp(argv[i],"--frames")&&i+1<argc)limit=strtoul(argv[++i],0,0);
        else if(!strcmp(argv[i],"--start-frame")&&i+1<argc)start=strtoul(argv[++i],0,0);
        else if(!strcmp(argv[i],"--shot")&&i+1<argc)shot=argv[++i];
        else if(argv[i][0]!='-'&&!rom_path)rom_path=argv[i];
        else{fprintf(stderr,"Unknown argument: %s\n",argv[i]);return 1;}
    }
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_EVENTS)){fprintf(stderr,"SDL: %s\n",SDL_GetError());return 1;}
    SDL_Window *window=SDL_CreateWindow("SG-1000 — flèches, Espace/Ctrl, Entrée, *",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,768,576,SDL_WINDOW_RESIZABLE);
    SDL_Renderer *renderer=window?SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED):0;
    if(window&&!renderer)renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
    SDL_Texture *texture=renderer?SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,SG_WIDTH,SG_HEIGHT):0;
    if(!texture){fprintf(stderr,"Display: %s\n",SDL_GetError());if(renderer)SDL_DestroyRenderer(renderer);if(window)SDL_DestroyWindow(window);SDL_Quit();return 1;}
    SDL_RenderSetLogicalSize(renderer,SG_WIDTH,SG_HEIGHT);
    uint8_t *rom=0;char *dropped=0;int status=1;
    SDL_RWops *file=0;
#ifdef SG_EMBEDDED_ROM
    if(!rom_path)file=SDL_RWFromConstMem(sg_embedded_rom,sizeof(sg_embedded_rom));
#endif
    if(!rom_path&&!file){
        dropped=drop_cartridge(window,renderer);
        if(!dropped){status=0;goto cleanup;}
        rom_path=dropped;
    }
    SDL_SetWindowTitle(window,"SG-1000 — flèches, Espace/Ctrl, Entrée, *");
    /* SDL handles UTF-8 paths on Windows as well as Linux. */
    if(!file)file=SDL_RWFromFile(rom_path,"rb");
    if(!file){fprintf(stderr,"Cartridge: %s\n",SDL_GetError());goto cleanup;}
    Sint64 length=SDL_RWsize(file);
    if(length<=0||length>0xc000){SDL_RWclose(file);fprintf(stderr,"Unsupported cartridge size\n");goto cleanup;}
    size_t size=(size_t)length;rom=malloc(size);
    if(!rom||SDL_RWread(file,rom,1,size)!=size){SDL_RWclose(file);goto cleanup;}
    SDL_RWclose(file);
    SDL_AudioSpec desired={0};desired.freq=22050;desired.format=AUDIO_U8;desired.channels=1;desired.samples=512;desired.callback=audio_callback;
    device=SDL_OpenAudioDevice(0,0,&desired,0,0);
    if(!device)fprintf(stderr,"Audio: %s\n",SDL_GetError());
    sg1000_init(rom,size,cart_ram);machine.ref_row=row;machine.frame=frame;sg1000_audio_rate(22050);
    if(device)SDL_PauseAudioDevice(device,0);
    uint64_t frequency=SDL_GetPerformanceFrequency(),deadline=SDL_GetPerformanceCounter(),fraction=0;
    uint64_t run_start=deadline,stats_start=deadline,active_ticks=0,stats_active=0;
    unsigned stats_frames=0;
    int quit=0;sg_input_t input={0};
    while(!quit&&!machine.cpu.fault&&(!limit||machine.frames<limit)){
        SDL_Event event;while(SDL_PollEvent(&event)){
            if(event.type==SDL_QUIT||(event.type==SDL_KEYDOWN&&event.key.keysym.sym==SDLK_ESCAPE))quit=1;
            sg_input_event(&input,&event);
        }
        if(quit)break;
        const Uint8 *keys=SDL_GetKeyboardState(0);
        uint8_t buttons=sg_input_buttons(keys);
        if(machine.frames>=start&&machine.frames-start<10)buttons|=16;
        sg1000_buttons(0,buttons);sg1000_pause(sg_input_pause(&input,keys));
        uint64_t active_start=SDL_GetPerformanceCounter();
        machine.stop=0;sg1000_run();
        /* Capacity measures the core and sound queue, excluding presentation
         * and pacing. Current speed includes the entire real-time loop. */
        uint64_t active=SDL_GetPerformanceCounter()-active_start;
        active_ticks+=active;stats_active+=active;stats_frames++;
        SDL_UpdateTexture(texture,0,pixels,SG_WIDTH*sizeof(*pixels));SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,0,0);SDL_RenderPresent(renderer);
        if(!limit||realtime){
            fraction+=frequency*59736u;deadline+=fraction/3579545u;fraction%=3579545u;
            uint64_t now=SDL_GetPerformanceCounter();
            while(now<deadline){uint64_t ms=(deadline-now)*1000/frequency;SDL_Delay(ms>1?(Uint32)(ms-1):1);now=SDL_GetPerformanceCounter();}
            if(now-deadline>frequency/4)deadline=now;
        }
        uint64_t stats_now=SDL_GetPerformanceCounter();
        if(stats_now-stats_start>=frequency/2){
            char title[256];
            snprintf(title,sizeof(title),"SG-1000 — actuel %.0f %% | possible %.0f %% — Espace/Ctrl, Entrée, *",
                speed_percent(stats_frames,stats_now-stats_start,frequency),
                speed_percent(stats_frames,stats_active,frequency));
            SDL_SetWindowTitle(window,title);
            stats_start=stats_now;stats_frames=0;stats_active=0;
        }
    }
    uint64_t elapsed=SDL_GetPerformanceCounter()-run_start;
    printf("speed_current=%.1f%% speed_possible=%.1f%%\n",
        speed_percent(machine.frames,elapsed,frequency),speed_percent(machine.frames,active_ticks,frequency));
    status=machine.cpu.fault?1:0;
    if(shot&&!screenshot(shot)){perror(shot);status=1;}
    printf("frames=%u cycles=%u pc=%04x\n",machine.frames,machine.cyc,machine.cpu.pc);
cleanup:
    if(device)SDL_CloseAudioDevice(device);
    SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);
    SDL_free(dropped);SDL_Quit();free(rom);return status;
}
