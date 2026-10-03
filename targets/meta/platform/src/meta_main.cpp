/*
 * Gamebuino META frontend for gbl.
 *
 * Video: the display pipeline (meta_display.c) renders scanlines into strips
 * sent to the ST7735 by DMA while emulation continues.  The DMG's 144 lines
 * become the META's 128 with one of seven modes (HOME held one second
 * cycles them): TBLEND (default: TILE, with the last line of each tile row
 * merged into the next instead of dropped), SMOOTH (every row blends two
 * lines by area), TILE (drop the last line of each tile row), BLEND, DARK,
 * CROP, SKIP.
 *
 * Input:  D-pad, A = Game Boy B and B = Game Boy A (META_SWAP_AB), MENU =
 *         Start, HOME tap = Select, HOME held half a second = option menu
 *         (display mode, palette, frame skip, CPU clock, back to the
 *         Gamebuino loader).
 *
 * Pacing: 59.73 Hz, kept by an accumulator of elapsed time against emulated
 * frames (Gdl's rate).  Frame skip: automatic (when emulation falls behind
 * by a frame, rendering of up to META_MAX_SKIP consecutive frames is
 * skipped), none, or one frame drawn in two, three or four.  Frames whose
 * picture cannot differ from the one on screen are not rendered at all
 * (META_STATIC_SKIP).  Once per second a status
 * line is printed on the USB serial port (never on Serial: its pins drive
 * the display).
 *
 * The same file is the Pokitto frontend (meta_target.h, --target pokitto):
 * bare metal instead of the Arduino core, the picture 1:1 in the middle of
 * the 220x176 panel, A = A, B = B, C tap = Start, C held half a second =
 * option menu, C + B = Select, C held at power-on = the Pokitto loader.
 * The META-only parts are under META_ARDUINO / META_SCALED_MODES /
 * META_USB_SERIAL; the Pokitto drivers are in platform/pokitto.
 */
#include "meta_target.h"

#if META_ARDUINO
#include <Arduino.h>
#include <SPI.h>
#else
#include <string.h>
#include "pokitto_hw.h"
#endif
#include <setjmp.h>

#include "meta_audio.h"
#include "meta_clock.h"
#include "meta_display.h"
#include "meta_lcd.h"
#include "meta_menu.h"
#include "meta_sd.h"
#include "meta_save.h"
#include "meta_state.h"
#include "meta_stream.h"

extern "C" {
#include "gbl.h"
#include "gbl_config.h"
#include "gbl_options.h"
}

#ifndef META_MAX_SKIP
#define META_MAX_SKIP 3
#endif
/* Skip rendering and SPI transfer while the picture cannot have changed
 * (see gbl_frame_unchanged_next); a change then appears one frame late. */
#ifndef META_STATIC_SKIP
#define META_STATIC_SKIP 1
#endif

#define FRAME_US 16743u   /* 70224 cycles at 4.194304 MHz */

#ifndef GBL_GEN_VERSION
#define GBL_GEN_VERSION "unknown"
#endif
/* One contiguous string, kept in flash: compile.sh reads it back from
 * firmware.bin to tell which gbrecomp produced this firmware
 * ("gbrecomp-meta x.y.z", "gbrecomp-pokitto x.y.z"). */
static const char gbl_gen_version[] __attribute__((used)) =
    "gbrecomp-" META_TARGET_NAME " " GBL_GEN_VERSION;

static uint8_t input_deferred;

/* Hand the last strip to the DMA and, when the bus is free (or input was
 * already deferred once), release it so the buttons can be read. */
static int close_frame(void) {
    meta_display_flush();
    if (meta_lcd_busy() && !input_deferred) {
        input_deferred = 1;
        return 0;
    }
    meta_lcd_end_frame();
    input_deferred = 0;
    return 1;
}

/* ---- buttons -------------------------------------------------------------- */

#if META_ARDUINO
#define BTN_CS_MASK (1u << 3) /* PB03 */

/* 12 MHz like the Gamebuino library.  The library also waits ~1 us after CS
 * low for the shift register's parallel load; reading immediately works on
 * the META tested here, so the wait is optional (META_BUTTONS_PL_US). */
#ifndef META_BUTTONS_PL_US
#define META_BUTTONS_PL_US 0
#endif
#ifndef META_BUTTONS_SPI_HZ
#define META_BUTTONS_SPI_HZ 12000000
#endif

static uint8_t buttons_raw = 0xFF; /* last shift-register byte (active low) */

#endif /* META_ARDUINO */

/* META A gives Game Boy B and META B gives Game Boy A; 0 maps them straight
 * (the Pokitto's default: its A is already the right-hand button). */
#ifndef META_SWAP_AB
#define META_SWAP_AB META_SWAP_AB_DEFAULT
#endif

#if META_ARDUINO

static uint8_t read_buttons_raw(void) {
    SPI.beginTransaction(SPISettings(meta_spi_hz(META_BUTTONS_SPI_HZ), MSBFIRST, SPI_MODE0));
    /* Drop bytes left by display writes (the receive FIFO holds two). */
    for (int i = 0; i < 2 && (SERCOM4->SPI.INTFLAG.reg & SERCOM_SPI_INTFLAG_RXC); i++)
        (void)SERCOM4->SPI.DATA.reg;
    SERCOM4->SPI.STATUS.reg = SERCOM_SPI_STATUS_BUFOVF;
    PORT->Group[1].OUTCLR.reg = BTN_CS_MASK;
#if META_BUTTONS_PL_US > 0
    delayMicroseconds(META_BUTTONS_PL_US);
#endif
    uint8_t raw = SPI.transfer(1);
    buttons_raw = raw;
    PORT->Group[1].OUTSET.reg = BTN_CS_MASK;
    SPI.endTransaction();
    return (uint8_t)~raw; /* active low */
}

/* Hand the console back to the Gamebuino loader in the state it expects:
 * display idle, our audio and DMA interrupts off, SD card deselected and in
 * SPI mode. */
static void enter_loader(void) {
    meta_clock_set(48); /* the loader expects the core's clocks */
    gbl_save_flush(); /* unsaved cartridge RAM changes */
    meta_lcd_end_frame();
    meta_audio_stop();
    NVIC_DisableIRQ(DMAC_IRQn);
    meta_sd_deselect();
    ((void (*)(void))(*((uint32_t *)0x3FF4)))();
}

/* Pressed buttons in the Gamebuino library order (META_MB_*): 0 down,
 * 1 left, 2 right, 3 up, 4 A, 5 B, 6 MENU, 7 HOME. */
static uint8_t meta_buttons(void) {
    uint8_t m = read_buttons_raw();
#ifdef META_BUTTONS_LAPINOU_ORDER
    /* 0 left, 1 right, 2 up, 3 A, 4 B, 5 MENU, 6 down, 7 HOME */
    uint8_t b = (uint8_t)(m & 0x80);
    if (m & 0x01) b |= META_MB_LEFT;
    if (m & 0x02) b |= META_MB_RIGHT;
    if (m & 0x04) b |= META_MB_UP;
    if (m & 0x08) b |= META_MB_A;
    if (m & 0x10) b |= META_MB_B;
    if (m & 0x20) b |= META_MB_MENU;
    if (m & 0x40) b |= META_MB_DOWN;
    return b;
#else
    return m;
#endif
}

#else /* Pokitto */

static uint8_t buttons_raw; /* last state, for symmetry with the META */

/* Hand the console to the Pokitto loader: saves written back, sound off,
 * card deselected.  Without a loader in flash this returns. */
static void enter_loader(void) {
    gbl_save_flush();
    meta_audio_stop();
    meta_sd_deselect();
    pokitto_enter_loader();
    meta_audio_init();
}

/* Pokitto buttons as META_MB_* bits: C plays the META's HOME. */
static uint8_t meta_buttons(void) {
    uint8_t p = pokitto_buttons();
    buttons_raw = p;
    uint8_t m = 0;
    if (p & POKITTO_A) m |= META_MB_A;
    if (p & POKITTO_B) m |= META_MB_B;
    if (p & POKITTO_C) m |= META_MB_HOME;
    if (p & POKITTO_UP) m |= META_MB_UP;
    if (p & POKITTO_DOWN) m |= META_MB_DOWN;
    if (p & POKITTO_LEFT) m |= META_MB_LEFT;
    if (p & POKITTO_RIGHT) m |= META_MB_RIGHT;
    return m;
}

#endif /* META_ARDUINO */

static uint32_t home_since;
static uint8_t home_held, home_stage, select_frames, menu_request;
#if META_TARGET_POKITTO
static uint8_t start_frames;
#endif

static void poll_input(void) {
    uint8_t m = meta_buttons();
    uint8_t gb = 0;
    if (m & META_MB_A) gb |= 0x01;
    if (m & META_MB_B) gb |= 0x02;
    if (m & META_MB_MENU) gb |= 0x08; /* Start */
    if (m & META_MB_RIGHT) gb |= 0x10;
    if (m & META_MB_LEFT) gb |= 0x20;
    if (m & META_MB_UP) gb |= 0x40;
    if (m & META_MB_DOWN) gb |= 0x80;

#if META_SWAP_AB
    /* The META's A is the right-hand button: Game Boy B, and vice versa. */
    gb = (uint8_t)((gb & ~0x03u) | ((gb & 0x01) << 1) | ((gb & 0x02) >> 1));
#endif

    uint32_t now = millis();
#if META_TARGET_POKITTO
    /* C: a tap is Start, held half a second the option menu, with B Select
     * (B itself is then not pressed). */
    if (m & META_MB_HOME) {
        if (!home_held) {
            home_held = 1;
            home_stage = 0;
            home_since = now;
        }
        if (home_stage != 1 && (m & META_MB_B)) home_stage = 2;
        if (home_stage == 2) gb = (uint8_t)((gb & ~0x02u) | 0x04);
        else if (home_stage == 0 && now - home_since >= 500) {
            home_stage = 1;
            menu_request = 1;
        }
    } else if (home_held) {
        home_held = 0;
        if (home_stage == 0) start_frames = 6; /* a tap becomes a Start press */
    }
    if (start_frames) { gb |= 0x08; start_frames--; }
#else
    if (m & META_MB_HOME) {
        if (!home_held) {
            home_held = 1;
            home_stage = 0;
            home_since = now;
        } else if (now - home_since >= 500 && home_stage == 0) {
            home_stage = 1; /* no Select tap on release */
            menu_request = 1;
        }
    } else if (home_held) {
        home_held = 0;
        if (home_stage == 0) select_frames = 6; /* a tap becomes a Select press */
    }
#endif
    if (select_frames) { gb |= 0x04; select_frames--; }
    gbl_set_buttons(gb);
}

/* ---- pacing, frame skip and options --------------------------------------- */

/* Frame skip of the option menu: automatic, none, or one frame drawn in
 * 2, 3 or 4. */
enum { FSKIP_AUTO, FSKIP_NONE, FSKIP_HALF, FSKIP_THIRD, FSKIP_QUARTER, FSKIP_MODES };
static uint8_t skip_mode = FSKIP_AUTO;
static uint8_t fast_mode; /* menu "Vitesse": no frame limit */
static uint16_t last_frames, last_shown; /* previous second, for the menu */
static jmp_buf restart_point;             /* menu "Redemarrer", loaded states */
#ifdef GBL_STATE_SLOTS
#define GBL_STATE_SLOTS_OR_1 GBL_STATE_SLOTS
#else
#define GBL_STATE_SLOTS_OR_1 1
#endif

static uint32_t stat_start_ms, idle_us;
static int save_state;
#if GBL_ROM_STREAM && META_USB_SERIAL
static uint32_t stream_reported;
#endif
static uint16_t frames, shown, unchanged, skipped_run;

/* Time owed to the emulation, like Gdl's rate: elapsed time comes in, each
 * emulated frame takes FRAME_US out.  Below zero the frame was early and
 * pacing waits; a frame or more ahead, the automatic frame skip renders
 * nothing until it has caught up. */
static uint32_t pace_last;
static int32_t pace_owed;

static void pace_reset(void) {
    pace_last = meta_micros();
    pace_owed = 0;
}

static void pace_frame(void) {
    uint32_t now = meta_micros();
    if (fast_mode) {
        pace_last = now;
        pace_owed = 0;
        return;
    }
    pace_owed += (int32_t)(now - pace_last);
    pace_last = now;
    pace_owed -= (int32_t)FRAME_US;
    if (pace_owed < 0) {
        uint32_t until = now + (uint32_t)-pace_owed;
        idle_us += (uint32_t)-pace_owed;
        while ((int32_t)(meta_micros() - until) < 0) meta_audio_idle();
        now = meta_micros();
        pace_owed += (int32_t)(now - pace_last);
        pace_last = now;
    }
    /* Long stall (LCD off loops, USB, the menu), or no catching up wanted:
     * forget the debt. */
    if (pace_owed > 200000 || (skip_mode != FSKIP_AUTO && pace_owed > (int32_t)FRAME_US))
        pace_owed = (int32_t)FRAME_US;
}

#if META_SCALED_MODES
static const char *const mode_names[META_MODE_COUNT] = {"SMOOTH", "TILE", "BLEND", "DARK",
                                                        "CROP", "SKIP", "TBLEND"};
#endif
static const char *const skip_names[FSKIP_MODES] = {"Auto", "Aucun", "1 sur 2", "1 sur 3", "1 sur 4"};
static const char *const speed_names[2] = {"Normale", "Rapide"};
static const char *const clock_names[2] = {"48 MHz", "72 MHz"};

static char *put_uint(char *p, unsigned v) {
    char tmp[10];
    int n = 0;
    do tmp[n++] = (char)('0' + v % 10u); while ((v /= 10u) && n < 10);
    while (n) *p++ = tmp[--n];
    return p;
}

/* Save states: the menu picks the slot and the operation, which runs at
 * the next instruction boundary (gbl_platform_safe_point). */
enum { STATE_NONE, STATE_SAVE, STATE_LOAD };
static uint8_t state_slot, state_op;
static const char *state_msg; /* result, shown once by the next menu */
static char slot_text[GBL_STATE_SLOTS_OR_1][8];
static const char *slot_names[GBL_STATE_SLOTS_OR_1];
static const char *const volume_names[9] = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};

static void name_slots(void) {
    uint16_t used = meta_state_used();
    for (unsigned i = 0; i < GBL_STATE_SLOTS_OR_1; i++) {
        char *p = put_uint(slot_text[i], i + 1u);
        if (!(used >> i & 1u)) memcpy(p, " vide", 6);
        else *p = 0;
        slot_names[i] = slot_text[i];
    }
}

enum { M_RESUME, M_RESTART, M_SLOT, M_SAVE, M_LOAD, M_VOLUME, M_SCREEN, M_PALETTE, M_SKIP,
       M_SPEED, M_CLOCK, M_QUIT };

static void option_menu(void) {
    uint8_t mode = meta_display_mode(), pal = meta_display_palette(), skip = skip_mode;
    uint8_t fast = fast_mode, mhz = meta_clock_mhz() == 72, vol = meta_audio_volume();
    uint8_t slot = state_slot;
    meta_menu_item items[13];
    uint8_t ids[13];
    unsigned n = 0;
#define ADD(id, label, type, value, count, names) \
    (ids[n] = (id), items[n++] = (meta_menu_item){label, type, value, count, names})
    ADD(M_RESUME, "Reprendre", META_MENU_ACTION, 0, 0, 0);
    ADD(M_RESTART, "Redemarrer", META_MENU_ACTION, 0, 0, 0);
    if (meta_state_available()) {
        name_slots();
        ADD(M_SLOT, "Etat", META_MENU_CHOICE, &slot, GBL_STATE_SLOTS_OR_1, slot_names);
        ADD(M_SAVE, "Sauver l'etat", META_MENU_ACTION, 0, 0, 0);
        ADD(M_LOAD, "Charger l'etat", META_MENU_ACTION, 0, 0, 0);
    }
    ADD(M_VOLUME, "Volume", META_MENU_CHOICE, &vol, 9, volume_names);
#if META_SCALED_MODES
    ADD(M_SCREEN, "Ecran", META_MENU_CHOICE, &mode, META_MODE_COUNT, mode_names);
#endif
    ADD(M_PALETTE, "Palette", META_MENU_CHOICE, &pal, META_PALETTE_COUNT, meta_palette_names);
    ADD(M_SKIP, "Images", META_MENU_CHOICE, &skip, FSKIP_MODES, skip_names);
    ADD(M_SPEED, "Vitesse", META_MENU_CHOICE, &fast, 2, speed_names);
    ADD(M_CLOCK, "Processeur", META_MENU_CHOICE, &mhz, 2, clock_names);
    ADD(M_QUIT, "Quitter", META_MENU_ACTION, 0, 0, 0);
#undef ADD
    /* "59 ips, 41 vues": emulated and drawn frames of the last second, or
     * the result of the last save state operation. */
    char info[32];
    if (state_msg) {
        strcpy(info, state_msg);
        state_msg = 0;
    } else {
        char *p = put_uint(info, last_frames);
        memcpy(p, " ips, ", 6);
        p = put_uint(p + 6, last_shown);
        memcpy(p, " vues", 6);
    }
    int line = meta_menu_run(GBL_TITLE, items, n, meta_buttons, info);
    int action = line >= 0 ? ids[line] : -1;
    if (action == M_QUIT) enter_loader();
    skip_mode = skip;
    fast_mode = fast;
    state_slot = slot;
    meta_audio_set_volume(vol);
    meta_clock_set(mhz ? 72 : 48);
    if (pal != meta_display_palette()) meta_display_set_palette(pal);
    meta_display_set_mode(mode); /* also redraws the whole picture */
    skipped_run = 0;
    pace_reset();
    if (action == M_SAVE || action == M_LOAD) {
        state_op = action == M_SAVE ? STATE_SAVE : STATE_LOAD;
        gbl_safe_request = 1;
        gbr.next_evt = gbr.cyc; /* reach an interrupt check soon */
    }
    if (action == M_RESTART) {
        gbl_save_flush(); /* gbl_reset reads the cartridge RAM back */
        longjmp(restart_point, 1);
    }
}

/* Between two instructions (gbl_irq_enter): run the menu's save state
 * operation.  The strip buffers serve as the sector buffer. */
extern "C" void gbl_platform_safe_point(void) {
    uint8_t op = state_op;
    state_op = STATE_NONE;
    if (op == STATE_NONE) return;
    unsigned rows;
    uint8_t *buf = (uint8_t *)meta_display_scratch(&rows);
    meta_lcd_wait();
    meta_lcd_end_frame();
    if (op == STATE_SAVE) {
        state_msg = meta_state_save(state_slot, buf) ? "Etat sauve" : "Echec sauvegarde";
        return;
    }
    if (!meta_state_load(state_slot, buf)) {
        state_msg = "Echec chargement";
        return;
    }
    state_msg = "Etat charge";
    meta_display_set_mode(meta_display_mode());
    pace_reset();
    longjmp(restart_point, 2); /* the machine as loaded: start over from gbr.pc */
}

extern "C" void gbl_platform_vblank(void) {
#if META_ARDUINO
    /* If the loader locked the watchdog on (ALWAYSON), initVariant could not
     * disable it: feed it once per frame instead. */
    if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
#endif
    int bus_free = close_frame();
    int was_shown = !gbl_skip_render;
    if (was_shown) shown++;
    frames++;
    int static_next = META_STATIC_SKIP ? gbl_frame_unchanged_next(was_shown) : 0;
    if (bus_free) poll_input(); /* else keep last frame's buttons */
    if (menu_request) {
        menu_request = 0;
        option_menu();
        static_next = 0;
    }
    /* Audio first: a save flush can block on the SD card, and the DMA then
     * plays from a full ring (meta_audio.cpp). */
    meta_audio_frame();
    meta_save_tick();
    pace_frame();

    static uint8_t phase;
    int skip = 0;
    if (skip_mode == FSKIP_AUTO)
        skip = pace_owed >= (int32_t)FRAME_US && skipped_run < META_MAX_SKIP;
    else if (skip_mode != FSKIP_NONE)
        skip = phase != 0; /* draw one frame in skip_mode (2, 3 or 4) */
    if (skip_mode >= FSKIP_HALF) phase = (uint8_t)(phase + 1 < skip_mode ? phase + 1 : 0);
    if (static_next) {
        gbl_skip_render = 1;
        unchanged++;
    } else if (skip) {
        gbl_skip_render = 1;
        skipped_run++;
    } else {
        gbl_skip_render = 0;
        skipped_run = 0;
    }

    uint32_t ms = millis();
    if (ms - stat_start_ms >= 1000) {
#if META_USB_SERIAL
        /* Only when a terminal is attached and the report fits without
         * blocking emulation. */
        if (SerialUSB && SerialUSB.availableForWrite() >= 64) {
            SerialUSB.print("btn 0x");
            SerialUSB.print((uint8_t)~buttons_raw, HEX); /* pressed bits: 1 down 2 left 4 right 8 up 10 A 20 B 40 menu 80 home */
            SerialUSB.print(", emu ");
            SerialUSB.print(frames);
            SerialUSB.print(" fps, shown ");
            SerialUSB.print(shown);
            SerialUSB.print(", lcd waits ");
            SerialUSB.print(meta_display_lcd_waits);
            SerialUSB.print(", rows kept ");
            SerialUSB.print(meta_display_rows_skipped);
            SerialUSB.print(", unchanged ");
            SerialUSB.print(unchanged);
            SerialUSB.print(", idle ");
            SerialUSB.print(idle_us / ((ms - stat_start_ms) * 10u));
            SerialUSB.print("%");
            if (save_state == META_SAVE_NO_FILE) SerialUSB.print(", SAUVEGARDE DESACTIVEE (fichier .SAV absent)");
            SerialUSB.print(", saves ");
            SerialUSB.print(meta_save_writes);
            SerialUSB.print(", interp ");
#if GBL_ROM_STREAM
            SerialUSB.print(gbl_interp_instrs);
            SerialUSB.print(GBL_ROM_PACKED ? ", unpacked sectors " : ", SD sectors ");
            SerialUSB.print(gbl_stream_misses - stream_reported);
            SerialUSB.print(" (errors ");
            SerialUSB.print(meta_stream_errors);
            SerialUSB.println(")");
            stream_reported = gbl_stream_misses;
#else
            SerialUSB.println(gbl_interp_instrs);
#endif
        }
#endif /* META_USB_SERIAL */
        gbl_interp_instrs = 0;
        last_frames = frames;
        last_shown = shown;
        frames = shown = unchanged = 0;
        meta_display_lcd_waits = meta_display_rows_skipped = 0;
        idle_us = 0;
        stat_start_ms = ms;
    }
}

/* Runs right after the core's init(), before the first SysTick.  The META
 * loader normally points VTOR at the application already; be certain.  It
 * also leaves its launch watchdog armed and nothing in this runtime feeds
 * it, so disarm it here or the console resets shortly after boot. */
#if META_ARDUINO
void initVariant() {
    SCB->VTOR = 0x4000u & SCB_VTOR_TBLOFF_Msk;
    NVIC_DisableIRQ(WDT_IRQn);
    WDT->INTENCLR.reg = WDT_INTENCLR_EW;
    WDT->INTFLAG.reg = WDT_INTFLAG_EW;
    WDT->CTRL.reg = 0;
    while (WDT->STATUS.reg & WDT_STATUS_SYNCBUSY) {}
    /* Keep the SD card off the shared SPI bus from the very start. */
    meta_sd_deselect();
}
#endif

extern "C" char __bss_end__;

/* A file the game needs is missing or unusable: a coloured screen tells
 * which, the USB serial port says it in words (META), HOME (Pokitto: C)
 * held 3 s returns to the loader.  Red: ROM not found, blue: card
 * unreadable, orange: ROM fragmented, magenta: a different ROM, yellow:
 * save file missing. */
static void __attribute__((unused)) boot_error(uint16_t colour, const char *text) {
    meta_lcd_fill(colour);
    uint32_t held = 0, last = 0;
    for (;;) {
#if META_ARDUINO
        if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
        uint8_t m = read_buttons_raw();
#else
        uint8_t m = meta_buttons();
#endif
        uint32_t now = millis();
        if (m & 0x80) {
            if (!held) held = now;
            else if (now - held > 3000) enter_loader();
        } else {
            held = 0;
        }
        if (now - last >= 1000) {
            last = now;
#if META_USB_SERIAL
            if (SerialUSB) {
                SerialUSB.print("ROM " GBL_TITLE " : ");
                SerialUSB.println(text);
            }
#else
            (void)text;
#endif
        }
        delay(20);
    }
}

#if GBL_ROM_STREAM && !GBL_ROM_PACKED /* packed: the ROM is all in flash */
static void stream_error_screen(int err) {
    static const uint16_t colour[] = {0, META_COLOR(0x0000FF), META_COLOR(0xFF0000), META_COLOR(0xFFA000),
                                      META_COLOR(0xFF00FF)};
    static const char *const text[] = {
        "", "carte SD illisible (FAT16/FAT32 attendu)",
        "fichier " GBL_STREAM_NAME " introuvable (racine de la carte ou un dossier)",
        "fichier " GBL_STREAM_NAME " trop fragmente : recopie-le",
        "fichier " GBL_STREAM_NAME " : ce n'est pas la ROM de ce firmware"};
    boot_error(colour[err], text[err]);
}
#endif

void setup() {
#if META_ARDUINO
    PORT->Group[1].DIRSET.reg = BTN_CS_MASK;
    PORT->Group[1].OUTSET.reg = BTN_CS_MASK;
#else
    pokitto_buttons_init();
#endif
    meta_lcd_init();
    /* Like the Gamebuino library's SD.begin at boot: the loader relies on
     * the card being in SPI mode when HOME brings it back. */
    int card_ok = meta_sd_init();
#if META_USB_SERIAL
    SerialUSB.begin(115200);
    /* Also keeps gbl_gen_version in the binary (compile.sh reads it there). */
    SerialUSB.println(gbl_gen_version);
#else
    __asm__ volatile("" ::"r"(gbl_gen_version)); /* kept in the binary for compile.sh */
#endif
#if GBL_ROM_STREAM && !GBL_ROM_PACKED
    /* Before gbl_reset: the lookup borrows the stream cache. */
    int stream_err = meta_stream_init(card_ok);
    if (stream_err) stream_error_screen(stream_err);
#endif
    /* Also before gbl_reset, which loads the saved cartridge RAM. */
    save_state = meta_save_init(card_ok);
    {
        unsigned rows;
        meta_state_init(card_ok, (uint8_t *)meta_display_scratch(&rows));
    }
#if GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD
    if (save_state == META_SAVE_MISSING)
        boot_error(META_COLOR(0xFFFF00),
                   "fichier " GBL_SAVE_NAME " introuvable : copie-le a cote de la ROM (compile.sh le cree)");
#endif
#if META_TARGET_POKITTO
    meta_lcd_fill(0); /* black around the picture */
#else
    meta_lcd_fill(meta_dmg_palette[0]);
#endif
    meta_clock_init(); /* after SerialUSB.begin: USB moves to generator 4 */
    meta_audio_init();

#if META_SCALED_MODES
    meta_display_set_mode(META_MODE_TBLEND); /* TILE when streaming */
#else
    meta_display_set_mode(0);
#endif
    /* Keep generated code within a fixed C stack budget; deeper nesting
     * unwinds to the nearest dispatcher loop. */
    char probe;
    uintptr_t limit = (uintptr_t)&probe - 3072u;
    uintptr_t floor_addr = (uintptr_t)&__bss_end__ + 768u; /* interrupt headroom */
    gbl_stack_limit = limit > floor_addr ? limit : floor_addr;

    /* The menu's "Redemarrer" comes back here (from inside the emulation:
     * longjmp drops its C frames). */
    if (setjmp(restart_point) != 2) gbl_reset(); /* 2: a state was loaded */
    gbl_skip_render = 0;
    pace_reset();
    stat_start_ms = millis();
    gbl_run();
}

#if META_ARDUINO
void loop() {}
#else
int main(void) {
    setup();
    for (;;) {}
}
#endif
