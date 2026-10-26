#include "md.h"
#include <string.h>

static uint32_t read_rom32(uint32_t at) {
    if (at + 3 >= md_rom_size) return 0;
    return (uint32_t)md_rom[at] << 24 | (uint32_t)md_rom[at + 1] << 16 |
           (uint32_t)md_rom[at + 2] << 8 | md_rom[at + 3];
}
void md_reset(Md *m) {
    memset(m, 0, sizeof *m);
    m->a[7] = read_rom32(0);
    m->pc = read_rom32(4);
    m->vdp_reg[15] = 2;
    m->pad_th = 0x40;
}
static uint8_t pad_data(const Md *m) {
    uint8_t v = 0x80u;
    if (!(m->pad & MD_UP)) v |= 1u;
    if (!(m->pad & MD_DOWN)) v |= 2u;
    if (m->pad_th & 0x40u) {
        if (!(m->pad & MD_LEFT)) v |= 4u;
        if (!(m->pad & MD_RIGHT)) v |= 8u;
        if (!(m->pad & MD_B)) v |= 16u;
        if (!(m->pad & MD_C)) v |= 32u;
        v |= 64u;
    } else {
        if (!(m->pad & MD_A)) v |= 16u;
        if (!(m->pad & MD_START)) v |= 32u;
    }
    return v;
}
uint8_t md_read8(Md *m, uint32_t addr) {
    addr &= 0xffffffu;
    if (addr == 0xa10001u) return 0x80u; /* export/NTSC hardware version */
    if (addr == 0xa10003u) return pad_data(m);
    if (addr == 0xa10009u) return m->pad_control;
    if (addr < md_rom_size) return md_rom[addr];
#ifdef MD_FULL_VDP
    if (addr >= 0xff0000u) return m->ram[addr & 65535u];
#else
    if (addr >= 0xfff000u) return m->ram[addr & 4095u];
#endif
    m->fault = 1;
    return 0xff;
}
uint16_t md_read16(Md *m, uint32_t addr) {
    if (addr & 1u) { m->fault = 1; return 0xffff; }
    return (uint16_t)(md_read8(m, addr) << 8 | md_read8(m, addr + 1));
}
uint32_t md_read32(Md *m, uint32_t addr) {
    return (uint32_t)md_read16(m, addr) << 16 | md_read16(m, addr + 2);
}
void md_write8(Md *m, uint32_t addr, uint8_t value) {
    addr &= 0xffffffu;
    if (addr == 0xa10003u) { m->pad_th = value & 0x40u; return; }
    if (addr == 0xa10009u) { m->pad_control = value; return; }
#ifdef MD_FULL_VDP
    if (addr >= 0xff0000u) { m->ram[addr & 65535u] = value; return; }
#else
    if (addr >= 0xfff000u) { m->ram[addr & 4095u] = value; return; }
#endif
    m->fault = 1;
}
static void vdp_control(Md *m, uint16_t value) {
    if ((value & 0xc000u) == 0x8000u) {
        unsigned reg = (value >> 8) & 31u;
        if (reg < 24) m->vdp_reg[reg] = value & 255u;
        m->vdp_pending = 0;
        return;
    }
    if (!m->vdp_pending) { m->vdp_first = value; m->vdp_pending = 1; return; }
    m->vdp_addr = (uint16_t)((m->vdp_first & 0x3fffu) | ((value & 3u) << 14));
    m->vdp_target = (uint8_t)(((m->vdp_first >> 14) & 3u) | ((value >> 2) & 0xcu));
    m->vdp_pending = 0;
}
void md_write16(Md *m, uint32_t addr, uint16_t value) {
    addr &= 0xffffffu;
    if (addr & 1u) { m->fault = 1; return; }
    if (addr == 0xc00004u || addr == 0xc00006u) { vdp_control(m, value); return; }
    if (addr == 0xc00000u || addr == 0xc00002u) {
        if (m->vdp_target == 3 && m->vdp_addr < 128u)
            m->cram[m->vdp_addr >> 1] = value;
#ifdef MD_FULL_VDP
        else if (m->vdp_target == 1) {
            m->vram[m->vdp_addr] = (uint8_t)(value >> 8);
            m->vram[(uint16_t)(m->vdp_addr + 1u)] = (uint8_t)value;
        }
#endif
        else m->fault = 1;
        m->vdp_addr = (uint16_t)(m->vdp_addr + m->vdp_reg[15]);
        return;
    }
#ifdef MD_FULL_VDP
    if (addr >= 0xff0000u) { md_write8(m, addr, value >> 8); md_write8(m, addr + 1, (uint8_t)value); return; }
#else
    if (addr >= 0xfff000u) { md_write8(m, addr, value >> 8); md_write8(m, addr + 1, (uint8_t)value); return; }
#endif
    m->fault = 1;
}
void md_write32(Md *m, uint32_t addr, uint32_t value) {
    md_write16(m, addr, value >> 16);
    if (!m->fault) md_write16(m, addr + 2, (uint16_t)value);
}
void md_set_word(Md *m, unsigned n, uint16_t v) {
    m->d[n] = (m->d[n] & 0xffff0000u) | v;
    m->z = v == 0;
}
void md_set_long(Md *m, unsigned n, uint32_t v) { m->d[n] = v; m->z = v == 0; }
uint16_t md_background_rgb565(const Md *m) {
    const uint16_t c = m->cram[m->vdp_reg[7] & 63u];
    const unsigned r = (c & 0x0eu) >> 1, g = (c & 0xe0u) >> 5, b = (c & 0xe00u) >> 9;
    return (uint16_t)((r * 31u / 7u) << 11 | (g * 63u / 7u) << 5 | (b * 31u / 7u));
}
/* Plane A, 32x28 visible tiles. The first PC renderer deliberately leaves
 * scrolling, window, sprites, shadow/highlight and per-line VDP timing for
 * later; a zero pixel exposes the backdrop colour. */
void md_render_rgb565(const Md *m, uint16_t *pixels, unsigned stride) {
    const uint16_t backdrop = md_background_rgb565(m);
    for (unsigned y = 0; y < 224; ++y)
        for (unsigned x = 0; x < 320; ++x) pixels[y * stride + x] = backdrop;
#ifdef MD_FULL_VDP
    if (!(m->vdp_reg[1] & 0x40u)) return;
    const unsigned base = (m->vdp_reg[2] & 0x38u) << 10;
    const unsigned width = 32u << (m->vdp_reg[16] & 3u);
    const unsigned height = 32u << ((m->vdp_reg[16] >> 4) & 3u);
    for (unsigned y = 0; y < 224; ++y) {
        for (unsigned x = 0; x < 320; ++x) {
            const unsigned entry_addr = (base + 2u * (((y >> 3) % height) * width + ((x >> 3) % width))) & 65535u;
            const uint16_t entry = (uint16_t)(m->vram[entry_addr] << 8 |
                                               m->vram[(entry_addr + 1u) & 65535u]);
            const unsigned row = entry & 0x1000u ? 7u - (y & 7u) : y & 7u;
            const unsigned col = entry & 0x0800u ? 7u - (x & 7u) : x & 7u;
            const unsigned pattern = ((entry & 0x07ffu) * 32u + row * 4u + (col >> 1)) & 65535u;
            const unsigned nibble = col & 1u ? m->vram[pattern] & 15u : m->vram[pattern] >> 4;
            if (nibble) {
                const unsigned palette = ((entry >> 9) & 0x30u) | nibble;
                const uint16_t c = m->cram[palette];
                const unsigned r = (c & 0x0eu) >> 1, g = (c & 0xe0u) >> 5, b = (c & 0xe00u) >> 9;
                pixels[y * stride + x] = (uint16_t)((r * 31u / 7u) << 11 |
                    (g * 63u / 7u) << 5 | (b * 31u / 7u));
            }
        }
    }
#endif
}
uint32_t md_hash(const Md *m) {
    uint32_t h = 2166136261u;
    const uint8_t *parts[] = {(const uint8_t *)m->d, (const uint8_t *)m->a,
                              (const uint8_t *)m->ram, (const uint8_t *)m->cram,
#ifdef MD_FULL_VDP
                              (const uint8_t *)m->vram,
#endif
    };
    const unsigned sizes[] = {sizeof m->d, sizeof m->a, sizeof m->ram, sizeof m->cram,
#ifdef MD_FULL_VDP
                              sizeof m->vram,
#endif
    };
    for (unsigned j = 0; j < sizeof sizes / sizeof sizes[0]; ++j)
        for (unsigned i = 0; i < sizes[j]; ++i) h = (h ^ parts[j][i]) * 16777619u;
    for (unsigned i = 0; i < 4; ++i) h = (h ^ (m->pc >> (8 * i) & 255u)) * 16777619u;
    h = (h ^ m->pad) * 16777619u;
    h = (h ^ m->pad_th) * 16777619u;
    h = (h ^ m->pad_control) * 16777619u;
    return h;
}

/* Reference path for the supported opcodes.  It reads the ROM at run time;
 * md_code.c is generated from a static walk and does not decode at run time. */
void md_run_interp(Md *m, uint32_t steps) {
    while (steps-- && !m->fault) {
        const uint32_t pc = m->pc;
        const uint16_t op = md_read16(m, pc);
        uint32_t next = pc + 2;
        if (op == 0x4e71) {}
        else if (op == 0x33fc || op == 0x23fc) {
            unsigned size = op == 0x33fc ? 2 : 4;
            uint32_t value = size == 2 ? md_read16(m, next) : md_read32(m, next);
            next += size;
            uint32_t addr = md_read32(m, next); next += 4;
            if (size == 2) md_write16(m, addr, (uint16_t)value);
            else md_write32(m, addr, value);
        } else if ((op & 0xfff8u) == 0x33c0u) {
            uint32_t addr = md_read32(m, next); next += 4;
            md_write16(m, addr, (uint16_t)m->d[op & 7u]);
        } else if ((op & 0xf100u) == 0x7000u) {
            md_set_long(m, (op >> 9) & 7u, (uint32_t)(int32_t)(int8_t)op);
        } else if ((op & 0xfff8u) == 0x0640u) {
            unsigned n = op & 7u;
            md_set_word(m, n, (uint16_t)(m->d[n] + md_read16(m, next))); next += 2;
        } else if ((op & 0xf1f8u) == 0x5040u) {
            unsigned n = op & 7u, count = (op >> 9) & 7u;
            md_set_word(m, n, (uint16_t)(m->d[n] + (count ? count : 8)));
        } else if ((op & 0xfff8u) == 0x0c40u) {
            m->z = (uint16_t)m->d[op & 7u] == md_read16(m, next); next += 2;
        } else if ((op & 0xf000u) == 0x6000u && ((op >> 8) & 15u) <= 7u) {
            unsigned cc = (op >> 8) & 15u;
            int32_t disp = (int8_t)op;
            if (!(op & 255u)) { disp = (int16_t)md_read16(m, next); next += 2; }
            if (cc == 0 || (cc == 6 && !m->z) || (cc == 7 && m->z)) next = pc + 2 + disp;
            else if (cc != 6 && cc != 7) m->fault = 2;
        } else m->fault = 2;
        m->pc = next;
        m->instructions++;
    }
}
