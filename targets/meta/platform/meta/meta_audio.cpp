/*
 * Gamebuino META audio: gbl APU samples played through the DAC (PA02).
 *
 * TC4 runs from generator 4, 48 MHz at either CPU clock (meta_clock.cpp),
 * and wraps at 48 MHz / 2177 (about 22049 Hz).  The ring is refilled once
 * per emulated frame.
 *
 * META_AUDIO_DMA=1 (gbl_profile.h): each wrap triggers one DMAC beat (channel 1)
 * that copies the next ring entry, already a DAC value, to the DAC, with no
 * CPU work.  The interrupt it replaces cost about 65 cycles per sample,
 * some 3% of the CPU.  Each frame's samples get a descriptor (two at the
 * ring's wrap) linked from the previous frame's, and the last one links to
 * an invalid descriptor: when emulation falls behind, the DMAC stops there
 * and the DAC holds the last sample, as the interrupt did; the next frame
 * sees the channel stopped and restarts it after LATENCY samples.  The
 * DMAC position is not readable reliably, so the samples played while the
 * channel runs are derived from meta_micros() and only bound how much can
 * be queued.
 *
 * META_AUDIO_DMA=0: the TC4 interrupt writes the next sample to the DAC.
 * On underrun the last sample is held; on overrun the newest samples are
 * dropped.
 */
#include "meta_audio.h"
#include "meta_clock.h"
#include "meta_dma.h"
#include "meta_target.h"

#include "meta_hw.h"

#include <string.h>

extern "C" {
#include "gbl.h"
}

#define AUDIO_TOP 2177u
#define AUDIO_RATE (48000000u / AUDIO_TOP)
#define DAC_MID 512u
/* 8-bit APU sample (128 = silence) to DAC value around mid: volume 4 (the
 * default) is half scale, 8 the DAC's whole range, 0 silence. */
static uint8_t volume = 4;
#define DAC_VALUE(s) ((uint16_t)(DAC_MID + ((((int32_t)(s) - 128) * (int32_t)volume) >> 1)))

void meta_audio_set_volume(uint8_t v) { volume = v > 8 ? 8 : v; }
uint8_t meta_audio_volume(void) { return volume; }

static uint32_t frac;

static unsigned frame_samples(void) { return meta_audio_frame_samples(AUDIO_RATE, &frac); }

/* Still advance the APU when there is no room (output stalled) so length
 * counters and envelopes keep time. */
static void drop_samples(unsigned n) {
    uint8_t scratch[64];
    while (n) {
        unsigned k = n > sizeof(scratch) ? sizeof(scratch) : n;
        gbl_apu_render(scratch, k);
        n -= k;
    }
}

/* PA02 to the DAC (function B), referenced to AVCC with its output on the
 * pin as the Arduino core set it, holding mid-scale.  CTRLB can only be
 * written while the DAC is off (the loader may have left it on). */
static void dac_start(void) {
    PORT->Group[0].PMUX[1].bit.PMUXE = PORT_PMUX_PMUXE_B_Val;
    PORT->Group[0].PINCFG[2].reg |= PORT_PINCFG_PMUXEN;
    DAC->CTRLA.bit.ENABLE = 0;
    while (DAC->STATUS.bit.SYNCBUSY) {}
    DAC->CTRLB.reg = DAC_CTRLB_REFSEL_AVCC | DAC_CTRLB_EOEN;
    DAC->DATA.reg = DAC_MID;
    while (DAC->STATUS.bit.SYNCBUSY) {}
    DAC->CTRLA.bit.ENABLE = 1;
    while (DAC->STATUS.bit.SYNCBUSY) {}
}

static void timer_start(bool irq) {
    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK4 | GCLK_CLKCTRL_ID_TC4_TC5;
    while (GCLK->STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CTRLA.reg &= (uint16_t)~TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_WAVEGEN_MFRQ | TC_CTRLA_PRESCALER_DIV1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CC[0].reg = AUDIO_TOP - 1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    if (irq) {
        TC4->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
        NVIC_SetPriority(TC4_IRQn, 0);
        NVIC_EnableIRQ(TC4_IRQn);
    }
    TC4->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
}

static void timer_stop(void) {
    TC4->COUNT16.CTRLA.reg &= (uint16_t)~TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
}

#if META_AUDIO_DMA

#define RING 768u    /* samples (1.5 KiB), about two frames */
#define LATENCY 256u /* samples queued before the first one after a restart */
#define GUARD 8u     /* margin around the estimated DMA position */
#define MIN_CHUNK 256u /* fewer samples than this are dropped, not queued */
#define SLOTS 8u     /* linked descriptors: at most three chunks of MIN_CHUNK
                      * are queued, two descriptors each, plus the end */

static uint16_t ring[RING] __attribute__((aligned(4)));
static meta_dma_descriptor slot[SLOTS] __attribute__((aligned(16)));
static unsigned next_slot;       /* invalid descriptor the queued chain ends on */
static uint32_t written, played; /* samples, counted from the start */
static unsigned head;            /* written % RING, kept without dividing */
static uint32_t last_us, cycles;

/* CHID selects the channel the CHxxx registers address; the display's DMAC
 * interrupt changes it. */
static bool channel_running(void) {
    __disable_irq();
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_AUDIO);
    /* Stopped at an invalid descriptor: disabled, or suspended with a fetch
     * error, depending on the DMAC's reading of it. */
    bool on = (DMAC->CHCTRLA.reg & DMAC_CHCTRLA_ENABLE) && !(DMAC->CHSTATUS.reg & DMAC_CHSTATUS_FERR) &&
              !(DMAC->CHINTFLAG.reg & (DMAC_CHINTFLAG_SUSP | DMAC_CHINTFLAG_TERR));
    __enable_irq();
    return on;
}

static void channel_start(const meta_dma_descriptor *first) {
    meta_dma_desc[META_DMA_AUDIO] = *first;
    __disable_irq();
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_AUDIO);
    /* A chain that ran into its invalid descriptor may leave the channel
     * enabled (suspended with a fetch error): disable it before the reset.
     * SWRST written to an enabled channel faulted on device (2026-10-16:
     * HardFault on this write, 400 ms into every game). */
    DMAC->CHCTRLA.reg = 0;
    while (DMAC->CHCTRLA.reg & DMAC_CHCTRLA_ENABLE) {}
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;
    while (DMAC->CHCTRLA.reg & DMAC_CHCTRLA_SWRST) {}
    DMAC->CHCTRLB.reg = DMAC_CHCTRLB_LVL(1) | DMAC_CHCTRLB_TRIGSRC(TC4_DMAC_ID_OVF) |
                        DMAC_CHCTRLB_TRIGACT_BEAT;
    DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
    __enable_irq();
}

/* Descriptor for ring[start .. start + count) linked to next.  VALID is
 * written last: the DMAC may fetch it while it is being filled, and then
 * stops on the invalid descriptor instead of reading a partial one. */
static void describe(meta_dma_descriptor *d, unsigned start, unsigned count,
                     const meta_dma_descriptor *next) {
    d->btcnt = (uint16_t)count;
    d->srcaddr = (uint32_t)&ring[start] + 2u * count;
    d->dstaddr = (uint32_t)&DAC->DATA.reg;
    d->descaddr = (uint32_t)next;
    *(volatile uint16_t *)&d->btctrl = DMAC_BTCTRL_VALID | DMAC_BTCTRL_BEATSIZE_HWORD | DMAC_BTCTRL_SRCINC;
}

void meta_audio_init(void) {
    dac_start();
    for (unsigned i = 0; i < RING; i++) ring[i] = DAC_MID;
    gbl_apu_set_rate(AUDIO_RATE);
    meta_dma_init();
    timer_start(false); /* the channel starts with the first frame */
    last_us = meta_micros();
}

void meta_audio_frame(void) {
    unsigned n = frame_samples();
    uint32_t now = meta_micros(), dt = now - last_us;
    last_us = now;
    unsigned prefix = 0;
    if (!channel_running()) {
        /* Not started, or the DMA reached the end of what was queued (the
         * DAC holds the last sample): restart after LATENCY samples of it. */
        played = written;
        cycles = 0;
        prefix = LATENCY;
    } else {
        /* Samples taken since the last call (TC4 counts 48 MHz). */
        if (dt > 1000000u) dt = 1000000u;
        cycles += dt * 48u;
        played += cycles / AUDIO_TOP;
        cycles %= AUDIO_TOP;
        if ((int32_t)(written - played) < 0) played = written;
    }
    unsigned queued = written - played;
    unsigned room = queued < RING - GUARD ? RING - GUARD - queued : 0;
    if (room < prefix + MIN_CHUNK) { /* nearly full: catching up */
        drop_samples(n);
        return;
    }
    if (n > room - prefix) {
        drop_samples(n - (room - prefix));
        n = room - prefix;
    }
    unsigned total = prefix + n;

    unsigned start = head;
    uint16_t last = ring[head ? head - 1u : RING - 1u];
    for (unsigned i = 0; i < prefix; i++) {
        ring[head] = last;
        if (++head == RING) head = 0;
    }
    uint8_t buf[64];
    while (n) {
        unsigned k = n > sizeof(buf) ? sizeof(buf) : n;
        gbl_apu_render(buf, k);
        n -= k;
        for (unsigned i = 0; i < k; i++) {
            ring[head] = DAC_VALUE(buf[i]);
            if (++head == RING) head = 0;
        }
    }
    written += total;

    /* Chain: first (the slot the previous chain ends on) [-> second, at the
     * ring's wrap] -> a new invalid slot. */
    meta_dma_descriptor *first = &slot[next_slot];
    unsigned part = RING - start;
    unsigned extra = total > part ? 1u : 0u;
    meta_dma_descriptor *second = &slot[(next_slot + 1u) % SLOTS];
    meta_dma_descriptor *end = &slot[(next_slot + 1u + extra) % SLOTS];
    end->btctrl = 0;
    if (extra) {
        describe(second, 0, total - part, end);
        describe(first, start, part, second);
    } else {
        describe(first, start, total, end);
    }
    if (!channel_running()) channel_start(first);
    next_slot = (next_slot + 1u + extra) % SLOTS;
}

void meta_audio_idle(void) {}

void meta_audio_stop(void) {
    __disable_irq();
    DMAC->CHID.reg = DMAC_CHID_ID(META_DMA_AUDIO);
    DMAC->CHCTRLA.reg = 0;
    while (DMAC->CHCTRLA.reg & DMAC_CHCTRLA_ENABLE) {}
    __enable_irq();
    timer_stop();
    DAC->DATA.reg = DAC_MID; /* mid-scale: no click */
}

void meta_audio_pause(int on) {
    static bool paused;
    if (on == paused) return;
    paused = on;
    if (on) {
        meta_audio_stop(); /* channel off: the next frame restarts it */
        return;
    }
    timer_start(false); /* the channel restarts at the next frame */
}

#else /* META_AUDIO_DMA */

#define RING 1024u

static uint8_t ring[RING];
static volatile uint16_t ring_tail;
static uint16_t ring_head;

extern "C" void TC4_Handler(void) {
    TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
    uint16_t t = ring_tail;
    if (t != ring_head) {
        DAC->DATA.reg = DAC_VALUE(ring[t]);
        ring_tail = (uint16_t)((t + 1) & (RING - 1));
    }
}

void meta_audio_init(void) {
    dac_start();
    memset(ring, 128, sizeof(ring));
    gbl_apu_set_rate(AUDIO_RATE);
    timer_start(true);
}

void meta_audio_frame(void) {
    unsigned n = frame_samples();
    uint16_t tail = ring_tail;
    unsigned used = (unsigned)((ring_head - tail) & (RING - 1));
    unsigned room = RING - 1u - used;
    if (n > room) {
        drop_samples(n - room);
        n = room;
    }
    while (n) {
        unsigned chunk = RING - ring_head;
        if (chunk > n) chunk = n;
        gbl_apu_render(&ring[ring_head], chunk);
        ring_head = (uint16_t)((ring_head + chunk) & (RING - 1));
        n -= chunk;
    }
}

void meta_audio_idle(void) {}

void meta_audio_stop(void) {
    NVIC_DisableIRQ(TC4_IRQn);
    TC4->COUNT16.INTENCLR.reg = TC_INTENCLR_MC0;
    timer_stop();
    DAC->DATA.reg = DAC_MID; /* mid-scale: no click */
}

void meta_audio_pause(int on) {
    static bool paused;
    if (on == paused) return;
    paused = on;
    if (on) meta_audio_stop();
    else timer_start(true); /* INTENSET and NVIC back on */
}

#endif /* META_AUDIO_DMA */
