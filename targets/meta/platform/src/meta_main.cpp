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
 *         Start, HOME tap = Select,
 *         HOME held 1 s = next display mode, held 3 s = Gamebuino loader.
 *
 * Pacing: 59.73 Hz; when emulation falls behind, rendering of up to
 * META_MAX_SKIP consecutive frames is skipped.  Frames whose picture cannot
 * differ from the one on screen are not rendered at all (META_STATIC_SKIP).  Once per second a status
 * line is printed on the USB serial port (never on Serial: its pins drive
 * the display).
 */
#include <Arduino.h>
#include <SPI.h>

#include "meta_audio.h"
#include "meta_display.h"
#include "meta_lcd.h"
#include "meta_sd.h"
#include "meta_save.h"
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
#define BTN_CS_MASK (1u << 3) /* PB03 */

#ifndef GBL_GEN_VERSION
#define GBL_GEN_VERSION "unknown"
#endif
/* One contiguous string, kept in flash: compile.sh reads it back from
 * firmware.bin to tell which gbrecomp produced this firmware. */
static const char gbl_gen_version[] __attribute__((used)) =
    "gbrecomp-meta " GBL_GEN_VERSION;

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

/* META A gives Game Boy B and META B gives Game Boy A; 0 maps them straight. */
#ifndef META_SWAP_AB
#define META_SWAP_AB 1
#endif

static uint8_t read_buttons_raw(void) {
    SPI.beginTransaction(SPISettings(META_BUTTONS_SPI_HZ, MSBFIRST, SPI_MODE0));
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
    gbl_save_flush(); /* unsaved cartridge RAM changes */
    meta_lcd_end_frame();
    meta_audio_stop();
    NVIC_DisableIRQ(DMAC_IRQn);
    meta_sd_deselect();
    ((void (*)(void))(*((uint32_t *)0x3FF4)))();
}

static uint32_t home_since;
static uint8_t home_held, home_stage, select_frames;

static void poll_input(void) {
    uint8_t m = read_buttons_raw();
    uint8_t gb = 0;
#ifdef META_BUTTONS_LAPINOU_ORDER
    /* 0 left, 1 right, 2 up, 3 A, 4 B, 5 MENU, 6 down, 7 HOME */
    if (m & 0x08) gb |= 0x01;
    if (m & 0x10) gb |= 0x02;
    if (m & 0x20) gb |= 0x08;
    if (m & 0x02) gb |= 0x10;
    if (m & 0x01) gb |= 0x20;
    if (m & 0x04) gb |= 0x40;
    if (m & 0x40) gb |= 0x80;
#else
    /* Gamebuino library order: 0 down, 1 left, 2 right, 3 up, 4 A, 5 B,
     * 6 MENU, 7 HOME. */
    if (m & 0x10) gb |= 0x01; /* A */
    if (m & 0x20) gb |= 0x02; /* B */
    if (m & 0x40) gb |= 0x08; /* MENU -> Start */
    if (m & 0x04) gb |= 0x10; /* right */
    if (m & 0x02) gb |= 0x20; /* left */
    if (m & 0x08) gb |= 0x40; /* up */
    if (m & 0x01) gb |= 0x80; /* down */
#endif

#if META_SWAP_AB
    /* The META's A is the right-hand button: Game Boy B, and vice versa. */
    gb = (uint8_t)((gb & ~0x03u) | ((gb & 0x01) << 1) | ((gb & 0x02) >> 1));
#endif

    uint32_t now = millis();
    if (m & 0x80) {
        if (!home_held) {
            home_held = 1;
            home_stage = 0;
            home_since = now;
        } else if (now - home_since > 3000) {
            enter_loader();
        } else if (now - home_since > 1000 && home_stage == 0) {
            home_stage = 1;
            meta_display_set_mode((uint8_t)((meta_display_mode() + 1) % META_MODE_COUNT));
        }
    } else if (home_held) {
        home_held = 0;
        if (home_stage == 0) select_frames = 6; /* a tap becomes a Select press */
    }
    if (select_frames) { gb |= 0x04; select_frames--; }
    gbl_set_buttons(gb);
}

/* ---- pacing and statistics ------------------------------------------------ */

static uint32_t next_frame_us, stat_start_ms, idle_us;
static int save_state;
#if GBL_ROM_STREAM
static uint32_t stream_reported;
#endif
static uint16_t frames, shown, unchanged, skipped_run;

extern "C" void gbl_platform_vblank(void) {
    /* If the loader locked the watchdog on (ALWAYSON), initVariant could not
     * disable it: feed it once per frame instead. */
    if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
    int bus_free = close_frame();
    int was_shown = !gbl_skip_render;
    if (was_shown) shown++;
    frames++;
    int static_next = META_STATIC_SKIP ? gbl_frame_unchanged_next(was_shown) : 0;
    if (bus_free) poll_input(); /* else keep last frame's buttons */
    meta_save_tick();
    meta_audio_frame();

    uint32_t now = micros();
    int32_t late = (int32_t)(now - next_frame_us);
    if (late < 0) {
        idle_us += (uint32_t)-late;
        while ((int32_t)(micros() - next_frame_us) < 0) meta_audio_idle();
        late = 0;
    } else if (late > 200000) {
        next_frame_us = now; /* long stall (LCD off loops, USB...): resync */
        late = 0;
    }
    next_frame_us += FRAME_US;

    if (static_next) {
        gbl_skip_render = 1;
        unchanged++;
    } else if (late > (int32_t)FRAME_US && skipped_run < META_MAX_SKIP) {
        gbl_skip_render = 1;
        skipped_run++;
    } else {
        gbl_skip_render = 0;
        skipped_run = 0;
    }

    uint32_t ms = millis();
    if (ms - stat_start_ms >= 1000) {
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
        gbl_interp_instrs = 0;
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

extern "C" char __bss_end__;

/* A file the game needs is missing or unusable: a coloured screen tells
 * which, the USB serial port says it in words, HOME held 3 s returns to the
 * loader.  Red: ROM not found, blue: card unreadable, orange: ROM
 * fragmented, magenta: a different ROM, yellow: save file missing. */
static void __attribute__((unused)) boot_error(uint16_t colour_be, const char *text) {
    meta_lcd_fill(colour_be);
    uint32_t held = 0, last = 0;
    for (;;) {
        if (WDT->CTRL.reg & WDT_CTRL_ENABLE) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
        uint8_t m = read_buttons_raw();
        uint32_t now = millis();
        if (m & 0x80) {
            if (!held) held = now;
            else if (now - held > 3000) enter_loader();
        } else {
            held = 0;
        }
        if (now - last >= 1000) {
            last = now;
            if (SerialUSB) {
                SerialUSB.print("ROM " GBL_TITLE " : ");
                SerialUSB.println(text);
            }
        }
        delay(20);
    }
}

#if GBL_ROM_STREAM && !GBL_ROM_PACKED /* packed: the ROM is all in flash */
static void stream_error_screen(int err) {
    static const uint16_t colour_be[] = {0, 0x1F00, 0x00F8, 0x00FD, 0x1FF8}; /* RGB565, byte-swapped */
    static const char *const text[] = {
        "", "carte SD illisible (FAT16/FAT32 attendu)",
        "fichier " GBL_STREAM_NAME " introuvable (racine de la carte ou un dossier)",
        "fichier " GBL_STREAM_NAME " trop fragmente : recopie-le",
        "fichier " GBL_STREAM_NAME " : ce n'est pas la ROM de ce firmware"};
    boot_error(colour_be[err], text[err]);
}
#endif

void setup() {
    PORT->Group[1].DIRSET.reg = BTN_CS_MASK;
    PORT->Group[1].OUTSET.reg = BTN_CS_MASK;
    meta_lcd_init();
    /* Like the Gamebuino library's SD.begin at boot: the loader relies on
     * the card being in SPI mode when HOME brings it back. */
    int card_ok = meta_sd_init();
    SerialUSB.begin(115200);
    /* Also keeps gbl_gen_version in the binary (compile.sh reads it there). */
    SerialUSB.println(gbl_gen_version);
#if GBL_ROM_STREAM && !GBL_ROM_PACKED
    /* Before gbl_reset: the lookup borrows the stream cache. */
    int stream_err = meta_stream_init(card_ok);
    if (stream_err) stream_error_screen(stream_err);
#endif
    /* Also before gbl_reset, which loads the saved cartridge RAM. */
    save_state = meta_save_init(card_ok);
#if GBL_ERAM_SIZE > 0 && GBL_ERAM_ON_SD
    if (save_state == META_SAVE_MISSING)
        boot_error(0xE0FF, /* yellow */
                   "fichier " GBL_SAVE_NAME " introuvable : copie-le a cote de la ROM (compile.sh le cree)");
#endif
    meta_lcd_fill(meta_dmg_palette[0]);
    meta_audio_init();

    meta_display_set_mode(META_MODE_TBLEND); /* TILE when streaming */
    /* Keep generated code within a fixed C stack budget; deeper nesting
     * unwinds to the nearest dispatcher loop. */
    char probe;
    uintptr_t limit = (uintptr_t)&probe - 3072u;
    uintptr_t floor_addr = (uintptr_t)&__bss_end__ + 768u; /* interrupt headroom */
    gbl_stack_limit = limit > floor_addr ? limit : floor_addr;

    gbl_reset();
    next_frame_us = micros();
    stat_start_ms = millis();
    gbl_run();
}

void loop() {}
