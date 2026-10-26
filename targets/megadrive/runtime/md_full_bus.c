#include "md_full.h"
#include <string.h>

static uint32_t rom32(uint32_t a) {
    return a + 3 < md_rom_size ? (uint32_t)md_rom[a] << 24 |
        (uint32_t)md_rom[a + 1] << 16 | (uint32_t)md_rom[a + 2] << 8 | md_rom[a + 3] : 0;
}

void md_full_reset(MdFull *m) {
    memset(m, 0, sizeof *m);
    m->a[7] = rom32(0);
    m->pc = rom32(4);
    m->sr = 0x2700;
    m->vdp_reg[15] = 2;
    m->pad_th = 0x40;
    md_full_audio_reset(m);
}

static uint8_t pad_read(MdFull *m) {
    const uint8_t p = m->pad;
    uint8_t v = 0x80;
    if (!(p & 1)) v |= 1;
    if (!(p & 2)) v |= 2;
    if (m->pad_th) {
        if (!(p & 4)) v |= 4;
        if (!(p & 8)) v |= 8;
        if (!(p & 32)) v |= 16;
        if (!(p & 64)) v |= 32;
        v |= 64;
    } else {
        if (!(p & 16)) v |= 16;
        if (!(p & 128)) v |= 32;
    }
    return v;
}

static uint16_t vdp_status(const MdFull *m) {
    /* A full NTSC frame is 262 lines; the active display lasts 224. */
    const unsigned line = m->frame_cycles / 488u;
    return (uint16_t)(0x3400u | (line >= 224 ? 0x0008u : 0u) |
                      (line >= 224 && m->irq_pending ? 0x0080u : 0u));
}

uint8_t md_full_read8(MdFull *m, uint32_t addr) {
    addr &= 0xffffffu;
    if (addr < md_rom_size) return md_rom[addr];
    if (addr >= 0xff0000u) return m->ram[addr & 0xffffu];
    if (addr >= 0xa00000u && addr < 0xa10000u) return md_full_sound_read(m, addr & 0xffffu);
    if (addr >= 0xa10000u && addr <= 0xa1001fu) {
        switch (addr & 0x1eu) {
        case 0: return 0x80;  /* NTSC/export version */
        case 2: return pad_read(m);
        case 8: return m->pad_control;
        default: return 0;
        }
    }
    if (addr >= 0xa11000u && addr < 0xa12000u)
        return (addr & 0xff00u) == 0x1100u ? (m->z80_bus ? 0 : 1) : 0;
    if (addr >= 0xc00000u && addr <= 0xc0001fu) {
        uint16_t v = md_full_read16(m, addr & ~1u);
        return (addr & 1u) ? (uint8_t)v : (uint8_t)(v >> 8);
    }
    if (addr >= 0xa14000u && addr < 0xa14004u) return 0;
    m->fault = 1; m->fault_pc = m->pc;
    return 0xff;
}

uint16_t md_full_read16(MdFull *m, uint32_t addr) {
    addr &= 0xffffffu;
    if (addr & 1u) { m->fault = 3; m->fault_pc = m->pc; return 0xffff; }
    if (addr >= 0xc00000u && addr <= 0xc0001fu) {
        if ((addr & 0x1cu) == 4u) { m->vdp_pending = 0; return vdp_status(m); }
        if ((addr & 0x1cu) == 8u) return (uint16_t)((m->frame_cycles / 488u) << 8);
        if ((addr & 0x1cu) == 0u) {
            uint16_t v = 0;
            if ((m->vdp_code & 15u) == 0u) v = (uint16_t)(m->vram[m->vdp_addr & 0xffffu] << 8 |
                m->vram[(m->vdp_addr + 1u) & 0xffffu]);
            else if ((m->vdp_code & 15u) == 8u) v = m->cram[(m->vdp_addr >> 1) & 63u];
            else if ((m->vdp_code & 15u) == 4u) v = (uint16_t)(m->vsram[m->vdp_addr % 80u] << 8 |
                m->vsram[(m->vdp_addr + 1u) % 80u]);
            m->vdp_addr = (m->vdp_addr + m->vdp_reg[15]) & 0xffffu;
            return v;
        }
    }
    return (uint16_t)(md_full_read8(m, addr) << 8 | md_full_read8(m, addr + 1u));
}

uint32_t md_full_read32(MdFull *m, uint32_t addr) {
    uint32_t hi = md_full_read16(m, addr);
    return hi << 16 | md_full_read16(m, addr + 2u);
}

static void vdp_store(MdFull *m, uint16_t v) {
    const unsigned addr = m->vdp_addr & 0xffffu;
    switch (m->vdp_code & 15u) {
    case 1:
        m->vram[addr] = (uint8_t)(v >> 8);
        m->vram[(addr + 1u) & 0xffffu] = (uint8_t)v;
        break;
    case 3: m->cram[(addr >> 1) & 63u] = v & 0x0eeeu; break;
    case 5:
        m->vsram[addr % 80u] = (uint8_t)(v >> 8);
        m->vsram[(addr + 1u) % 80u] = (uint8_t)v;
        break;
    default: m->fault = 4; m->fault_pc = m->pc; break;
    }
    m->vdp_addr = (m->vdp_addr + m->vdp_reg[15]) & 0xffffu;
}

static void vdp_dma(MdFull *m) {
    uint32_t length = (m->vdp_reg[20] << 8) | m->vdp_reg[19];
    if (!length) length = 65536;
    if (length > 65536u) length = 65536u;
    const unsigned mode = m->vdp_reg[23] >> 6;
    if (mode == 2u) { m->dma_fill = 1; return; }
    uint32_t source = ((m->vdp_reg[23] & 0x7fu) << 16) |
                      (m->vdp_reg[22] << 8) | m->vdp_reg[21];
    if (mode == 3u) {
        for (uint32_t i = 0; i < length; ++i) {
            const uint8_t v = m->vram[source++ & 0xffffu];
            m->vram[m->vdp_addr++ & 0xffffu] = v;
        }
    } else {
        for (uint32_t i = 0; i < length && !m->fault; ++i) {
            uint16_t v = md_full_read16(m, (source++ << 1) & 0xffffffu);
            vdp_store(m, v);
        }
    }
    m->vdp_reg[19] = m->vdp_reg[20] = 0;
    m->vdp_reg[21] = source & 255u;
    m->vdp_reg[22] = (source >> 8) & 255u;
}

static void vdp_control(MdFull *m, uint16_t v) {
    if (!m->vdp_pending && (v & 0xc000u) == 0x8000u) {
        unsigned reg = (v >> 8) & 31u;
        if (reg < 24u) m->vdp_reg[reg] = v & 255u;
        return;
    }
    if (!m->vdp_pending) { m->vdp_first = v; m->vdp_pending = 1; return; }
    m->vdp_addr = (m->vdp_first & 0x3fffu) | ((v & 3u) << 14);
    m->vdp_code = (uint8_t)((m->vdp_first >> 14) | ((v >> 2) & 0x3cu));
    m->vdp_pending = 0;
    if (m->vdp_code & 0x20u) vdp_dma(m);
}

void md_full_write8(MdFull *m, uint32_t addr, uint8_t v) {
    addr &= 0xffffffu;
    if (addr >= 0xff0000u) {
        m->ram[addr & 0xffffu] = v; return;
    }
    if (addr >= 0xa00000u && addr < 0xa10000u) { md_full_sound_write(m, addr & 0xffffu, v); return; }
    if (addr >= 0xa10000u && addr <= 0xa1001fu) {
        if ((addr & 0x1eu) == 2u) m->pad_th = v & 0x40u;
        else if ((addr & 0x1eu) == 8u) m->pad_control = v;
        return;
    }
    if (addr >= 0xa11000u && addr < 0xa12000u) {
        if (!(addr & 1u)) {
            if ((addr & 0xff00u) == 0x1100u) md_full_sound_control(m, 0, !!v);
            else if ((addr & 0xff00u) == 0x1200u) md_full_sound_control(m, 1, !!v);
        }
        return;
    }
    if (addr >= 0xa14000u && addr < 0xa14004u) return;
    if (addr >= 0xc00000u && addr <= 0xc0001fu) {
        if ((addr & 0x1fu) == 0x11u || (addr & 0x1fu) == 0x10u) {
            md_full_sound_write(m, 0x7f11u, v); return;
        }
        md_full_write16(m, addr & ~1u, (uint16_t)(v << 8 | v));
        return;
    }
    m->fault = 1; m->fault_pc = m->pc;
}

void md_full_write16(MdFull *m, uint32_t addr, uint16_t v) {
    addr &= 0xffffffu;
    if (addr & 1u) { m->fault = 3; m->fault_pc = m->pc; return; }
    if (addr >= 0xff0000u) {
        m->ram[addr & 0xffffu] = v >> 8;
        m->ram[(addr + 1u) & 0xffffu] = (uint8_t)v;
        return;
    }
    if (addr >= 0xc00000u && addr <= 0xc0001fu) {
        if ((addr & 0x1cu) == 4u) { vdp_control(m, v); return; }
        if ((addr & 0x1cu) == 0u) {
            if (m->dma_fill) {
                uint32_t count = m->vdp_reg[19] | m->vdp_reg[20] << 8;
                if (!count) count = 65536;
                m->dma_fill = 0;
                while (count--) vdp_store(m, v);
            } else vdp_store(m, v);
            return;
        }
        if ((addr & 0x1cu) == 0x10u) { md_full_sound_write(m, 0x7f11u, (uint8_t)v); return; }
    }
    md_full_write8(m, addr, v >> 8);
    md_full_write8(m, addr + 1u, (uint8_t)v);
}

void md_full_write32(MdFull *m, uint32_t addr, uint32_t v) {
    md_full_write16(m, addr, v >> 16);
    if (!m->fault) md_full_write16(m, addr + 2u, (uint16_t)v);
}
