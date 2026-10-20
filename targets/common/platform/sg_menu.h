/* The option menu of the Sega and PICO-8 frontends (sg_main.cpp, gg_main.cpp,
 * p8_main.cpp), the Game Boy's (meta_main.cpp) adapted: HOME (Pokitto: C)
 * held half a second alone opens it, MENU keeps its game role (Start, Pause,
 * the cartridge's restart).  The lines differ per console; what they share is
 * here: the HOME hold, the buttons of the menu, the info line (frames of the
 * last second and the stack never used) and the shared lines (Reprendre,
 * Redemarrer, Volume, Vitesse, Processeur, Quitter, plus the save states
 * when the frontend defines SG_MENU_STATE_SLOTS and the USB bootloader when
 * it defines SG_MENU_BOOTLOADER).
 *
 * Each frontend defines before including:
 *   SG_MENU_TAP      statements for a HOME tap (the game's key, whose hold
 *                    must then not be the game's key: the Pokitto's C)
 *   SG_MENU_RESTART  statements restarting the cartridge
 *   SG_MENU_QUIT     statements handing the console back to its loader
 *   SG_MENU_BOOTLOADER()  (META) statements entering the USB bootloader: the
 *                    sam-ba magic at 0x20007FFC, then a software reset —
 *                    works without META_USB, where the double reset would
 *                    reach the loader instead
 *   SG_MENU_RESUME   statements once the menu is closed (the pacing again)
 * optional:
 *   SG_MENU_ON_STACK_TOP
 *                    an expression: run the menu on the stack ending there
 *                    instead of the C stack (the Game Gear's lent card
 *                    cache; the menu needs about 600 bytes and the
 *                    dispatcher calls frame() deep in the game's calls),
 *                    with SG_MENU_UNLEND() statements giving the blocks
 *                    back once it has run
 *   SG_MENU_TITLE    the menu's title (default "Options")
 *   SG_MENU_STATE_SLOTS, SG_MENU_STATE_BUF(), SG_MENU_STATE_LOADED()
 *                    save states: the slot count (when the console has a
 *                    state file on the card), 512 bytes of scratch for the
 *                    card sectors, and what the frame loop re-synchronises
 *                    after a load
 * and implements meta_menu_scratch (meta_menu.h) over its own buffers.
 */
#ifndef SG_MENU_H
#define SG_MENU_H

#include "meta_menu.h"
#include "meta_audio.h"
#include "meta_clock.h"

#include <string.h>

#ifndef SG_MENU_TITLE
#define SG_MENU_TITLE "Options"
#endif

static const char *const sg_volume_names[9] = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const sg_speed_names[2] = {"Normale", "Rapide"};
#if META_TARGET_ESPBOY
static const char *const sg_clock_names[2] = {"80 MHz", "160 MHz"};
#elif META_TARGET_POKITTO
static const char *const sg_clock_names[2] = {"48 MHz", "72 MHz"};
#else
static const char *const sg_clock_names[2] = {"48 MHz", "70 MHz"}; /* meta_clock.cpp: 72 asked, 70 run */
#endif

#ifdef SG_MENU_STATE_SLOTS
#include "meta_state.h"
/* The slots' names, the used ones without " vide". */
static char sg_slot_text[SG_MENU_STATE_SLOTS][12];
static const char *sg_slot_names[SG_MENU_STATE_SLOTS];
static const char *sg_state_msg; /* the last save state operation's result */
static uint8_t sg_state_slot;
static void sg_name_slots(void) {
    uint16_t used = meta_state_used();
    for (unsigned i = 0; i < SG_MENU_STATE_SLOTS; i++) {
        char *p = sg_put_uint(sg_slot_text[i], i + 1u);
        if (!(used >> i & 1u)) memcpy(p, " vide", 6);
        else *p = 0;
        sg_slot_names[i] = sg_slot_text[i];
    }
}
#endif

static char *sg_put_uint(char *p, unsigned v) {
    char tmp[10];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) && n < 10);
    while (n) *p++ = tmp[--n];
    return p;
}

/* Frames of the last second, for the info line (sg_menu_frame per frame). */
static uint32_t sg_stat_ms;
static uint16_t sg_frames, sg_ips;

static void sg_menu_frame(void) {
    sg_frames++;
    uint32_t ms = millis();
    if (ms - sg_stat_ms >= 1000) {
        sg_ips = sg_frames;
        sg_frames = 0;
        sg_stat_ms = ms;
    }
}

#if !META_TARGET_ESPBOY
/* Free RAM painted at boot (sg_frontend.h), as the Game Boy's menu shows it. */
static unsigned sg_stack_never_used(void) {
    const uint32_t *p = (const uint32_t *)(((uintptr_t)&end + 3u) & ~(uintptr_t)3u);
    unsigned n = 0;
    while (p[n] == SG_STACK_PAINT_WORD) n++;
    return n * 4u;
}
#endif

/* HOME held half a second (alone) asks for the menu; any other key while it
 * is held cancels it (HOME stays exclusive).  On release without the hold,
 * SG_MENU_TAP (the game's key on the Pokitto, whose C is HOME). */
static uint8_t sg_menu_was_down, sg_menu_stage, sg_menu_request;
static uint32_t sg_menu_since;

#if META_TARGET_POKITTO
#define SG_MENU_HOME_BIT POKITTO_C
#else
#define SG_MENU_HOME_BIT 0x80
#endif

static void sg_menu_poll(uint8_t raw) {
    const uint8_t home = raw & SG_MENU_HOME_BIT, others = raw & (uint8_t)~SG_MENU_HOME_BIT;
    uint32_t now = millis();
    if (home) {
        if (!sg_menu_was_down) {
            sg_menu_was_down = 1;
            sg_menu_stage = 0;
            sg_menu_since = now;
        } else if (sg_menu_stage == 0 && now - sg_menu_since >= 500) {
            sg_menu_stage = 1; /* no tap on release */
            sg_menu_request = 1;
        }
        if (sg_menu_stage == 0 && others) sg_menu_stage = 3;
    } else if (sg_menu_was_down) {
        sg_menu_was_down = 0;
        if (sg_menu_stage == 0) { SG_MENU_TAP(); }
    }
}

/* The menu's keys as the META_MB_* bits meta_menu_run reads them. */
static uint8_t sg_menu_buttons(void) {
#if META_TARGET_POKITTO
    const uint8_t p = SG_PAD_RAW();
    uint8_t m = 0;
    if (p & POKITTO_DOWN) m |= META_MB_DOWN;
    if (p & POKITTO_LEFT) m |= META_MB_LEFT;
    if (p & POKITTO_RIGHT) m |= META_MB_RIGHT;
    if (p & POKITTO_UP) m |= META_MB_UP;
    if (p & POKITTO_A) m |= META_MB_A;
    if (p & POKITTO_B) m |= META_MB_B;
    return m; /* C stays held: it opened the menu */
#else
    return SG_PAD_RAW(); /* the library's bit order (META pad, ESPboy) */
#endif
}

static void sg_option_menu(void);

#ifdef SG_MENU_ON_STACK_TOP
/* Calls fn on the stack ending at top (the lent cache); sp restored. */
__attribute__((naked, noinline)) static void sg_call_on_stack(void (*fn)(void), uintptr_t top) {
    __asm__ volatile("push {r4, lr}\n\t"
                     "mov r4, sp\n\t"
                     "mov sp, r1\n\t"
                     "blx r0\n\t"
                     "mov sp, r4\n\t"
                     "pop {r4, pc}\n\t");
}
static void sg_menu_run(void) { sg_call_on_stack(sg_option_menu, SG_MENU_ON_STACK_TOP); }
#else
static void sg_menu_run(void) { sg_option_menu(); }
#endif

enum { SGM_RESUME, SGM_RESTART, SGM_SLOT, SGM_SAVE, SGM_LOAD, SGM_VOLUME, SGM_SPEED, SGM_CLOCK,
       SGM_BOOTLOADER, SGM_QUIT };

static void sg_option_menu(void) {
    uint8_t vol = meta_audio_volume();
    uint8_t fast = sg_menu_fast;
    uint8_t mhz = meta_clock_mhz() == (META_TARGET_ESPBOY ? 160 : 72);
#ifdef SG_MENU_STATE_SLOTS
    uint8_t slot = sg_state_slot;
#endif
    meta_menu_item items[11];
    uint8_t ids[11];
    unsigned n = 0;
#define SG_ADD(id, label, type, value, count, names) \
    (ids[n] = (id), items[n++] = (meta_menu_item){label, type, value, count, names})
    SG_ADD(SGM_RESUME, "Reprendre", META_MENU_ACTION, 0, 0, 0);
    SG_ADD(SGM_RESTART, "Redemarrer", META_MENU_ACTION, 0, 0, 0);
#ifdef SG_MENU_STATE_SLOTS
    if (meta_state_available()) {
        sg_name_slots();
        SG_ADD(SGM_SLOT, "Etat", META_MENU_CHOICE, &slot, SG_MENU_STATE_SLOTS, sg_slot_names);
        SG_ADD(SGM_SAVE, "Sauver l'etat", META_MENU_ACTION, 0, 0, 0);
        SG_ADD(SGM_LOAD, "Charger l'etat", META_MENU_ACTION, 0, 0, 0);
    }
#endif
    SG_ADD(SGM_VOLUME, "Volume", META_MENU_CHOICE, &vol, 9, sg_volume_names);
    SG_ADD(SGM_SPEED, "Vitesse", META_MENU_CHOICE, &fast, 2, sg_speed_names);
    SG_ADD(SGM_CLOCK, "Processeur", META_MENU_CHOICE, &mhz, 2, sg_clock_names);
#ifdef SG_MENU_BOOTLOADER
    SG_ADD(SGM_BOOTLOADER, "Bootloader USB", META_MENU_ACTION, 0, 0, 0);
#endif
    SG_ADD(SGM_QUIT, "Quitter", META_MENU_ACTION, 0, 0, 0);
#undef SG_ADD
    /* "59 ips, pile 812": frames of the last second and the stack never
     * used so far (bytes above the end of .bss). */
    char info[32];
#ifdef SG_MENU_STATE_SLOTS
    if (sg_state_msg) {
        strcpy(info, sg_state_msg);
        sg_state_msg = 0;
    } else
#endif
    {
        char *p = sg_put_uint(info, sg_ips);
        memcpy(p, " ips, pile ", 11);
#if META_TARGET_ESPBOY
        p = sg_put_uint(p + 11, 0);
#else
        p = sg_put_uint(p + 11, sg_stack_never_used());
#endif
        *p = 0;
    }
    meta_audio_pause(1); /* the game's sound stops while the menu is open */
    int line = meta_menu_run(SG_MENU_TITLE, items, n, sg_menu_buttons, info);
    meta_audio_pause(0);
    int action = line >= 0 ? ids[line] : -1;
    meta_audio_set_volume(vol);
    sg_menu_fast = fast;
    meta_lcd_end_frame(); /* no transfer while the clocks change */
    meta_clock_set(META_TARGET_ESPBOY ? (mhz ? 160 : 80) : (mhz ? 72 : 48));
#ifdef SG_MENU_STATE_SLOTS
    sg_state_slot = slot;
    if (action == SGM_SAVE || action == SGM_LOAD) {
        meta_lcd_end_frame();
        if (action == SGM_SAVE)
            sg_state_msg = meta_state_save(slot, SG_MENU_STATE_BUF()) ? "Etat sauve" : "Echec sauvegarde";
        else if (!meta_state_load(slot, SG_MENU_STATE_BUF()))
            sg_state_msg = "Echec chargement";
        else {
            SG_MENU_STATE_LOADED();
            sg_state_msg = "Etat charge";
        }
    }
#endif
    SG_MENU_RESUME();
    if (action == SGM_RESTART) SG_MENU_RESTART();
#ifdef SG_MENU_BOOTLOADER
    if (action == SGM_BOOTLOADER) SG_MENU_BOOTLOADER();
#endif
    if (action == SGM_QUIT) SG_MENU_QUIT();
}

/* Once per frame, from frame(): the menu opens here when HOME was held. */
static void sg_menu_tick(void) {
    if (!sg_menu_request) return;
    sg_menu_request = 0;
    sg_menu_run();
#ifdef SG_MENU_ON_STACK_TOP
    SG_MENU_UNLEND();
#endif
}

#endif
