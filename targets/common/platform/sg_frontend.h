/* Frontend pieces the SG-1000 and Game Gear builds share: console setup, the
 * pad read, frame pacing and the C stack guard.  The recompiler copies this
 * file next to sg_main.cpp / gg_main.cpp, which include it once sg1000.h
 * (or gg.h) and the consoles' drivers are in scope.
 *
 * These are macros rather than functions on purpose: the two firmwares then
 * see the statements they had before this file existed, so a refactor keeps
 * firmware.bin comparable with the previous sources. */
#ifndef SG_FRONTEND_H
#define SG_FRONTEND_H

extern "C" {
extern char end; /* end of .bss: the C stack grows down to it */
extern char __StackTop;
}

/* NTSC: 59736 T-states at 3.579545 MHz.  The CPU, the PSG and the sprite
 * status keep full precision whatever is skipped. */
#define SG_FRAME_US 16688u
/* Behind by six frames (a stall): restart from now instead of running fast
 * to catch up; smaller debts are paid back.  Signed, so SG_PACE_FRAME's
 * comparison stays the signed one the frontends had. */
#define SG_STALL_US 100128

#if META_TARGET_POKITTO
#define SG_PLATFORM_INIT() do { \
        pokitto_buttons_init(); \
        meta_clock_init(); \
        meta_lcd_init(); \
        meta_lcd_fill(0); \
    } while (0)
#else
#define SG_PLATFORM_INIT() do { \
        meta_spi_init(); \
        meta_clock_init(); \
        if (META_CLOCK_DEFAULT != 48) meta_clock_set(META_CLOCK_DEFAULT); /* META_OVERCLOCK */ \
        meta_lcd_init(); \
        meta_lcd_fill(0); \
    } while (0)
#endif

/* Pad: the library's bit order on the META (META_BUTTONS_SPI_HZ), the
 * Pokitto's own bits through its driver; the META's watchdog is fed while
 * the pad is clocked. */
#if META_TARGET_POKITTO
#define SG_PAD_RAW() pokitto_buttons()
#define SG_PAD_WDT() do { } while (0)
#else
#define SG_PAD_RAW() meta_pad_read()
#define SG_PAD_WDT() do { if (WDT->CTRL.reg & WDT_CTRL_ENABLE) { WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY; } } while (0)
#endif

/* Frame pacing: the game itself keeps its speed, a presentation late by less
 * than a frame is skipped (at most max_skip in a row) and the deadline stays
 * put, so the debt is paid back. */
#define SG_PACE_FRAME(deadline, skip_video, skipped_run, max_skip) do { \
        (deadline) += SG_FRAME_US; \
        (skip_video) = (int32_t)(meta_micros() - (deadline)) > 0 && (skipped_run) < (max_skip); \
        (skipped_run) = (skip_video) ? (skipped_run) + 1 : 0; \
        while ((int32_t)(meta_micros() - (deadline)) < 0) meta_audio_idle(); \
        if ((int32_t)(meta_micros() - (deadline)) > SG_STALL_US) (deadline) = meta_micros(); \
    } while (0)

/* Calls the runtime makes from a region need stack below the guard (sync,
 * line rendering, audio catch-up, LCD/audio drivers and interrupts: about
 * 2 KiB, -fstack-usage); at least 1 KiB is left for nesting when RAM is
 * tight (cartridge RAM). */
#define SG_STACK_GUARD() do { \
        uintptr_t stack = (uintptr_t)&__StackTop - (uintptr_t)&end, reserve = 2560; \
        if (stack < reserve + 1024) reserve = stack > 2048 ? stack - 1024 : stack / 2; \
        sg_stack_limit = (uintptr_t)&end + reserve; \
    } while (0)

#endif
