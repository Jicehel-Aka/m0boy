#include "sg_input.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"Failed at line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(int argc,char **argv){
    (void)argc;(void)argv;
    Uint8 keys[SDL_NUM_SCANCODES]={0};sg_input_t input={0};SDL_Event event={0};
    SDL_Scancode start[]={SDL_SCANCODE_RETURN,SDL_SCANCODE_KP_ENTER,SDL_SCANCODE_SPACE};
    for(unsigned i=0;i<sizeof(start)/sizeof(*start);i++){
        keys[start[i]]=1;CHECK(sg_input_buttons(keys)==16);CHECK(!sg_input_pause(&input,keys));keys[start[i]]=0;
    }
    keys[SDL_SCANCODE_LCTRL]=1;CHECK(sg_input_buttons(keys)==32);
    keys[SDL_SCANCODE_LCTRL]=0;keys[SDL_SCANCODE_RCTRL]=1;CHECK(sg_input_buttons(keys)==32);
    keys[SDL_SCANCODE_SPACE]=1;keys[SDL_SCANCODE_LEFT]=1;CHECK(sg_input_buttons(keys)==52);
    memset(keys,0,sizeof(keys));keys[SDL_SCANCODE_KP_MULTIPLY]=1;
    CHECK(sg_input_pause(&input,keys));CHECK(!sg_input_buttons(keys));keys[SDL_SCANCODE_KP_MULTIPLY]=0;
    event.type=SDL_KEYDOWN;event.key.keysym.sym=SDLK_ASTERISK;event.key.keysym.scancode=SDL_SCANCODE_BACKSLASH;
    sg_input_event(&input,&event);CHECK(sg_input_pause(&input,keys));
    /* Keyup can carry a different symbol after Shift is released. */
    event.type=SDL_KEYUP;event.key.keysym.sym=SDLK_BACKSLASH;
    sg_input_event(&input,&event);CHECK(!sg_input_pause(&input,keys));
    event.type=SDL_KEYDOWN;event.key.keysym.sym=SDLK_ASTERISK;sg_input_event(&input,&event);
    event.type=SDL_WINDOWEVENT;event.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
    sg_input_event(&input,&event);CHECK(!sg_input_pause(&input,keys));
    event.type=SDL_KEYDOWN;event.key.keysym.sym=SDLK_8;event.key.keysym.scancode=SDL_SCANCODE_8;event.key.keysym.mod=KMOD_SHIFT;
    sg_input_event(&input,&event);CHECK(sg_input_pause(&input,keys));
    event.type=SDL_KEYUP;event.key.keysym.mod=0;
    sg_input_event(&input,&event);CHECK(!sg_input_pause(&input,keys));
    return 0;
}
