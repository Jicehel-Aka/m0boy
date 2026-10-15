/* SG-1000 host harness: coverage profiling (interpreter), native/interpreter
 * comparison (run_hash) and the device renderer checked against the
 * reference one on every displayed line. */
#include "sg1000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SG_GENERATED
#include "sg_config.h"
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#endif

static uint8_t image[SG_HEIGHT][SG_WIDTH];
static uint16_t out_row[SG_OUT_WIDTH];
static unsigned out_mismatch;
static uint8_t cart_ram[SG_CART_RAM_SIZE];
static FILE *audio;
static uint32_t audio_samples;
static int audio_error;
static unsigned frames = 300, buttons, start_frame = ~0u, pause_frame = ~0u, explore_frames;
static uint32_t hits[65536];
static uint8_t entries[65536];
static uint16_t coverage_order[65536];

static void le32(FILE *f, uint32_t v) { for (unsigned i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static void wav_header(FILE *f, uint32_t n) {
    fwrite("RIFF", 1, 4, f); le32(f, 36 + n); fwrite("WAVEfmt ", 1, 8, f); le32(f, 16);
    fputc(1, f); fputc(0, f); fputc(1, f); fputc(0, f); le32(f, 22050); le32(f, 22050);
    fputc(1, f); fputc(0, f); fputc(8, f); fputc(0, f); fwrite("data", 1, 4, f); le32(f, n);
}
/* --input "frame:mask:frames,...": buttons held from frame for frames */
static const char *input_script;
static unsigned scripted(unsigned frame) {
    unsigned keys = 0;
    for (const char *p = input_script; p && *p;) {
        unsigned at = 0, mask = 0, len = 0;
        if (sscanf(p, "%u:%u:%u", &at, &mask, &len) == 3 && frame >= at && frame < at + len) keys |= mask;
        p = strchr(p, ',');
        if (p) p++;
    }
    return keys;
}
/* --random seed: buttons redrawn every 16 frames (coverage profiling) */
static uint32_t random_state;
static unsigned random_keys;
static unsigned randomized(unsigned frame) {
    if (!random_state) return 0;
    if (frame % 16 == 0) {
        random_state = random_state * 1103515245u + 12345u;
        random_keys = (random_state >> 16) & 63;
    }
    return random_keys;
}
static void inputs(void) {
    unsigned keys = buttons | scripted(machine.frames) | randomized(machine.frames);
    if (machine.frames >= start_frame && machine.frames < start_frame + 10) keys |= 16;
    sg1000_buttons(0, (uint8_t)keys);
    sg1000_pause(machine.frames == pause_frame);
}
#ifndef SG_GENERATED
/* --explore frames: the coverage-guided player of m0_explore.h, from the
 * start or from snapshots of the machine (and cartridge RAM) that reached
 * code never seen.  No START: the console's pause button is not on the pad. */
#include "m0_explore.h"
typedef struct { sg1000_t machine; uint8_t cart[SG_CART_RAM_SIZE]; } snapshot_t;
static m0x_t explorer;
static unsigned explore_on;
static void snapshot_save(void *ctx, void *state) {
    snapshot_t *s = state;
    (void)ctx;
    memcpy(&s->machine, &machine, sizeof machine);
    memcpy(s->cart, cart_ram, sizeof cart_ram);
}
static void snapshot_load(void *ctx, const void *state) {
    const snapshot_t *s = state;
    (void)ctx;
    memcpy(&machine, &s->machine, sizeof machine);
    memcpy(cart_ram, s->cart, sizeof cart_ram);
    machine.sprites_dirty = 1; /* sprite masks derived from the tables */
}
static uint32_t explore_novelty(void) { /* program counters reached so far */
    uint32_t n = 0;
    for (unsigned pc = 0; pc < 65536; pc++) n += hits[pc] != 0;
    return n;
}
static void explore(unsigned total) {
    const m0x_machine_t m = { 0, sizeof(snapshot_t), snapshot_save, snapshot_load, M0X_NO_START };
    machine.row = 0; /* nothing is drawn: the game does not need it */
    machine.ref_row = 0;
    if (!m0x_begin(&explorer, &m, random_state, total)) return;
    explore_on = 1;
    do {
        machine.stop = 0;
        sg1000_run();
    } while (m0x_next(&explorer, explore_novelty()));
    fprintf(stderr, "explore: %u frames, %u segments, %u snapshots, %u locations\n", explorer.frames,
            explorer.segments, explorer.pool_size, explore_novelty());
}
#endif
static void frame(void *ctx) {
    (void)ctx;
#ifndef SG_GENERATED
    if (explore_on) {
        const unsigned k = m0x_keys(&explorer);
        if (k == M0X_END) machine.stop = 1;
        else sg1000_buttons(0, (uint8_t)((k & M0X_UP ? 1 : 0) | (k & M0X_DOWN ? 2 : 0) | (k & M0X_LEFT ? 4 : 0) |
                                         (k & M0X_RIGHT ? 8 : 0) | (k & M0X_B1 ? 16 : 0) | (k & M0X_B2 ? 32 : 0)));
        return;
    }
#endif
    if (audio) {
        uint8_t buffer[512];
        unsigned n = sg1000_audio_available();
        sg1000_audio_render(buffer, n);
        if (fwrite(buffer, 1, n, audio) != n) audio_error = 1;
        audio_samples += n;
    }
    if (machine.frames >= frames) machine.stop = 1;
    else inputs();
}
static void row(void *ctx, unsigned y, uint16_t *pixels) { (void)ctx; (void)y; (void)pixels; /* already in out_row */ }
static void ref_row(void *ctx, unsigned y, const uint8_t *pixels) {
    (void)ctx;
    memcpy(image[y], pixels, SG_WIDTH);
    uint16_t expected[SG_OUT_WIDTH];
    sg1000_filter_line(pixels, expected);
    if (memcmp(out_row, expected, sizeof expected)) out_mismatch++;
}
static int hot_first(const void *a, const void *b) {
    unsigned x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    if (hits[x] != hits[y]) return hits[x] > hits[y] ? -1 : 1;
    return x < y ? -1 : x > y;
}
static uint32_t hash_bytes(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static uint32_t run_hash(void) {
    sg1000_t *s = &machine;
    uint32_t h = 2166136261u;
    uint16_t registers[] = { s->cpu.bc, s->cpu.de, s->cpu.hl, s->cpu.bc2, s->cpu.de2, s->cpu.hl2, s->cpu.af2,
                             s->cpu.ix, s->cpu.iy, s->cpu.sp, s->cpu.pc };
    for (unsigned i = 0; i < sizeof registers / sizeof *registers; i++) {
        uint8_t bytes[] = { (uint8_t)registers[i], (uint8_t)(registers[i] >> 8) };
        h = hash_bytes(h, bytes, 2);
    }
    uint8_t flags[] = { s->cpu.a, s->cpu.f, s->cpu.i, s->cpu.r, s->cpu.im, s->cpu.iff1, s->cpu.iff2, s->cpu.halted,
                        s->cpu.ei_delay, s->vdp_status, s->vdp_latch, s->vdp_pending, s->vdp_buffer, s->cpu.irq,
                        s->cpu.nmi, s->cpu.fault, s->pause };
    h = hash_bytes(h, flags, sizeof flags);
    uint32_t counters[] = { s->cyc, s->frames, s->render_line, s->vdp_address };
    h = hash_bytes(h, counters, sizeof counters);
    h = hash_bytes(h, s->ram, sizeof s->ram);
    h = hash_bytes(h, s->vram, sizeof s->vram);
    h = hash_bytes(h, s->vdp, sizeof s->vdp);
    h = hash_bytes(h, s->buttons, sizeof s->buttons);
    h = hash_bytes(h, s->psg_tone, sizeof s->psg_tone);
    h = hash_bytes(h, s->psg_volume, sizeof s->psg_volume);
    h = hash_bytes(h, &s->psg_noise, 1);
    h = hash_bytes(h, &s->psg_latch, 1);
    /* cartridge RAM: the generic harness always maps it; generated projects
     * only when the profile saw it written */
#ifdef SG_GENERATED
    if (SG_CART_RAM) h = hash_bytes(h, s->cart_ram, SG_CART_RAM_SIZE);
#else
    if (s->cart_ram_used) h = hash_bytes(h, s->cart_ram, SG_CART_RAM_SIZE);
#endif
    return hash_bytes(h, image, sizeof image);
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *shot = 0, *cov_path = 0, *wav_path = 0;
    int interp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) cov_path = argv[++i];
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) buttons = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--start-frame") && i + 1 < argc) start_frame = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--pause-frame") && i + 1 < argc) pause_frame = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) random_state = (uint32_t)strtoul(argv[++i], 0, 0) | 1u;
#ifndef SG_GENERATED
        else if (!strcmp(argv[i], "--explore") && i + 1 < argc) explore_frames = (unsigned)strtoul(argv[++i], 0, 0);
#endif
        else if (argv[i][0] != '-' && !rom_path) rom_path = argv[i];
        else { fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]); return 1; }
    }
    uint8_t *loaded = 0;
    const uint8_t *rom;
    size_t size;
    uint8_t *cart = cart_ram;
#ifdef SG_GENERATED
    rom = sg_rom; size = sg_rom_size;
    if (rom_path) { fprintf(stderr, "Generated harness already contains its cartridge\n"); return 1; }
    if (!SG_CART_RAM) cart = 0;
    if (!interp) machine.lookup = sg_gen_lookup;
#else
    (void)interp;
    if (!rom_path) { fprintf(stderr, "Usage: sg1000_host cartridge.sg [--frames N] [--shot file.ppm] [--coverage file]\n"); return 1; }
    FILE *file = fopen(rom_path, "rb");
    if (!file) { perror(rom_path); return 1; }
    if (fseek(file, 0, SEEK_END) || ftell(file) <= 0 || ftell(file) > 0xc000) { fclose(file); fprintf(stderr, "Unsupported cartridge size\n"); return 1; }
    size = (size_t)ftell(file);
    rewind(file);
    loaded = malloc(size);
    if (!loaded || fread(loaded, 1, size, file) != size) { fclose(file); free(loaded); return 1; }
    fclose(file);
    rom = loaded;
#endif
    FILE *coverage = 0;
    if (cov_path) {
        coverage = fopen(cov_path, "w");
        if (!coverage) { perror(cov_path); free(loaded); return 1; }
    }
    sg1000_init(rom, size, cart);
    machine.row = row;
    machine.row_out = out_row;
    machine.ref_row = ref_row;
    machine.frame = frame;
    if (coverage) { machine.coverage = hits; machine.entries = entries; }
    if (wav_path) {
        audio = fopen(wav_path, "wb");
        if (!audio) { perror(wav_path); if (coverage) fclose(coverage); free(loaded); return 1; }
        wav_header(audio, 0);
        sg1000_audio_rate(22050);
    }
    inputs();
#ifndef SG_GENERATED
    if (explore_frames && cov_path) { explore(explore_frames); machine.frames = explorer.frames; }
    else
#endif
    if (frames) sg1000_run();
    if (audio) {
        if (audio_samples & 1) fputc(0, audio); /* RIFF chunks are padded to an even size */
        if (fseek(audio, 0, SEEK_SET)) audio_error = 1;
        wav_header(audio, audio_samples);
        if (audio_samples & 1) { fseek(audio, 4, SEEK_SET); le32(audio, 36 + audio_samples + 1); }
        if (ferror(audio)) audio_error = 1;
        if (fclose(audio)) audio_error = 1;
        if (audio_error) { perror("audio"); if (coverage) fclose(coverage); free(loaded); return 1; }
    }
    if (coverage) {
        /* v2: every address line carries its execution count, so the
         * recompiler ranks heat across sessions instead of file order */
        fprintf(coverage, "# m0boy.sg1000.coverage.v2\n");
        if (machine.cart_ram_used) fprintf(coverage, "# cart-ram\n");
        for (unsigned pc = 0; pc < 65536; pc++) /* dynamic jump targets: region entries */
            if (entries[pc]) fprintf(coverage, "# entry %04x\n", pc);
        unsigned n = 0;
        for (unsigned pc = 0; pc < 65536; pc++) if (hits[pc]) coverage_order[n++] = (uint16_t)pc;
        qsort(coverage_order, n, sizeof *coverage_order, hot_first);
        for (unsigned i = 0; i < n; i++)
            if (fprintf(coverage, "%04x %u\n", coverage_order[i], hits[coverage_order[i]]) < 0) { fclose(coverage); free(loaded); return 1; }
        if (fclose(coverage)) { perror("coverage"); free(loaded); return 1; }
    }
    if (shot) {
        FILE *f = fopen(shot, "wb");
        if (!f) { perror(shot); free(loaded); return 1; }
        fprintf(f, "P6\n256 192\n255\n");
        for (unsigned y = 0; y < SG_HEIGHT; y++)
            for (unsigned x = 0; x < SG_WIDTH; x++) {
                uint32_t rgb = sg1000_palette[image[y][x] & 15];
                uint8_t p[] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
                if (fwrite(p, 1, 3, f) != 3) { fclose(f); free(loaded); return 1; }
            }
        if (fclose(f)) { free(loaded); return 1; }
    }
    printf("frames=%u cycles=%u pc=%04x compiled=%u interpreted=%u out_mismatch=%u run_hash=%08x\n",
           machine.frames, machine.cyc, machine.cpu.pc, machine.compiled, machine.interpreted, out_mismatch, run_hash());
    free(loaded);
    return machine.cpu.fault || out_mismatch ? 1 : 0;
}
