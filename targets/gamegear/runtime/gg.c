#include "gg.h"
#ifdef GG_ROM_TRACE /* host analysis: ROM bytes read as data, by offset */
void gg_rom_trace(uint32_t offset);
static inline uint8_t gg_native_read(z80_t *c, uint16_t a) {
    (void)c;
    if (a < 0x400) gg_rom_trace(a);
    else if (a < 0xc000 && !(a >= 0x8000 && (machine.ram_ctl & 8))) gg_rom_trace((uint32_t)machine.page[a >> 14] * 0x4000 + (a & 0x3fff));
    return gg_read(a);
}
#else
static inline uint8_t gg_native_read(z80_t *c, uint16_t a) { (void)c; return gg_read(a); }
#endif
static inline void gg_native_write(z80_t *c, uint16_t a, uint8_t v) { (void)c; gg_write(a, v); }
#define Z80_NATIVE_READ gg_native_read
#define Z80_NATIVE_WRITE gg_native_write
#include "z80_ops.h"
/* Opcode fetch as the generic decoder sees it (bus_read): mapped by the
 * mapper, never traced as a data read (gg_rom_trace). */
#define Z80_FAST_FETCH(a) gg_read((a))
#include "z80_fast.h"
#include <string.h>
#ifdef GG_POOL_PACKED
#include "m0_rom_codec.h"
#endif

gg_t machine;
uintptr_t sg_stack_limit;

/* ---- bus ------------------------------------------------------------------ */

static uint8_t bus_read(void *u, uint16_t a) { (void)u; return gg_read(a); }
static void bus_write(void *u, uint16_t a, uint8_t v) { (void)u; gg_write(a, v); }
static uint8_t bus_in(void *u, uint16_t p) { (void)u; return gg_in((uint8_t)p); }
static void bus_out(void *u, uint16_t p, uint8_t v) { (void)u; gg_out((uint8_t)p, v); }

static void map_slot(unsigned s, uint8_t value) {
    gg_t *m = &machine;
    /* power-of-two page counts mirror with a mask; others (rare) divide */
    const unsigned n = m->rom_pages;
    const unsigned page = !n ? 0 : (n & (n - 1)) ? value % n : value & (n - 1);
    if (m->page[s] != page || (s == 2 && m->page2 != page)) { /* generated code checks at its next sync */
        m->remapped = 1;
        m->next_evt = m->cyc;
    }
    m->page[s] = (uint8_t)page;
    if (s == 2) {
        m->page2 = (uint8_t)page;
        if (m->ram_ctl & 8) m->page[2] = 0xff;
    }
#if GG_STREAM
    const unsigned index = gg_page_index[page];
    m->slot[s] = index == 0xff ? 0 : m->rom + (size_t)index * 0x4000;
#else
    m->slot[s] = m->rom + (size_t)page * 0x4000;
#endif
}

#if GG_STREAM || GG_CART_ON_CARD
/* Sector cache for the card: ROM pages left there and the cartridge RAM's
 * save file (tags with GG_SAVE_TAG), least recently used, written back
 * when dirty. */
#define GG_SAVE_TAG 0x40000000
static uint8_t card_buf[GG_STREAM_SECTORS][512] __attribute__((aligned(4)));
static int32_t card_tag[GG_STREAM_SECTORS];
static uint32_t card_age[GG_STREAM_SECTORS], card_clock;
static uint8_t card_dirty[GG_STREAM_SECTORS], card_last;
uint32_t gg_stream_misses;

uint8_t *gg_stream_scratch(void) { return card_buf[0]; }

static void card_reset(void) {
    for (unsigned i = 0; i < GG_STREAM_SECTORS; i++) { card_tag[i] = -1; card_age[i] = 0; card_dirty[i] = 0; }
    card_last = 0;
}

#if GG_CART_ON_CARD
static uint8_t save_pending;
static uint32_t save_change;
static void card_writeback(unsigned i) {
    if (!card_dirty[i]) return;
    gg_platform_save_write((uint32_t)card_tag[i] & ~GG_SAVE_TAG, card_buf[i]);
    card_dirty[i] = 0;
}
void gg_save_flush(void) {
    for (unsigned i = 0; i < GG_STREAM_SECTORS; i++) card_writeback(i);
    save_pending = 0;
}
void gg_save_tick(void) {
    if (save_pending && machine.frames - save_change >= 120) gg_save_flush();
}
#endif

static uint8_t *card_sector(int32_t tag) {
    if (card_tag[card_last] == tag) return card_buf[card_last];
    unsigned slot = 0;
    for (unsigned i = 0; i < GG_STREAM_SECTORS; i++) {
        if (card_tag[i] == tag) { slot = i; goto hit; }
        if (card_age[i] < card_age[slot]) slot = i;
    }
#if GG_CART_ON_CARD
    card_writeback(slot);
    if (tag & GG_SAVE_TAG) {
        if (!gg_platform_save_read((uint32_t)tag & ~GG_SAVE_TAG, card_buf[slot]))
            for (unsigned i = 0; i < 512; i++) card_buf[slot][i] = 0xff;
    } else
#endif
#if GG_STREAM
#ifdef GG_POOL_PACKED
    if (gg_pool_index[(uint32_t)tag] != GG_POOL_NONE) {
        const uint32_t record = gg_pool_records[gg_pool_index[(uint32_t)tag]];
        const uint8_t *src = gg_pool + (record & 0x3fffffffu);
        if (record >> 30) m0_rom_unpack512(src, card_buf[slot]);
        else memcpy(card_buf[slot], src, 512);
    } else
#endif
    if (!gg_platform_rom_read((uint32_t)tag, card_buf[slot]))
#endif
        for (unsigned i = 0; i < 512; i++) card_buf[slot][i] = 0xff;
    card_tag[slot] = tag;
    gg_stream_misses++;
hit:
    card_age[slot] = ++card_clock;
    card_last = (uint8_t)slot;
    return card_buf[slot];
}

#if GG_STREAM
uint8_t gg_stream_read(uint32_t offset) {
#ifdef GG_POOL_SECTORS
    const unsigned pool = gg_pool_index[offset >> 9];
    if (pool != GG_POOL_NONE) {
#ifdef GG_POOL_PACKED
        const uint32_t record = gg_pool_records[pool];
        if (!(record >> 30)) return gg_pool[(record & 0x3fffffffu) + (offset & 511)];
#else
        return gg_pool[pool << 9 | (offset & 511)];
#endif
    }
#endif
    return card_sector((int32_t)(offset >> 9))[offset & 511];
}
#endif
#if GG_CART_ON_CARD
uint8_t gg_cart_read(uint32_t offset) { return card_sector(GG_SAVE_TAG | (int32_t)(offset >> 9))[offset & 511]; }
void gg_cart_store(uint32_t offset, uint8_t v) {
    uint8_t *p = card_sector(GG_SAVE_TAG | (int32_t)(offset >> 9));
    if (p[offset & 511] == v) return;
    p[offset & 511] = v;
    card_dirty[card_last] = 1;
    save_pending = 1;
    save_change = machine.frames;
}
#endif
#endif

/* Sega mapper: FFFC RAM control, FFFD-FFFF pages of slots 0-2 (the RAM
 * byte is written as well, by gg_write). */
void gg_mapper_write(uint16_t a, uint8_t v) {
    gg_t *m = &machine;
    if (GG_CODEMASTERS) { map_slot(a >> 14, v); return; } /* 0000, 4000, 8000 */
    if (a == 0xfffc) {
#if GG_CART_ON_CARD
        if ((m->ram_ctl & 8) && !(v & 8)) gg_save_flush(); /* games disable the RAM after saving */
#endif
        m->ram_ctl = v;
        if (v & 8) m->cart_ram_used = 1;
        m->page[2] = (v & 8) ? 0xff : m->page2;
        m->remapped = 1;
        m->next_evt = m->cyc;
    } else map_slot(a - 0xfffd, v);
}

void gg_write_cart(uint16_t a, uint8_t v) {
#if GG_CART_ON_CARD
    gg_cart_store(gg_cart_offset(a, machine.ram_ctl), v);
#else
    if (machine.cart_ram) machine.cart_ram[gg_cart_offset(a, machine.ram_ctl)] = v;
#endif
}

/* ---- ports ---------------------------------------------------------------- */

uint8_t gg_in(uint8_t port) {
    gg_t *m = &machine;
    if (port <= 6 && !GG_IS_SMS) { /* Game Gear registers: START, region, link port, stereo */
        if (!port) return (uint8_t)((m->buttons & GG_START ? 0 : 0x80) | 0x40); /* export, NTSC */
        if (port == 4) return 0xff;              /* serial receive: nothing */
        if (port == 5) return m->io[5] & 0xf8;   /* serial: not busy, nothing received, no error */
        return m->io[port];
    }
    switch (port & 0xc1) {
    case 0x40: return gg_vcounter();
    case 0x41: { /* H counter: pixel position / 2 within the line */
        const uint32_t t = m->cyc - m->frame_start, line = ((t >> 2) * 18397u) >> 20; /* t / 228, exact below 77000 */
        return (uint8_t)(((t - line * GG_LINE_CYCLES) * 768u) >> 10); /* * 171 / 228 */
    }
    case 0x80: return gg_vdp_data_read();
    case 0x81: return gg_vdp_status_read();
    case 0xc0: return (uint8_t)~(m->buttons & 63);
    case 0xc1:
        if (GG_IS_SMS) { /* RESET up, TH A/B: outputs read back their level */
            const unsigned c = m->io_ctl, tha = (c & 2) ? 1 : (c >> 5) & 1, thb = (c & 8) ? 1 : (c >> 7) & 1;
            return (uint8_t)(0x3f | tha << 6 | thb << 7);
        }
        return 0xff;
    default: return 0xff;
    }
}

void gg_out(uint8_t port, uint8_t v) {
    if (GG_IS_SMS) {
        if ((port & 0xc1) == 0x01) { machine.io_ctl = v; return; } /* I/O control */
        if (port < 0x40) return;                                      /* memory control */
    } else if (port <= 6) { /* link port (written values read back), stereo: no sound effect here */
        if (port) machine.io[port] = v;
        return;
    }
    switch (port & 0xc1) {
    case 0x40: case 0x41: gg_psg_write(v); break;
    case 0x80: gg_vdp_data_write(v); break;
    case 0x81: gg_vdp_control_write(v); break;
    default: break; /* memory and I/O control */
    }
}

/* ---- machine -------------------------------------------------------------- */

void gg_irq_update(void) {
    gg_t *m = &machine;
    m->cpu.irq = ((m->vdp_status & 128) && (m->vdp[1] & 32)) || (m->line_irq && (m->vdp[0] & 16));
    if (m->cpu.irq && m->cpu.iff1) m->next_evt = m->cyc;
}

/* Next device event; an acceptable interrupt makes it now. */
static void schedule(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    m->next_evt = m->frame_start + (m->vblank_done ? GG_FRAME_CYCLES : GG_VBLANK_CYCLES);
    const uint32_t line = gg_vdp_next_event();
    if (line && (int32_t)(line - m->next_evt) < 0) m->next_evt = line;
    /* Consoles: lines are drawn every GG_RENDER_STEP lines, so that rows
     * go out to the display while the game runs, not in one burst. */
#ifdef GG_RENDER_STEP
    if (m->row && m->render_line < GG_WIN_TOP + GG_WIN_LINES) {
        const uint32_t step = m->frame_start + (uint32_t)((m->render_line / GG_RENDER_STEP + 1) * GG_RENDER_STEP) * GG_LINE_CYCLES;
        if ((int32_t)(step - m->next_evt) < 0) m->next_evt = step;
    }
#endif
    if (c->nmi || (c->irq && c->iff1 && !c->ei_delay)) m->next_evt = m->cyc;
}

void gg_init(const uint8_t *rom, size_t size, uint8_t *cart_ram) {
#ifdef GG_SMS
    gg_init_system(rom, size, cart_ram, GG_SMS);
#else
    gg_init_system(rom, size, cart_ram, 0);
#endif
}

void gg_init_system(const uint8_t *rom, size_t size, uint8_t *cart_ram, int sms) {
    gg_t *m = &machine;
    sg_fn (*lookup)(uint16_t) = m->lookup;
    uint32_t *coverage = m->coverage;
    uint8_t *entries = m->entries;
    uint32_t coverage_size = m->coverage_size;
    memset(m, 0, sizeof *m);
    m->lookup = lookup;
    m->coverage = coverage; m->entries = entries; m->coverage_size = coverage_size;
    m->sms = (uint8_t)!!sms;
    m->rom = rom; /* rom_pages * 16 KiB readable bytes (callers pad) */
    m->rom_size = (uint32_t)size;
    m->rom_pages = (uint16_t)((size + 0x3fff) / 0x4000);
    /* Codemasters boards: checksum and its complement at 7FE6/7FE8.  Only
     * the generic host detects it: generated projects fix GG_MAPPER, and a
     * streamed one may hold page 0 alone in flash, so 7FE6 may lie past the
     * array -- past the end of flash when it is full (device, 2026-10-17:
     * HardFault at boot). */
#ifndef GG_MAPPER
    if (size >= 0x8000) {
        const unsigned sum = rom[0x7fe6] | rom[0x7fe7] << 8, inverse = rom[0x7fe8] | rom[0x7fe9] << 8;
        m->mapper = sum && !((sum + inverse) & 0xffff);
    }
#else
    m->mapper = GG_MAPPER;
#endif
    m->cart_ram = cart_ram;
#if GG_STREAM || GG_CART_ON_CARD
    card_reset();
#endif
    m->cpu.user = m;
    m->cpu.read = bus_read; m->cpu.write = bus_write;
    m->cpu.in = bus_in; m->cpu.out = bus_out;
    gg_reset();
}

void gg_reset(void) {
    gg_t *m = &machine;
    z80_t keep = m->cpu;
    z80_reset(&m->cpu);
    m->cpu.user = keep.user; m->cpu.read = keep.read; m->cpu.write = keep.write;
    m->cpu.in = keep.in; m->cpu.out = keep.out;
    m->cpu.sp = 0xdff0;
    memset(m->ram, 0, sizeof m->ram);
    memset(m->vram, 0, sizeof m->vram);
    memset(m->cram, 0, sizeof m->cram);
    if (m->cart_ram) memset(m->cart_ram, 0, GG_CART_RAM_SIZE);
    m->ram_ctl = 0;
    for (unsigned s = 0; s < 3; s++) map_slot(s, (uint8_t)s);
    /* VDP registers as left by the boot ROM */
    static const uint8_t regs[11] = { 0x36, 0x80, 0xff, 0xff, 0xff, 0xff, 0xfb, 0x00, 0x00, 0x00, 0xff };
    memset(m->vdp, 0, sizeof m->vdp);
    memcpy(m->vdp, regs, sizeof regs);
    m->sat_base = (uint16_t)((m->vdp[5] & 0x7e) << 7);
    m->vdp_address = 0;
    m->vdp_code = m->vdp_pending = m->vdp_latch = m->vdp_buffer = m->cram_latch = 0;
    m->vdp_status = 0;
    m->line_irq = 0;
    m->sprites_dirty = 1;
    for (unsigned i = 0; i < 32; i++) m->pal[i] = 0;
    m->cyc = m->frame_start = 0;
    m->render_line = 0;
    m->render_at = GG_LINE_CYCLES;
    m->vblank_done = 0;
    m->frames = 0;
    m->lc_count = m->vdp[10];
    gg_vdp_frame_start();
    m->buttons = 0;
    {   /* the link port and stereo registers at power on */
        static const uint8_t io[7] = { 0xc0, 0x7f, 0xff, 0x00, 0xff, 0x00, 0xff };
        memcpy(m->io, io, sizeof io);
        m->io_ctl = 0xff;
    }
    memset(m->psg_tone, 0, sizeof m->psg_tone);
    memset(m->psg_volume, 15, sizeof m->psg_volume);
    m->psg_noise = m->psg_latch = 0;
    m->audio_phase = m->audio_tick_phase = 0;
    m->audio_cyc = 0;
    m->audio_head = m->audio_tail = 0;
    m->audio_lfsr = 0x8000;
    memset(m->audio_count, 0, sizeof m->audio_count);
    memset(m->audio_output, 0, sizeof m->audio_output);
    m->interpreted = m->compiled = 0;
    schedule();
}

/* Master System: PAUSE pressed raises the NMI */
void gg_buttons(uint8_t pressed) {
    gg_t *m = &machine;
    if (GG_IS_SMS && (pressed & GG_PAUSE) && !(m->buttons & GG_PAUSE)) { m->cpu.nmi = 1; m->next_evt = m->cyc; }
    m->buttons = pressed;
}

/* ---- events and interrupts ------------------------------------------------ */

void sg_sync(void) {
    gg_t *m = &machine;
#ifdef GG_RENDER_STEP
    gg_vdp_catchup();
#endif
    for (;;) {
        gg_line_counter_catchup();
        if (!m->vblank_done) {
            if ((int32_t)(m->cyc - (m->frame_start + GG_VBLANK_CYCLES)) < 0) break;
            gg_vdp_catchup(); /* the 192 lines have ended */
            m->vblank_done = 1;
            m->vdp_status |= 128;
        } else {
            if ((int32_t)(m->cyc - (m->frame_start + GG_FRAME_CYCLES)) < 0) break;
            gg_audio_catchup();
            m->frame_start += GG_FRAME_CYCLES;
            m->vblank_done = 0;
            m->render_line = 0;
            m->render_at = m->frame_start + GG_LINE_CYCLES;
            gg_vdp_frame_start();
            m->frames++;
            if (m->frame) m->frame(m->platform);
        }
    }
    gg_irq_update();
    schedule();
}

/* Take an NMI or an acceptable IRQ: push pc, jump to the vector. */
static int irq_enter(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    unsigned cycles;
    if (c->nmi) {
        c->nmi = 0;
        c->iff2 = c->iff1; c->iff1 = 0;
        z80_push(c, c->pc);
        if (m->entries && m->irq_depth < 8) m->irq_sp[m->irq_depth++] = c->sp;
        c->pc = 0x66;
        cycles = 11;
    } else if (c->irq && c->iff1 && !c->ei_delay) {
        c->iff1 = c->iff2 = 0;
        if (c->im == 0 && (c->irq_vector & 0xc7) != 0xc7) { c->fault = 1; m->stop = 1; return 0; }
        z80_push(c, c->pc);
        if (m->entries && m->irq_depth < 8) m->irq_sp[m->irq_depth++] = c->sp;
        c->pc = c->im == 2 ? z80_word(c, (uint16_t)((c->i << 8) | c->irq_vector))
              : c->im == 0 ? (uint16_t)(c->irq_vector & 0x38) : 0x38;
        cycles = c->im == 2 ? 19 : 13;
    } else return 0;
    c->halted = 0;
    c->r = (uint8_t)((c->r & 128) | ((c->r + 1) & 127));
    m->cyc += cycles;
    schedule();
    return 1;
}

/* Nonzero when pc moved, or when a page changed since the last event (a
 * block copy over the mapper registers, an interrupt handler that does not
 * restore them): generated code then returns to the dispatcher, which
 * looks pc up again under the new mapping. */
int sg_event(uint16_t resume_pc) {
    gg_t *m = &machine;
    for (;;) {
        sg_sync();
        m->cpu.pc = resume_pc;
        if (m->stop) return 1;
        if (m->remapped) { m->remapped = 0; return 1; } /* back to the dispatcher */
        if (!irq_enter()) return 0;
        sg_exec();
        if (m->cpu.pc != resume_pc || m->stop) return 1;
    }
}

void sg_halt(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    for (;;) {
        sg_sync();
        if (m->stop || c->nmi || (c->irq && c->iff1 && !c->ei_delay)) return;
        uint32_t ticks = (m->next_evt - m->cyc + 3) / 4;
        m->cyc += ticks * 4;
        c->r = (uint8_t)((c->r & 128) | ((c->r + ticks) & 127));
    }
}

/* ---- dispatch ------------------------------------------------------------- */

#define SG_EXEC_HEADROOM 768u

static void dispatch_one(int compiled) {
    gg_t *m = &machine;
    if (m->cpu.halted) {
        sg_halt();
        if (!m->stop) (void)sg_event(m->cpu.pc);
        return;
    }
    if (m->cpu.ei_delay) {
        sg_step(m->cpu.pc);
        return;
    }
    sg_fn fn = compiled && m->lookup ? m->lookup(m->cpu.pc) : 0;
    if (fn) { m->compiled++; fn(); }
    else sg_interp_block();
}

void sg_exec(void) {
    char probe;
    if ((uintptr_t)&probe < sg_stack_limit + SG_EXEC_HEADROOM) return;
    uint16_t sp0 = machine.cpu.sp;
    do {
        if (machine.stop) return;
        dispatch_one(1);
    } while ((int16_t)(machine.cpu.sp - sp0) <= 0);
}

void gg_run(void) {
    char probe;
    if (!sg_stack_limit) sg_stack_limit = (uintptr_t)&probe - 4096u;
    int room = (uintptr_t)&probe >= sg_stack_limit + SG_EXEC_HEADROOM;
    while (!machine.stop) dispatch_one(room);
}

/* Profiling: targets of RET, RETI/RETN and JP (HL/IX/IY), interrupt
 * returns left out (see sg1000.c). */
static void note_dynamic(const z80_decoded_t *d, uint16_t sp, uint16_t pc) {
    gg_t *m = &machine;
    const unsigned op = d->opcode;
    const int ret = d->group == 0 ? op == 0xc9 || (op & 0xc7) == 0xc0 : d->group == 2 && (op & 0xc7) == 0x45;
    if (!ret && !(d->group == 0 && op == 0xe9)) return;
    while (m->irq_depth && sp > m->irq_sp[m->irq_depth - 1]) m->irq_depth--;
    if (ret && m->irq_depth && sp == m->irq_sp[m->irq_depth - 1]) { m->irq_depth--; return; }
    const uint32_t loc = gg_location(pc);
    if (loc < m->coverage_size) m->entries[loc] = 1;
}

/* Host profiling: one more execution of the instruction at loc; the
 * explorer (gg_host --explore) counts the locations never seen before. */
#ifdef GG_ROM_TRACE
uint32_t gg_new_locations;
static inline void gg_cover(uint32_t loc) {
    if (!machine.coverage[loc]) gg_new_locations++;
    machine.coverage[loc]++;
}
#else
static inline void gg_cover(uint32_t loc) { machine.coverage[loc]++; }
#endif

static int interp_one(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    if ((int32_t)(m->cyc - m->next_evt) >= 0 && sg_event(c->pc)) return 0;
    uint16_t pc = c->pc;
    if (m->coverage) { const uint32_t loc = gg_location(pc); if (loc < m->coverage_size) gg_cover(loc); }
    int jumped;
    const int fast = z80_fast(c, &jumped); /* common opcodes without decoding */
    if (fast >= 0) {
        c->r = (uint8_t)((c->r & 128) | ((c->r + 1) & 127));
        m->cyc += (uint32_t)fast;
        m->interpreted++;
        if (c->ei_delay && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
        return !jumped;
    }
    z80_decoded_t d;
    if (!z80_decode(c, &d)) { c->fault = 1; m->stop = 1; return 0; }
    const uint16_t sp = c->sp;
    m->cyc += z80_execute(c, &d);
    m->interpreted++;
    if (m->entries && c->pc != (uint16_t)(pc + d.length)) note_dynamic(&d, sp, c->pc);
    if (c->ei_delay && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
    if (c->halted) {
        sg_halt();
        if (!m->stop) (void)sg_event(c->pc);
        return 0;
    }
    return c->pc == (uint16_t)(pc + d.length);
}

/* OUTI / OUTD in RAM, where games unroll their VRAM uploads: z80_execute's
 * effects without the generic decode.  1 done, 0 another instruction, -1
 * stopped by an event. */
static int ram_outx(void) {
    gg_t *m = &machine;
    z80_t *c = &m->cpu;
    const uint16_t pc = c->pc;
    if (pc < 0xc000 || pc == 0xffff || m->ram[pc & 8191] != 0xed) return 0;
    const uint8_t op = m->ram[(pc + 1) & 8191];
    if ((op & 0xf7) != 0xa3) return 0;
    if ((int32_t)(m->cyc - m->next_evt) >= 0 && sg_event(pc)) return -1;
    if (c->pc != pc) return 0; /* the event moved pc */
    if (m->coverage) { const uint32_t loc = gg_location(pc); if (loc < m->coverage_size) gg_cover(loc); }
    c->pc = (uint16_t)(pc + 2);
    c->r = (uint8_t)((c->r & 128) | ((c->r + 2) & 127));
    const uint8_t v = gg_native_read(c, c->hl);
    c->b--;
    c->hl = (uint16_t)(c->hl + (op & 8 ? -1 : 1));
    gg_out(c->c, v);
    const unsigned s = v + c->l;
    c->f = (uint8_t)(z80_sz(c->b) | (v & 128 ? Z80_N : 0) | (s > 255 ? Z80_H | Z80_C : 0) | z80_parity((uint8_t)((s & 7) ^ c->b)));
    m->cyc += 16;
    m->interpreted++;
    if (c->ei_delay && --c->ei_delay == 0 && c->irq) m->next_evt = m->cyc;
    return 1;
}

void sg_interp_block(void) {
    unsigned n = 0;
    if (machine.cpu.pc >= 0xc000)
        for (int r; n < 32 && (r = ram_outx()) != 0; n++) if (r < 0) return;
    for (; n < 32 && interp_one(); n++) {}
}

int sg_step(uint16_t after) {
    do (void)interp_one();
    while (machine.cpu.ei_delay && !machine.stop && !machine.cpu.fault);
    return machine.cpu.pc == after && !machine.stop;
}
