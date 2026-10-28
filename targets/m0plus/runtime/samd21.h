#ifndef M0BOY_SAMD21_H
#define M0BOY_SAMD21_H
#include "m0_machine.h"
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Gamebuino META board around the recompiled Cortex-M0+ (SAMD21G18):
 * SysTick/NVIC (m0_machine), TC4/TC5, DMAC, SERCOM4 (ST7735 + buttons + SD
 * card), PORT (APB and IOBUS), DAC, ADC, SYSCTRL/GCLK, NVMCTRL.  Peripherals
 * are lazy: their state is brought up to the CPU's cycle count when the
 * guest touches a register or when the next event falls due.  The surface
 * is the one the guest profile reports (m0plus-profile.txt). */
enum { SAMD21_W = 160, SAMD21_H = 128, SAMD21_DMA_CHANNELS = 12 };
enum { SAMD21_CPU_HZ = 48000000u };

typedef struct Samd21 Samd21;
typedef int (*Samd21SdRead)(uint32_t sector, uint8_t *out512);
typedef int (*Samd21SdWrite)(uint32_t sector, const uint8_t *in512);

struct Samd21Tc {
    uint8_t enabled, intEn, intFlag;
    uint16_t top;
    uint32_t ctrlA;
    uint64_t next;                      /* cycle of the next overflow */
};

struct Samd21 {
    M0Machine m;                        /* first: the CPU's board pointer */
    uint64_t next_event;                /* earliest TC/DMA deadline */
    uint64_t frame_end;
    uint32_t frac;
    struct Samd21Tc tc[2];
    /* PORT */
    uint32_t pa_out, pb_out, pa_dir, pb_dir;
    /* clocks */
    uint8_t dpll_on; uint16_t dpll_ldr; uint8_t gclk6_pll; uint32_t gclk_gen[16];
    /* SERCOM4 SPI */
    uint32_t spi_baud;
    uint8_t ser_data;
    struct { uint8_t d; uint64_t done; } rx[2]; int rx_n;
    uint64_t spi_last, spi_prev;
    uint8_t buttons;                    /* active low, lib order */
    /* DMAC */
    uint32_t dmac_base, dmac_wrb, dmac_chid;
    uint8_t dma_on[SAMD21_DMA_CHANNELS], dma_trig[SAMD21_DMA_CHANNELS];
    uint8_t dma_flag[SAMD21_DMA_CHANNELS], dma_en[SAMD21_DMA_CHANNELS];
    uint8_t dma_ferr[SAMD21_DMA_CHANNELS], dma_skip[SAMD21_DMA_CHANNELS];
    uint16_t dma_ctrl[SAMD21_DMA_CHANNELS], dma_cnt[SAMD21_DMA_CHANNELS], dma_idx[SAMD21_DMA_CHANNELS];
    uint32_t dma_src[SAMD21_DMA_CHANNELS], dma_dst[SAMD21_DMA_CHANNELS];
    uint32_t dma_next[SAMD21_DMA_CHANNELS], dma_resume[SAMD21_DMA_CHANNELS];
    int spi_dma;                        /* channel feeding SERCOM4, -1 */
    uint32_t spi_beat_thirds, spi_acc;  /* beat length and phase, in 1/3 cycle */
    uint64_t spi_from;
    int in_spi_dma;
    /* ST7735 */
    int lcd_xs, lcd_xe, lcd_ys, lcd_ye, lcd_x, lcd_y, lcd_arg, lcd_cmd, lcd_tmp;
    uint8_t lcd_bgr, lcd_colmod, lcd_pix[3];
    uint16_t *pix;
    /* DAC audio: level changes of the frame being run */
    uint16_t dac;
    struct { uint32_t t; int16_t v; } lev[4096]; unsigned lev_n; int16_t lev_cur;
    uint64_t audio_base;                /* cycle count of the frame's start */
    uint64_t audio_t;                   /* read cursor in this frame, cycles x 256 */
    uint32_t audio_step; unsigned audio_k; int16_t audio_cur;
    int audio_hp;                       /* one-pole high-pass state */
    uint32_t adc_prng;
    /* SD card (SPI mode) */
    Samd21SdRead sd_read; Samd21SdWrite sd_write;
    uint8_t sd_out[1100]; int sd_len, sd_pos;
    uint8_t sd_cmd[6]; int sd_cmd_n;
    uint8_t sd_wbuf[515]; int sd_wn, sd_writing; uint32_t sd_wlba;
    int sd_multi; uint32_t sd_lba; int sd_init;
    /* profile: per-instruction hook (interpreter run) and the MMIO table */
    void (*step_hook)(void *ctx, uint32_t pc, uint32_t cycles);
    void *hook_ctx;
    struct Samd21Mmio *mmio;
    /* poll-loop skipping */
    uint32_t idle_pc; uint64_t idle_at, idle_period; unsigned idle_run;
    uint64_t idle_skipped;
    struct Samd21Loop { uint32_t pc, stores, flags, r[16]; uint64_t at, period; unsigned run; } loop;
};

/* Register accesses of a run, for the profile (--profile): one entry per
 * (address, direction, size).  `known` is 0 where the board has no model
 * for that register: the profile lists those, a guest relying on one is
 * what the runtime still has to learn. */
struct Samd21Mmio {
    struct { uint32_t a, n, v[4], nv, pc[2]; uint8_t w, size, known; } e[1024];
    uint32_t pc;                        /* instruction being run */
};
int samd21_profile_start(Samd21 *);
void samd21_profile_mmio(const Samd21 *, FILE *out);
/* Cycles the translated code may run before anything can happen (a timer
 * or DMA deadline, SysTick, the end of the frame); 0 when an exception is
 * about to be taken.  Translated blocks run only inside this room. */
uint32_t samd21_room(const Samd21 *);
/* Skip whole iterations of a poll loop (see m0_native_idle). */
void samd21_idle(Samd21 *, uint32_t loop_pc, uint32_t polled_address);

/* Returns 0 on success.  `firmware` is placed at flash_base (0x4000). */
int samd21_init(Samd21 *, const uint8_t *flash, size_t flash_size, uint32_t flash_base,
                uint16_t *pixels);
void samd21_buttons(Samd21 *, uint8_t pressed);   /* bit set = pressed, lib order */
void samd21_sd(Samd21 *, Samd21SdRead, Samd21SdWrite);
/* Run the CPU one video frame (59.7275 Hz); native regions first, the
 * Thumb fallback otherwise. */
void samd21_frame(Samd21 *, int (*native_step)(M0Cpu *));
/* Unsigned 8-bit samples of the frame just run; may be called in pieces. */
void samd21_audio_rate(Samd21 *, unsigned hz);
void samd21_audio(Samd21 *, uint8_t *out, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
