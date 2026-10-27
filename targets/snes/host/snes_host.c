/* SNES host harness: a cartridge image in the machine, N frames out, and
 * the run's fingerprint (CPU, the 128 KiB of WRAM, the frame counter — the
 * same contract shape as the other runtimes).  Built with the recompiled
 * code (SNES_NATIVE), --interp runs the interpreter instead: the reference
 * a precise project must match.  --coverage writes the interpreter's
 * executions per address, the contexts (K, DBR, the direct page, the M/X/E
 * flags) they ran under, and the addresses a dynamic transfer (RTS, RTL,
 * RTI, a jump through a pointer, an interrupt) landed on. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pc_input.h"

#include "../runtime/snes.h"
#ifdef SNES_EMBEDDED
#include "snes_config.h"
extern const uint8_t snes_rom[];
#endif

#ifdef SNES_NATIVE
extern int snes_native;
#endif

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the 65816 interpreter calls these every instruction when the build
 * defines SNES_TRACE (the generated host projects do); --coverage dumps
 * them.  The context bytes of the instruction (up to four an address) and
 * the dynamic landings go with it: the recompiler keys its instances by
 * both.  The addresses land in an open-addressing table, half full at
 * most: a 24-bit K:PC would need 16 Mi entries flat. */
#define COV_BITS 20
#define COV_SLOTS (1u << COV_BITS)
#define SNES_CTX_MAX 4
typedef struct {
    uint32_t pc;
    uint32_t hits;
    uint64_t ctx[SNES_CTX_MAX];
    uint8_t ctx_n;
    uint8_t dyn;
} cov_ent;
static cov_ent *cov;

static cov_ent *cov_at(uint32_t pc) {
    if (!cov) cov = calloc(COV_SLOTS, sizeof *cov);
    uint32_t h = (pc * 0x9E3779B1u) >> (32 - COV_BITS);
    for (;;) {
        if (!cov[h].pc) { cov[h].pc = pc; return &cov[h]; }
        if (cov[h].pc == pc) return &cov[h];
        h = (h + 1) & (COV_SLOTS - 1);
    }
}

void snes_trace_step(uint32_t pc24) {
    cov_ent *e = cov_at(pc24 & 0xFFFFFFu);
    if (e->hits < 0xFFFFFFFFu) e->hits++;
    const uint64_t m = snes_ctx_key(&machine.cpu);
    for (uint8_t i = 0; i < e->ctx_n; i++)
        if (e->ctx[i] == m) return;
    if (e->ctx_n < SNES_CTX_MAX) e->ctx[e->ctx_n++] = m;
}
void snes_trace_dyn(uint32_t pc24) { cov_at(pc24 & 0xFFFFFFu)->dyn = 1; }

/* the header's map nibble, as the generator reads it: a paired checksum
 * complement or a known map byte, HiROM's $21/$25 winning over LoROM's
 * only when LoROM's own header does not answer */
static int header_map(const uint8_t *rom, uint32_t bytes, size_t at) {
    if (at + 0x40 > bytes) return -1;
    const unsigned comp = rom[at + 0x3C] | rom[at + 0x3D] << 8;
    const unsigned cksum = rom[at + 0x3E] | rom[at + 0x3F] << 8;
    const unsigned map = rom[at + 0x15] & 0x0F;
    if ((comp ^ cksum) == 0xFFFF && (comp != 0 || cksum != 0xFFFF)) return (int)map;
    if (map == 0 || map == 1 || map == 2 || map == 3 || map == 5) return (int)map;
    return -1;
}
static int detect_hirom(const uint8_t *rom, uint32_t bytes) {
    const int hi = header_map(rom, bytes, 0xFFC0);
    const int lo = header_map(rom, bytes, 0x7FC0);
    if (hi == 1 || hi == 5) return 1;
    if (lo == 0 || lo == 2 || lo == 3) return 0;
    return hi >= 0 && lo < 0;
}
/* the header's RAM byte, as the generator reads it: $01-$03 are 2/8/32 KiB,
 * the runtime's window mirrors 32 KiB at most */
static uint32_t header_sram(const uint8_t *rom, uint32_t bytes, int hirom) {
    static const uint32_t table[4] = {0, 2048, 8192, 32768};
    const size_t at = (hirom ? 0xFFC0u : 0x7FC0u) + 0x18;
    if (at >= bytes) return 0;
    return rom[at] < 4 ? table[rom[at]] : 0;
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *input_path = 0, *coverage = 0, *capture = 0;
    unsigned frames = 60;
    int interp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else rom_path = argv[i];
    }
#ifdef SNES_NATIVE
    /* the profile counts the interpreter's steps: every instruction then */
    snes_native = !(interp || coverage);
#endif
    (void)interp;
    const uint8_t *rom;
    uint32_t rom_bytes;
    int hirom = 0;
    uint32_t sram_bytes = 0;
    if (!rom_path) {
#ifdef SNES_EMBEDDED
        rom = snes_rom;
        rom_bytes = SNES_ROM_SIZE;
        hirom = SNES_HIROM;
        sram_bytes = SNES_SRAM_BYTES;
#else
        fprintf(stderr, "usage: snes_host rom.sfc [--frames n] [--input f] [--coverage f] [--interp]\n");
        return 2;
#endif
    } else {
        FILE *file = fopen(rom_path, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, size % 32768 == 512 ? 512 : 0, SEEK_SET); /* a 512-byte header, some collections carry one */
        rom_bytes = (uint32_t)(size - (size % 32768 == 512 ? 512 : 0));
        uint8_t *bytes = malloc(rom_bytes);
        if (!bytes || fread(bytes, 1, (size_t)rom_bytes, file) != rom_bytes) {
            fprintf(stderr, "truncated image: %s\n", rom_path);
            return 2;
        }
        fclose(file);
        rom = bytes;
        hirom = detect_hirom(rom, rom_bytes);
        sram_bytes = header_sram(rom, rom_bytes, hirom);
    }

    snes_init(rom, rom_bytes, hirom, sram_bytes);
    static const char *const names[12] = {
        "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r"
    };
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, names, 12)) return 2;
    for (unsigned f = 0; f < frames; f++) {
        unsigned keys = pc_input_keys(&script, f);
        uint16_t report = 0;
        for (unsigned i = 0; i < 12; i++) if (keys & (1u << i)) report |= (uint16_t)(0x8000u >> i);
        snes_set_joypad(report);
        snes_frame();
    }
    pc_input_free(&script);
    uint8_t *image = malloc(256u * 224u * 3u);
    if (!image) { fprintf(stderr, "cannot allocate image\n"); return 2; }
    memcpy(image, machine.screen, 256u * 224u * 3u); /* the frame as displayed */
    uint32_t image_hash = fnv(image, 256u * 224u * 3u, 2166136261u);
    if (capture) {
        FILE *out = fopen(capture, "wb");
        if (!out) { fprintf(stderr, "cannot write %s\n", capture); free(image); return 2; }
        fprintf(out, "P6\n256 224\n255\n");
        if (fwrite(image, 1, 256u * 224u * 3u, out) != 256u * 224u * 3u || fclose(out)) {
            fprintf(stderr, "cannot write capture\n"); free(image); return 2;
        }
    }
    free(image);

    uint32_t h = 2166136261u;
    /* registers field by field, as the other runtimes' explicit order */
    const uint8_t regs[16] = {
        (uint8_t)machine.cpu.a, (uint8_t)(machine.cpu.a >> 8),
        (uint8_t)machine.cpu.x, (uint8_t)(machine.cpu.x >> 8),
        (uint8_t)machine.cpu.y, (uint8_t)(machine.cpu.y >> 8),
        (uint8_t)machine.cpu.s, (uint8_t)(machine.cpu.s >> 8),
        (uint8_t)machine.cpu.d, (uint8_t)(machine.cpu.d >> 8),
        machine.cpu.k, machine.cpu.dbr, machine.cpu.p, machine.cpu.e,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(&machine.cpu.cycles, 4, h);
    h = fnv(machine.wram, sizeof machine.wram, h);
    h = fnv(machine.vram, sizeof machine.vram, h);
    h = fnv(machine.cgram, sizeof machine.cgram, h);
    h = fnv(machine.oam, sizeof machine.oam, h);
    h = fnv(machine.ppu_regs, sizeof machine.ppu_regs, h);
    h = fnv(machine.dma, sizeof machine.dma, h);
    h = fnv(machine.scroll, sizeof machine.scroll, h);
    h = fnv(machine.sram, machine.sram_bytes, h);
    const uint8_t state[] = {
        machine.cpu.mdr, (uint8_t)machine.cpu.nmi_enable,
        (uint8_t)machine.cpu.vblank_flag, (uint8_t)machine.cpu.nmi_pending,
        (uint8_t)machine.cpu.irq_line, (uint8_t)machine.cpu.halted,
        (uint8_t)machine.cpu.waiting, (uint8_t)machine.cpu.wram_addr,
        (uint8_t)(machine.cpu.wram_addr >> 8), (uint8_t)(machine.cpu.wram_addr >> 16),
        (uint8_t)machine.vmaddr, (uint8_t)(machine.vmaddr >> 8),
        (uint8_t)machine.cgaddr, (uint8_t)(machine.cgaddr >> 8),
        (uint8_t)machine.oamaddr, (uint8_t)(machine.oamaddr >> 8),
        machine.cg_latch, machine.oam_latch, machine.scroll_latch, machine.vblank, machine.hblank,
        machine.timer_latch,
        (uint8_t)machine.htime, (uint8_t)(machine.htime >> 8),
        (uint8_t)machine.vtime, (uint8_t)(machine.vtime >> 8),
        machine.timeup, machine.h_irq_en, machine.v_irq_en,
        machine.hdma_enable,
        machine.hdma_run[0], machine.hdma_run[1], machine.hdma_run[2], machine.hdma_run[3],
        machine.hdma_run[4], machine.hdma_run[5], machine.hdma_run[6], machine.hdma_run[7],
        machine.hdma_done[0], machine.hdma_done[1], machine.hdma_done[2], machine.hdma_done[3],
        machine.hdma_done[4], machine.hdma_done[5], machine.hdma_done[6], machine.hdma_done[7],
        machine.hdma_count[0], machine.hdma_count[1], machine.hdma_count[2], machine.hdma_count[3],
        machine.hdma_count[4], machine.hdma_count[5], machine.hdma_count[6], machine.hdma_count[7],
        machine.hdma_len[0], machine.hdma_len[1], machine.hdma_len[2], machine.hdma_len[3],
        machine.hdma_len[4], machine.hdma_len[5], machine.hdma_len[6], machine.hdma_len[7],
        machine.hdma_rep[0], machine.hdma_rep[1], machine.hdma_rep[2], machine.hdma_rep[3],
        machine.hdma_rep[4], machine.hdma_rep[5], machine.hdma_rep[6], machine.hdma_rep[7],
        (uint8_t)machine.joypad, (uint8_t)(machine.joypad >> 8),
        (uint8_t)machine.joy_auto, (uint8_t)(machine.joy_auto >> 8),
        (uint8_t)machine.joy_shift, (uint8_t)(machine.joy_shift >> 8),
        machine.joy_strobe, machine.joy_count, machine.joy_auto_enable, machine.joy_busy,
    };
    h = fnv(state, sizeof state, h);
    h = fnv(&machine.frames, 4, h);
    if (coverage) {
        FILE *out = fopen(coverage, "w");
        if (!out) { fprintf(stderr, "cannot write %s\n", coverage); return 2; }
        fprintf(out, "# m0boy SNES coverage: K:PC address, the contexts it ran under"
                     " (K, DBR, direct page, M/X/E flags; K lowest, up to %d),"
                     " 65816 instructions started,"
                     " d = reached by RTS/RTL/RTI/a jump through a pointer/an interrupt\n", SNES_CTX_MAX);
        for (uint32_t i = 0; i < COV_SLOTS; i++) {
            if (!cov || !cov[i].pc) continue;
            fprintf(out, "%06x", cov[i].pc);
            for (unsigned c = 0; c < cov[i].ctx_n; c++)
                fprintf(out, " %010llx", (unsigned long long)cov[i].ctx[c]);
            fprintf(out, " %u%s\n", (unsigned)cov[i].hits, cov[i].dyn ? " d" : "");
        }
        fclose(out);
    }
    printf("frames=%u cycles=%u k=%02x pc=%04x run_hash=%08x image_hash=%08x sram_dirty=%u\n",
           machine.frames, (unsigned)machine.cpu.cycles, machine.cpu.k, machine.cpu.pc, h, image_hash,
           machine.sram_dirty);
    return 0;
}
