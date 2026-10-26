#include "md_full.h"
#include <math.h>
#include <string.h>

enum { AUDIO_RATE = 44100 };
static uint8_t zread(void *user, uint16_t addr) {
    MdFull *m = (MdFull *)user;
    if (addr < 0x4000u) return m->zram[addr & 0x1fffu];
    if (addr < 0x6000u) return 0; /* YM status: no busy/timer yet */
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a < md_rom_size) return md_rom[a];
        if (a >= 0xff0000u) return m->ram[a & 0xffffu];
    }
    return 0xff;
}
static void zwrite(void *user, uint16_t addr, uint8_t value) {
    MdFull *m = (MdFull *)user;
    if (addr < 0x4000u) { m->zram[addr & 0x1fffu] = value; return; }
    if (addr < 0x6000u) {
        const unsigned bank = (addr >> 1) & 1u;
        if (!(addr & 1u)) m->ym_addr[bank] = value;
        else {
            const uint8_t reg = m->ym_addr[bank];
            m->ym_reg[bank][reg] = value;
            if (bank == 0 && reg == 0x28u) {
                const unsigned ch = (value & 3u) + ((value & 4u) ? 3u : 0u);
                if (ch < 6u) m->ym_key[ch] = value >> 4;
            }
        }
        return;
    }
    if (addr < 0x6100u) {
        m->z80_bank = ((m->z80_bank >> 1) | ((uint32_t)(value & 1u) << 23)) & 0xff8000u;
        return;
    }
    if (addr == 0x7f11u) {
        if (value & 0x80u) m->psg_latch = (value >> 4) & 7u;
        const unsigned r = m->psg_latch, ch = r >> 1;
        if (r & 1u) m->psg_volume[ch] = value & 15u;
        else if (ch == 3u) { m->psg_noise = value & 7u; m->psg_noise_lfsr = 0x8000u; }
        else if (value & 0x80u) m->psg_tone[ch] = (m->psg_tone[ch] & 0x3f0u) | (value & 15u);
        else m->psg_tone[ch] = (m->psg_tone[ch] & 15u) | ((value & 63u) << 4);
        return;
    }
    if (addr >= 0x8000u) {
        const uint32_t a = (m->z80_bank | (addr & 0x7fffu)) & 0xffffffu;
        if (a >= 0xff0000u) m->ram[a & 0xffffu] = value;
        else if ((a & 0xffffe0u) == 0xc00000u && (a & 0x1fu) == 0x11u) zwrite(user, 0x7f11u, value);
    }
}
static uint8_t zin(void *user, uint16_t port) { (void)user; (void)port; return 0xff; }
static void zout(void *user, uint16_t port, uint8_t value) { (void)user; (void)port; (void)value; }

void md_full_audio_reset(MdFull *m) {
    m->sound_cpu.user = m;
    m->sound_cpu.read = zread;
    m->sound_cpu.write = zwrite;
    m->sound_cpu.in = zin;
    m->sound_cpu.out = zout;
    z80_reset(&m->sound_cpu);
    m->z80_reset = 0;
    m->psg_noise_lfsr = 0x8000u;
    for (unsigned i = 0; i < 4; ++i) m->psg_volume[i] = 15;
    m->audio_hash = 2166136261u;
}

void md_full_sound_write(MdFull *m, uint32_t addr, uint8_t value) { zwrite(m, (uint16_t)addr, value); }
uint8_t md_full_sound_read(MdFull *m, uint32_t addr) { return zread(m, (uint16_t)addr); }

void md_full_sound_frame(MdFull *m) {
    /* 68000/7 and Z80/15 derive from the same master clock. A frame has
     * 127856 68000 cycles; run the Z80 at their ratio, carrying overshoot. */
    const uint64_t target = (uint64_t)m->frames * 127856u * 7u / 15u;
    if (!m->z80_reset || m->z80_bus || m->sound_cpu.fault) {
        m->sound_cpu.cycles = target;
        return;
    }
    const uint64_t irq_end = m->sound_cpu.cycles + 32u;
    m->sound_cpu.irq = 1;
    while (m->sound_cpu.cycles < target && !m->sound_cpu.fault) {
        if (m->sound_cpu.cycles >= irq_end) m->sound_cpu.irq = 0;
        z80_step(&m->sound_cpu);
    }
    m->sound_cpu.irq = 0;
}

static double operator_sample(MdFull *m, unsigned ch, unsigned op, double freq, double modulation) {
    static double level[128];
    static int levels_ready;
    if (!levels_ready) {
        for (unsigned i = 0; i < 128; ++i) level[i] = pow(10.0, -(double)i / 40.0);
        levels_ready = 1;
    }
    const unsigned bank = ch / 3u, lane = ch % 3u;
    const unsigned slot = (unsigned[]){0, 2, 1, 3}[op];
    const unsigned at = slot * 4u + lane;
    const uint8_t mul = m->ym_reg[bank][0x30u + at] & 15u;
    const uint8_t tl = m->ym_reg[bank][0x40u + at] & 127u;
    const uint8_t attack = m->ym_reg[bank][0x50u + at] & 31u;
    const uint8_t release = m->ym_reg[bank][0x80u + at] & 15u;
    const double target = (m->ym_key[ch] & (1u << op)) ? 1.0 : 0.0;
    double *env = &m->ym_env[ch][op];
    if (!target && *env < 0.00001) return 0;
    *env += (target - *env) * (target ? (0.0005 + attack * 0.0005) : (0.00003 + release * 0.00003));
    m->ym_phase[ch][op] += 6.283185307179586 * freq * (mul ? mul : 0.5) / AUDIO_RATE;
    if (m->ym_phase[ch][op] >= 6.283185307179586) m->ym_phase[ch][op] = fmod(m->ym_phase[ch][op], 6.283185307179586);
    return sin(m->ym_phase[ch][op] + modulation) * *env * level[tl];
}

void md_full_audio_render(MdFull *m, int16_t *samples, unsigned count) {
    static const double psg_amp[16] = {1, .794, .631, .501, .398, .316, .251, .200,
        .158, .126, .100, .079, .063, .050, .040, 0};
    for (unsigned i = 0; i < count; ++i) {
        double mix = 0;
        for (unsigned ch = 0; ch < 3; ++ch) {
            const unsigned period = m->psg_tone[ch] ? m->psg_tone[ch] : 1u;
            m->psg_phase[ch] += 3579545.0 / (32.0 * period * AUDIO_RATE);
            if (m->psg_phase[ch] >= 1.0) m->psg_phase[ch] -= floor(m->psg_phase[ch]);
            mix += (m->psg_phase[ch] < .5 ? 1.0 : -1.0) * psg_amp[m->psg_volume[ch]] * .10;
        }
        unsigned noise_period = (m->psg_noise & 3u) == 3u ?
            (m->psg_tone[2] ? m->psg_tone[2] : 1u) : 16u << (m->psg_noise & 3u);
        m->psg_phase[3] += 3579545.0 / (16.0 * noise_period * AUDIO_RATE);
        if (m->psg_phase[3] >= 1.0) {
            m->psg_phase[3] -= floor(m->psg_phase[3]);
            const unsigned tap = m->psg_noise & 4u ? ((m->psg_noise_lfsr ^ (m->psg_noise_lfsr >> 3)) & 1u) : (m->psg_noise_lfsr & 1u);
            m->psg_noise_lfsr = (m->psg_noise_lfsr >> 1) | (tap << 15);
        }
        mix += (m->psg_noise_lfsr & 1u ? 1.0 : -1.0) * psg_amp[m->psg_volume[3]] * .07;
        for (unsigned ch = 0; ch < 6; ++ch) {
            const unsigned bank = ch / 3u, lane = ch % 3u;
            const unsigned fnum = m->ym_reg[bank][0xa0u + lane] | ((m->ym_reg[bank][0xa4u + lane] & 7u) << 8);
            const unsigned block = (m->ym_reg[bank][0xa4u + lane] >> 3) & 7u;
            const double freq = fnum * (1u << block) * (7670454.0 / (144.0 * 1048576.0));
            const unsigned algorithm = m->ym_reg[bank][0xb0u + lane] & 7u;
            const double a = operator_sample(m, ch, 0, freq, 0);
            const double b = operator_sample(m, ch, 1, freq, a * 3.0);
            const double c = operator_sample(m, ch, 2, freq, (algorithm >= 4 ? 0 : b * 3.0));
            const double d = operator_sample(m, ch, 3, freq, (algorithm >= 6 ? 0 : c * 3.0));
            double channel = d + (algorithm >= 4 ? c : 0) + (algorithm >= 6 ? b : 0) + (algorithm == 7 ? a : 0);
            if (ch == 5 && (m->ym_reg[0][0x2b] & 0x80u)) channel = ((int)m->ym_reg[0][0x2a] - 128) / 128.0;
            mix += channel * .055;
        }
        int sample = (int)(mix * 32767.0);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        samples[i] = (int16_t)sample;
        m->audio_hash = (m->audio_hash ^ (uint8_t)sample) * 16777619u;
        m->audio_hash = (m->audio_hash ^ (uint8_t)(sample >> 8)) * 16777619u;
    }
}
