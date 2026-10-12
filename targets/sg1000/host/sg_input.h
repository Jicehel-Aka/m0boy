#ifndef SG_INPUT_H
#define SG_INPUT_H
#include <SDL.h>
typedef struct { int select_pressed; SDL_Scancode select_key; } sg_input_t;
static inline void sg_input_event(sg_input_t *input,const SDL_Event *event){
    if(event->type==SDL_KEYDOWN&&(event->key.keysym.sym==SDLK_ASTERISK||
       (event->key.keysym.sym==SDLK_8&&(event->key.keysym.mod&KMOD_SHIFT)))){
        input->select_pressed=1;input->select_key=event->key.keysym.scancode;
    }
    if(event->type==SDL_KEYUP&&event->key.keysym.scancode==input->select_key)input->select_pressed=0;
    if(event->type==SDL_WINDOWEVENT&&event->window.event==SDL_WINDOWEVENT_FOCUS_LOST)input->select_pressed=0;
}
static inline uint8_t sg_input_buttons(const Uint8 *keys){
    /* SG-1000 has two controller buttons. Games use button 1 to start. */
    return (keys[SDL_SCANCODE_UP]?1:0)|(keys[SDL_SCANCODE_DOWN]?2:0)|
        (keys[SDL_SCANCODE_LEFT]?4:0)|(keys[SDL_SCANCODE_RIGHT]?8:0)|
        ((keys[SDL_SCANCODE_SPACE]||keys[SDL_SCANCODE_RETURN]||keys[SDL_SCANCODE_KP_ENTER])?16:0)|
        ((keys[SDL_SCANCODE_LCTRL]||keys[SDL_SCANCODE_RCTRL])?32:0);
}
static inline int sg_input_pause(const sg_input_t *input,const Uint8 *keys){
    return input->select_pressed||keys[SDL_SCANCODE_KP_MULTIPLY];
}
#endif
