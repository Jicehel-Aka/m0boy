/*
 * Gamebuino META audio: gbl APU samples played through the DAC (PA02).
 *
 * TC4 runs from the 48 MHz GCLK0 and interrupts at 48 MHz / 2177 (about
 * 22049 Hz); its handler writes the next ring-buffer sample to the DAC.  The
 * ring is refilled once per emulated frame.  On underrun the last sample is
 * held; on overrun the newest samples are dropped.
 */
#include "meta_audio.h"

#include <Arduino.h>

extern "C" {
#include "gbl.h"
}

#define AUDIO_TOP 2177u
#define AUDIO_RATE (48000000u / AUDIO_TOP)
#define RING 1024u

static uint8_t ring[RING];
static volatile uint16_t ring_tail;
static uint16_t ring_head;
static uint32_t frac;

extern "C" void TC4_Handler(void) {
    TC4->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
    uint16_t t = ring_tail;
    if (t != ring_head) {
        DAC->DATA.reg = (uint16_t)(256u + ((uint16_t)ring[t] << 1));
        ring_tail = (uint16_t)((t + 1) & (RING - 1));
    }
}

void meta_audio_init(void) {
    analogWriteResolution(10);
    analogWrite(A0, 512); /* enables the DAC on PA02 */
    memset(ring, 128, sizeof(ring));
    gbl_apu_set_rate(AUDIO_RATE);

    GCLK->CLKCTRL.reg = GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID_TC4_TC5;
    while (GCLK->STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CTRLA.reg &= (uint16_t)~TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_WAVEGEN_MFRQ | TC_CTRLA_PRESCALER_DIV1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.CC[0].reg = AUDIO_TOP - 1;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    TC4->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
    NVIC_SetPriority(TC4_IRQn, 0);
    NVIC_EnableIRQ(TC4_IRQn);
    TC4->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
}

void meta_audio_frame(void) {
    /* One DMG frame worth of samples: 70224 cycles at 4194304 Hz. */
    frac += AUDIO_RATE * 70224u;
    unsigned n = frac / 4194304u;
    frac %= 4194304u;
    uint16_t tail = ring_tail;
    unsigned used = (unsigned)((ring_head - tail) & (RING - 1));
    unsigned room = RING - 1u - used;
    if (n > room) {
        /* No room (output stalled): still advance the APU so length
         * counters and envelopes keep time. */
        uint8_t scratch[64];
        unsigned drop = n - room;
        while (drop) {
            unsigned k = drop > sizeof(scratch) ? sizeof(scratch) : drop;
            gbl_apu_render(scratch, k);
            drop -= k;
        }
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
    TC4->COUNT16.CTRLA.reg &= (uint16_t)~TC_CTRLA_ENABLE;
    while (TC4->COUNT16.STATUS.bit.SYNCBUSY) {}
    DAC->DATA.reg = 512; /* mid-scale: no click */
}
