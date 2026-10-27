/* Initial SNES PPU: VRAM/CGRAM/OAM ports, DMA, backgrounds and objects.
 * The renderer samples the registers per line — the frame's rows keep the
 * registers as the code left them, so a write during a line's HBlank
 * reaches the next line (writes inside the visible part too: the exact
 * mid-scanline change points are not modelled). */
#include "snes.h"

static unsigned vram_word(void) {
    unsigned a = machine.vmaddr;
    switch ((machine.ppu_regs[0x15] >> 2) & 3) {
    case 1: return (a & 0xff00) | ((a & 0x1f) << 3) | ((a >> 5) & 7);
    case 2: return (a & 0xfe00) | ((a & 0x3f) << 3) | ((a >> 6) & 7);
    case 3: return (a & 0xfc00) | ((a & 0x7f) << 3) | ((a >> 7) & 7);
    default: return a;
    }
}
static void vram_inc(unsigned high) {
    static const unsigned step[4] = {1, 32, 128, 128};
    if (((machine.ppu_regs[0x15] >> 7) & 1) == high)
        machine.vmaddr += step[machine.ppu_regs[0x15] & 3];
}

static unsigned oam_offset(unsigned addr) {
    addr &= 0x3ff;
    return addr < 0x200 ? addr : 0x200 + (addr & 31);
}

void snes_ppu_write(uint16_t at, uint8_t v) {
    unsigned r = at & 0xff;
    if (r >= 0x40) return;
    machine.ppu_regs[r] = v;
    switch (r) {
    case 0x02: case 0x03:
        machine.oamaddr = ((machine.ppu_regs[2] | ((unsigned)machine.ppu_regs[3] & 1) << 8) << 1) & 0x3ff;
        break;
    case 0x04: {
        unsigned a = machine.oamaddr;
        if (!(a & 1)) machine.oam_latch = v;
        if (a & 0x200) machine.oam[oam_offset(a)] = v;
        else if (a & 1) {
            machine.oam[oam_offset(a - 1)] = machine.oam_latch;
            machine.oam[oam_offset(a)] = v;
        }
        machine.oamaddr = (a + 1) & 0x3ff;
        break;
    }
    case 0x0d: case 0x0e: case 0x0f: case 0x10:
    case 0x11: case 0x12: case 0x13: case 0x14:
        machine.scroll[r - 0x0d] = ((unsigned)v << 8 | machine.scroll_latch) & 0x3ff;
        machine.scroll_latch = v;
        break;
    case 0x16: machine.vmaddr = (machine.vmaddr & 0xff00) | v; break;
    case 0x17: machine.vmaddr = (machine.vmaddr & 0x00ff) | (unsigned)v << 8; break;
    case 0x18: case 0x19:
        machine.vram[((vram_word() << 1) + (r & 1)) & 0xffff] = v;
        vram_inc(r & 1);
        break;
    case 0x21: machine.cgaddr = (unsigned)v << 1; break;
    case 0x22:
        if (!(machine.cgaddr & 1)) machine.cg_latch = v;
        else {
            machine.cgram[(machine.cgaddr - 1) & 511] = machine.cg_latch;
            machine.cgram[machine.cgaddr & 511] = v & 0x7f;
        }
        machine.cgaddr = (machine.cgaddr + 1) & 511;
        break;
    }
}

uint8_t snes_ppu_read(uint16_t at) {
    unsigned r = at & 0xff;
    if (r == 0x38) {
        uint8_t v = machine.oam[oam_offset(machine.oamaddr)];
        machine.oamaddr = (machine.oamaddr + 1) & 0x3ff;
        return v;
    }
    if (r == 0x39 || r == 0x3a) {
        unsigned high = r == 0x3a;
        uint8_t v = machine.vram[((vram_word() << 1) + high) & 0xffff];
        vram_inc(high);
        return v;
    }
    if (r == 0x3b) {
        uint8_t v = machine.cgram[machine.cgaddr & 511];
        machine.cgaddr = (machine.cgaddr + 1) & 511;
        return v;
    }
    if (r == 0x3f) /* STAT77: the last evaluated line's sprite flags */
        return (uint8_t)((machine.obj_time_over ? 0x40 : 0) | (machine.obj_range_over ? 0x20 : 0)) |
               (machine.cpu.mdr & 0x1f);
    return machine.cpu.mdr;
}

/* the transfer modes' B-side register offsets and unit sizes, shared by
 * the general DMA and the HDMA */
static const uint8_t dma_offsets[8][4] = {
    {0,0,0,0}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1},
    {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}
};
static const uint8_t dma_periods[8] = {1,2,2,4,4,4,2,4};

void snes_dma_start(uint8_t mask) {
    for (unsigned ch = 0; ch < 8; ch++) if (mask & (1u << ch)) {
        uint8_t *d = machine.dma[ch];
        unsigned mode = d[0] & 7, addr = d[2] | d[3] << 8;
        unsigned n = d[5] | d[6] << 8;
        if (!n) n = 65536;
        for (unsigned i = 0; i < n; i++) {
            uint32_t a = (uint32_t)d[4] << 16 | addr;
            uint32_t b = 0x2100u | (uint8_t)(d[1] + dma_offsets[mode][i % dma_periods[mode]]);
            if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(b));
            else snes_bus_write(b, snes_bus_read(a));
            if (!(d[0] & 8)) addr = (addr + ((d[0] & 0x10) ? -1 : 1)) & 0xffff;
        }
        d[2] = addr; d[3] = addr >> 8; d[5] = d[6] = 0;
    }
}

/* The HDMA: one unit per line, run at the line's H-Blank (no bus time
 * stolen, as the general DMA).  Each enabled channel walks its table at
 * A1Bx/A2Ax: a 0 count byte ends the channel for the frame, bit 7 of the
 * count byte repeats the entry's unit every line for count lines, and
 * 01h-7Fh transfer one unit then pause count-1 lines.  Indirect mode
 * (DMAPx bit 6) loads DASx from a 16-bit table pointer per entry, the
 * data bank being DASBx; the data address always increments. */
void snes_hdma_line(void) {
    for (unsigned ch = 0; ch < 8; ch++) {
        if (!(machine.hdma_enable & (1u << ch)) || machine.hdma_done[ch]) continue;
        uint8_t *d = machine.dma[ch];
        if (!machine.hdma_run[ch]) {
            machine.hdma_run[ch] = 1; /* started this frame, possibly mid-frame */
            machine.hdma_count[ch] = 0;
            d[8] = d[2]; d[9] = d[3]; /* the table pointer reloads */
        }
        const unsigned mode = d[0] & 7;
        const uint16_t period = dma_periods[mode];
        if (!machine.hdma_count[ch]) {
            uint16_t table = d[8] | d[9] << 8;
            const uint32_t at = (uint32_t)d[4] << 16 | table;
            const uint8_t b = snes_bus_read(at);
            table++;
            if (d[0] & 0x40) {
                const uint16_t p = (uint16_t)(snes_bus_read((uint32_t)d[4] << 16 | table) |
                                             snes_bus_read((uint32_t)d[4] << 16 | (uint16_t)(table + 1)) << 8);
                table += 2;
                d[5] = (uint8_t)p; d[6] = (uint8_t)(p >> 8); /* DASx: the data address */
            }
            d[8] = (uint8_t)table; d[9] = (uint8_t)(table >> 8);
            if (!b) {
                machine.hdma_done[ch] = 1;
                machine.hdma_run[ch] = 0;
                continue;
            }
            machine.hdma_len[ch] = b & 0x7F;
            machine.hdma_count[ch] = b & 0x7F;
            machine.hdma_rep[ch] = b >> 7;
        }
        if ((machine.hdma_rep[ch] && machine.hdma_count[ch]) ||
            (!machine.hdma_rep[ch] && machine.hdma_count[ch] == machine.hdma_len[ch])) {
            for (unsigned j = 0; j < period; j++) {
                const uint32_t bport = 0x2100u | (uint8_t)(d[1] + dma_offsets[mode][j]);
                if (d[0] & 0x40) { /* indirect: the data at DASBx/DASx */
                    const uint32_t a = (uint32_t)d[7] << 16 | d[5] | d[6] << 8;
                    if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(bport));
                    else snes_bus_write(bport, snes_bus_read(a));
                    const uint16_t da = (uint16_t)(d[5] | d[6] << 8) + 1;
                    d[5] = (uint8_t)da; d[6] = (uint8_t)(da >> 8);
                } else { /* direct: the table itself is the data */
                    const uint32_t a = (uint32_t)d[4] << 16 | d[8] | d[9] << 8;
                    if (d[0] & 0x80) snes_bus_write(a, snes_bus_read(bport));
                    else snes_bus_write(bport, snes_bus_read(a));
                    const uint16_t ta = (uint16_t)(d[8] | d[9] << 8) + 1;
                    d[8] = (uint8_t)ta; d[9] = (uint8_t)(ta >> 8);
                }
            }
        }
        if (machine.hdma_count[ch]) machine.hdma_count[ch]--;
    }
}

static uint16_t vword(unsigned a) {
    a &= 0xffff;
    return machine.vram[a] | (unsigned)machine.vram[(a + 1) & 0xffff] << 8;
}

/* Object pixels are selected in OAM order before their priority is
 * compared to backgrounds: 32 sprites at most per line, 34 8-pixel tile
 * slots (a wide sprite spends several), the extras setting the flags of
 * $213F and staying invisible.  The vertical range counts even for a
 * sprite whose X sits off screen, as the hardware's evaluation does. */
static void object_line(unsigned y, uint8_t color[256], uint8_t prio[256]) {
    static const uint8_t width[8][2] = {
        {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{16,32},{16,32}
    };
    static const uint8_t height[8][2] = {
        {8,16},{8,32},{8,64},{16,32},{16,64},{32,64},{32,64},{32,32}
    };
    const uint8_t obsel = machine.ppu_regs[1];
    const unsigned size_mode = obsel >> 5;
    const unsigned first = (machine.ppu_regs[3] & 0x80) ? (machine.ppu_regs[2] >> 1) : 0;
    unsigned selected[32], ns = 0, tiles = 0;
    machine.obj_range_over = 0;
    machine.obj_time_over = 0;
    for (unsigned n = 0; n < 128; n++) {
        const unsigned i = (first + n) & 127;
        const uint8_t *o = machine.oam + i * 4;
        const unsigned high = (machine.oam[512 + i / 4] >> ((i & 3) * 2)) & 3;
        const unsigned h = height[size_mode][high >> 1];
        const unsigned py = (y - o[1]) & 255;
        if (py >= h) continue;
        if (ns == 32) {
            machine.obj_range_over = 1;
            break;
        }
        tiles += width[size_mode][high >> 1] / 8;
        if (tiles > 34) {
            machine.obj_time_over = 1;
            break;
        }
        selected[ns++] = i;
    }
    /* the lowest OAM order paints last, as the hardware's priority */
    for (int n = (int)ns - 1; n >= 0; n--) {
        const unsigned i = selected[n];
        const uint8_t *o = machine.oam + i * 4;
        const unsigned high = (machine.oam[512 + i / 4] >> ((i & 3) * 2)) & 3;
        const unsigned w = width[size_mode][high >> 1], h = height[size_mode][high >> 1];
        const int x0 = (int)o[0] - ((high & 1) ? 256 : 0);
        unsigned py = (y - o[1]) & 255;
        if (py >= h || x0 >= 256 || x0 + (int)w <= 0) continue;
        if (o[3] & 0x80) py = h - 1 - py;
        unsigned base = (obsel & 3) * 0x4000u;
        if (o[3] & 1) base += (((obsel >> 3) & 3) + 1) * 0x2000u;
        for (unsigned dx = 0; dx < w; dx++) {
            const int x = x0 + (int)dx;
            if (x < 0 || x >= 256) continue;
            const unsigned px = (o[3] & 0x40) ? w - 1 - dx : dx;
            const unsigned tile = ((o[2] + (py >> 3) * 16) & 0xf0) | ((o[2] + (px >> 3)) & 15);
            const unsigned row = base + tile * 32 + (py & 7) * 2, bit = 7 - (px & 7);
            unsigned c = 0;
            for (unsigned p = 0; p < 4; p++)
                c |= ((machine.vram[(row + (p / 2) * 16 + (p & 1)) & 0xffff] >> bit) & 1) << p;
            if (c) {
                color[x] = (uint8_t)(128 + ((o[3] >> 1) & 7) * 16 + c);
                prio[x] = (o[3] >> 4) & 3;
            }
        }
    }
}

static unsigned bg_priority(unsigned mode, unsigned bg, unsigned high, unsigned bg3_priority) {
    static const uint8_t mode0[4][2] = {{8,11},{7,10},{2,5},{1,4}};
    static const uint8_t mode1[3][2] = {{7,10},{6,9},{2,4}};
    if (mode == 0) return mode0[bg][high];
    if (bg == 2 && high && bg3_priority) return 12;
    return mode1[bg][high];
}

void snes_render_line(unsigned y, uint8_t *row) {
    const uint8_t *r = machine.ppu_regs;
    unsigned mode = r[5] & 7, bright = r[0] & 15;
    uint8_t obj_color[256] = {0}, obj_prio[256] = {0};
    if (!(r[0] & 0x80) && (r[0x2c] & 0x10) && (mode == 0 || mode == 1))
        object_line(y, obj_color, obj_prio);
    for (unsigned x = 0; x < 256; x++) {
        unsigned color = 0, best = 0;
        if (!(r[0] & 0x80) && (mode == 0 || mode == 1)) {
            if (obj_color[x]) {
                static const uint8_t sprite_priority[2][4] = {{3,6,9,12}, {3,5,8,11}};
                best = sprite_priority[mode][obj_prio[x]];
                color = obj_color[x];
            }
            for (int bg = 3; bg >= 0; bg--) {
                if (!(r[0x2c] & (1 << bg))) continue;
                unsigned bpp = mode == 0 ? 2 : (bg < 2 ? 4 : bg == 2 ? 2 : 0);
                if (!bpp) continue;
                unsigned sx = (x + machine.scroll[bg * 2]) & 0x3ff;
                unsigned sy = (y + machine.scroll[bg * 2 + 1]) & 0x3ff;
                unsigned size = (r[5] & (0x10 << bg)) ? 16 : 8;
                unsigned tx = sx / size, ty = sy / size;
                unsigned screen = r[7 + bg] & 3;
                unsigned page_x = (tx >> 5) & (screen & 1);
                unsigned page_y = (ty >> 5) & ((screen >> 1) & 1);
                unsigned page = page_x + page_y * ((screen & 1) ? 2 : 1);
                unsigned map = (r[7 + bg] & 0xfc) * 0x200u + page * 0x800u;
                uint16_t entry = vword(map + ((ty & 31) * 32 + (tx & 31)) * 2);
                unsigned px = sx % size, py = sy % size;
                if (entry & 0x4000) px = size - 1 - px;
                if (entry & 0x8000) py = size - 1 - py;
                unsigned tile = (entry & 0x3ff) + (px >> 3) + (py >> 3) * 16;
                unsigned base = ((r[0x0b + bg / 2] >> ((bg & 1) * 4)) & 15) * 0x2000u;
                unsigned row_ = base + tile * (bpp * 8) + (py & 7) * 2;
                unsigned bit = 7 - (px & 7), index = 0;
                for (unsigned p = 0; p < bpp; p++)
                    index |= ((machine.vram[(row_ + (p / 2) * 16 + (p & 1)) & 0xffff] >> bit) & 1) << p;
                unsigned priority = bg_priority(mode, bg, (entry >> 13) & 1, r[5] & 8);
                if (index && priority > best) {
                    best = priority;
                    color = (mode == 0 ? bg * 32 : 0) + ((entry >> 10) & 7) * (1u << bpp) + index;
                }
            }
        }
        unsigned c = machine.cgram[(color * 2) & 511] | machine.cgram[(color * 2 + 1) & 511] << 8;
        if (r[0] & 0x80) c = 0;
        for (unsigned k = 0; k < 3; k++) {
            unsigned v = (c >> (k * 5)) & 31;
            *row++ = (uint8_t)((v * 255u * bright + 232u) / (31u * 15u));
        }
    }
}

void snes_render(uint8_t *rgb) {
    for (unsigned y = 0; y < 224; y++)
        snes_render_line(y, rgb + y * 256u * 3u);
}
