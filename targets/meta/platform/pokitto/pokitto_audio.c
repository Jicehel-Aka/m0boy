/*
 * Pokitto audio: gbl APU samples on the 8-bit R2R DAC.
 *
 * Wiring and setup follow PokittoLib's PokittoHWSink: DAC bits 0-3 on
 * P1_28..31, bits 4-7 on P2_20..23, amplifier enable P1_17, CT32B0 match 1
 * interrupting at the sample rate (IRQ 18), hardware volume a digital
 * potentiometer at I2C address 0x2F on P0_4/P0_5 (written both ways round,
 * as the lib does for the two board variants).  The potentiometer is set
 * to the lib's level for its default volume; the menu's volume scales the
 * samples.  The ring holds finished DAC values and is refilled once per
 * emulated frame; on underrun the DAC keeps the last value, on overrun the
 * newest samples are dropped.
 */
#include "meta_audio.h"

#include <stddef.h>
#include <string.h>

#include "gbl.h"
#include "meta_clock.h"
#include "lpc11u6x.h"
#include "meta_target.h"
#include "pokitto_hw.h"

#define AUDIO_RATE 22050u
#define RING 1024u

/* POKITTO_AUDIO_ASM (gbl_profile.h): the interrupt in Thumb-1 from SRAM
 * (pokitto_audio_asm.S); 0 keeps the C one below, the reference. */

/* The queue, laid out for the assembly interrupt (tail at 0, head at 2, the
 * ring at 4), in the 2 KiB RAM1 bank (pokitto.ld). */
typedef struct {
    volatile uint16_t tail; /* next sample to play (interrupt) */
    uint16_t head;          /* next free slot (meta_audio_frame) */
    uint8_t ring[RING];
} audio_queue;
audio_queue pokitto_audio_q __attribute__((section(".bss_ram1.audio_q"), aligned(4)));
_Static_assert(offsetof(audio_queue, head) == 2 && offsetof(audio_queue, ring) == 4 && RING == 1024,
               "audio_queue layout used by pokitto_audio_asm.S");
#define ring pokitto_audio_q.ring
#define ring_tail pokitto_audio_q.tail
#define ring_head pokitto_audio_q.head
static uint8_t volume = 4; /* 0..8, 8 = the DAC's whole range */
static uint32_t frac;

#define DAC_VALUE(s) ((uint8_t)(128 + ((((int32_t)(s) - 128) * (int32_t)volume) >> 3)))

void meta_audio_set_volume(uint8_t v) { volume = v > 8 ? 8 : v; }
uint8_t meta_audio_volume(void) { return volume; }

/* Through SET/CLR, which need no MASK (MASK[2] belongs to the display's
 * MPIN writes, which this interrupt can preempt).  P2_23 goes last: the
 * emulator in tools/meta-emu-sdl latches a sample on a write to it. */
static inline void dac_write(uint32_t v) {
    GPIO_SET(1) = (v & 0xFu) << 28;
    GPIO_CLR(1) = (~v & 0xFu) << 28;
    GPIO_SET(2) = ((v >> 4) & 7u) << 20;
    GPIO_CLR(2) = ((~v >> 4) & 7u) << 20;
    if (v & 0x80u) GPIO_SET(2) = 1u << 23;
    else GPIO_CLR(2) = 1u << 23;
}

#if !POKITTO_AUDIO_ASM
void CT32B0_IRQHandler(void) {
    CT32B0_IR = 2u; /* match 1 */
    uint16_t t = ring_tail;
    if (t != ring_head) {
        dac_write(ring[t]);
        ring_tail = (uint16_t)((t + 1u) & (RING - 1u));
    }
}
#endif

/* ---- digital potentiometer (software I2C, open drain) ---------------------- */

static void i2c_wait(void) {
    for (volatile unsigned i = 0; i < 60u; i++) {}
}
/* Low: drive 0; high: release (pull-up). */
static void line(unsigned pin, int high) {
    if (high) GPIO_DIR(0) &= ~(1u << pin);
    else {
        GPIO_CLR(0) = 1u << pin;
        GPIO_DIR(0) |= 1u << pin;
    }
    i2c_wait();
}

static void i2c_write(unsigned scl, unsigned sda, uint8_t addr8, uint8_t value) {
    line(sda, 1);
    line(scl, 1);
    line(sda, 0); /* start */
    line(scl, 0);
    uint8_t bytes[2] = {addr8, value};
    for (unsigned b = 0; b < 2; b++) {
        for (int i = 7; i >= 0; i--) {
            line(sda, (bytes[b] >> i) & 1u);
            line(scl, 1);
            line(scl, 0);
        }
        line(sda, 1); /* acknowledge slot, not checked */
        line(scl, 1);
        line(scl, 0);
    }
    line(sda, 0); /* stop */
    line(scl, 1);
    line(sda, 1);
}

static void set_hw_volume(uint8_t level) {
    IOCON_SET(IOCON_PIO0(4), IOCON_FUNC(0), IOCON_MODE_NONE);
    IOCON_SET(IOCON_PIO0(5), IOCON_FUNC(0), IOCON_MODE_NONE);
    i2c_write(4, 5, 0x5E, level);
    i2c_write(5, 4, 0x5E, level);
}

/* ---- timer ------------------------------------------------------------------ */

void pokitto_audio_clock_changed(uint32_t hz) {
    if (!(SYSCON_SYSAHBCLKCTRL & AHB_CT32B0)) return;
    CT32B0_MR1 = (hz + AUDIO_RATE / 2u) / AUDIO_RATE;
    CT32B0_TC = 0;
}

void meta_audio_init(void) {
    for (unsigned n = 28; n <= 31; n++) IOCON_PIO1(n) = IOCON_DIGITAL;
    for (unsigned n = 20; n <= 23; n++) IOCON_PIO2(n) = IOCON_DIGITAL;
    GPIO_DIR(1) |= 0xFu << 28 | 1u << 17;
    GPIO_DIR(2) |= 0xFu << 20;
    dac_write(128);
    set_hw_volume(0x7F); /* PokittoLib: (227 >> 1) | 0xF for its default volume */
    GPIO_SET(1) = 1u << 17; /* amplifier on */

    memset(ring, 128, sizeof(ring));
    gbl_apu_set_rate(AUDIO_RATE);

    SYSCON_SYSAHBCLKCTRL |= AHB_CT32B0;
    CT32B0_TCR = 2u; /* reset */
    CT32B0_PR = 0;
    CT32B0_MCR = 3u << 3; /* match 1: interrupt and reset */
    pokitto_audio_clock_changed(meta_clock_mhz() * 1000000u);
    CT32B0_IR = 0x3Fu;
    CT32B0_TCR = 1u;
    NVIC_IPR(IRQ_CT32B0) &= ~(0xFFu << (8 * (IRQ_CT32B0 & 3))); /* highest priority */
    NVIC_ICPR = 1u << IRQ_CT32B0;
    NVIC_ISER = 1u << IRQ_CT32B0;
}

static unsigned frame_samples(void) { return meta_audio_frame_samples(AUDIO_RATE, &frac); }

void meta_audio_frame(void) {
    unsigned n = frame_samples();
    uint16_t tail = ring_tail;
    unsigned used = (unsigned)((ring_head - tail) & (RING - 1u));
    unsigned room = RING - 1u - used;
    uint8_t buf[64];
    while (n) {
        unsigned k = n > sizeof(buf) ? sizeof(buf) : n;
        gbl_apu_render(buf, k); /* also when there is no room: the APU keeps time */
        n -= k;
        for (unsigned i = 0; i < k && room; i++, room--) {
            ring[ring_head] = DAC_VALUE(buf[i]);
            ring_head = (uint16_t)((ring_head + 1u) & (RING - 1u));
        }
    }
}

void meta_audio_idle(void) {}

void meta_audio_stop(void) {
    NVIC_ICER = 1u << IRQ_CT32B0;
    CT32B0_TCR = 0;
    dac_write(128);
    GPIO_CLR(1) = 1u << 17;
}
