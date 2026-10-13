/* Game Gear and Master System host harness (.sms files, or GG_SMS projects): coverage profiling (interpreter), native/interpreter
 * comparison (run_hash over CPU, memory, devices and the picture), captures
 * and sound. */
#include "gg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SG_GENERATED
#include "sg_config.h"
extern const uint8_t sg_rom[];
extern const size_t sg_rom_size;
sg_fn sg_gen_lookup(uint16_t pc);
#endif

static uint16_t image[GG_ACTIVE][GG_WIDTH]; /* Game Gear: the first 144 rows */
static uint16_t row_buffer[GG_ROW_STRIDE] __attribute__((aligned(4))), tmp_buffer[GG_ROW_STRIDE] __attribute__((aligned(4)));
static uint8_t cart_ram[GG_CART_RAM_SIZE];
static FILE *audio;
static uint32_t audio_samples;
static int audio_error;
static unsigned frames = 300, buttons, start_frame = ~0u;
static uint32_t *hits;
static uint8_t *entries;
static uint32_t *coverage_order;
#ifdef GG_ROM_TRACE
/* Profiling: frames in which each 512-byte ROM sector was read as data
 * (the generator keeps the most read pages in flash). */
static uint32_t *read_frames, *read_last, rom_sectors;
void gg_rom_trace(uint32_t offset) {
    const uint32_t s = offset >> 9;
    if (!read_frames || s >= rom_sectors || read_last[s] == machine.frames + 1) return;
    read_last[s] = machine.frames + 1;
    read_frames[s]++;
}
#endif

static void le32(FILE *f, uint32_t v) { for (unsigned i = 0; i < 4; i++) fputc((v >> (8 * i)) & 255, f); }
static void wav_header(FILE *f, uint32_t n) {
    fwrite("RIFF", 1, 4, f); le32(f, 36 + n); fwrite("WAVEfmt ", 1, 8, f); le32(f, 16);
    fputc(1, f); fputc(0, f); fputc(1, f); fputc(0, f); le32(f, 22050); le32(f, 22050);
    fputc(1, f); fputc(0, f); fputc(8, f); fputc(0, f); fwrite("data", 1, 4, f); le32(f, n);
}
/* --input "frame:mask:frames,...": buttons held from frame for frames
 * (bits: up, down, left, right, 1, 2, START) */
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
/* --random seed: buttons redrawn every 16 frames, START now and then */
static uint32_t random_state;
static unsigned random_keys;
static unsigned randomized(unsigned frame) {
    if (!random_state) return 0;
    if (frame % 16 == 0) {
        random_state = random_state * 1103515245u + 12345u;
        random_keys = (random_state >> 16) & 63;
        if (((random_state >> 24) & 15) == 0 && !GG_IS_SMS) random_keys |= GG_START;
    }
    return random_keys;
}
static void inputs(void) {
    unsigned keys = buttons | scripted(machine.frames) | randomized(machine.frames);
    if (machine.frames >= start_frame && machine.frames < start_frame + 10) keys |= GG_IS_SMS ? GG_B1 : GG_START;
    gg_buttons((uint8_t)keys);
}
static void frame(void *ctx) {
    (void)ctx;
    if (audio) {
        uint8_t buffer[512];
        unsigned n = gg_audio_available();
        gg_audio_render(buffer, n);
        if (fwrite(buffer, 1, n, audio) != n) audio_error = 1;
        audio_samples += n;
    }
    if (machine.frames >= frames) machine.stop = 1;
    else inputs();
}
static void row(void *ctx, unsigned y, uint16_t *pixels) {
    (void)ctx;
    for (unsigned x = 0; x < GG_WIDTH; x++) /* true RGB565 whatever the panel order */
#if defined(GG_PANEL_SWAP) && GG_PANEL_SWAP
        image[y - GG_WIN_TOP][x] = (uint16_t)(pixels[x] >> 8 | pixels[x] << 8);
#else
        image[y - GG_WIN_TOP][x] = pixels[x];
#endif
}
static int hot_first(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    if (hits[x] != hits[y]) return hits[x] > hits[y] ? -1 : 1;
    return x < y ? -1 : x > y;
}
static uint32_t hash_bytes(uint32_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}
static uint32_t run_hash(void) {
    gg_t *s = &machine;
    uint32_t h = 2166136261u;
    uint16_t registers[] = { s->cpu.bc, s->cpu.de, s->cpu.hl, s->cpu.bc2, s->cpu.de2, s->cpu.hl2, s->cpu.af2,
                             s->cpu.ix, s->cpu.iy, s->cpu.sp, s->cpu.pc };
    for (unsigned i = 0; i < sizeof registers / sizeof *registers; i++) {
        uint8_t bytes[] = { (uint8_t)registers[i], (uint8_t)(registers[i] >> 8) };
        h = hash_bytes(h, bytes, 2);
    }
    uint8_t flags[] = { s->cpu.a, s->cpu.f, s->cpu.i, s->cpu.r, s->cpu.im, s->cpu.iff1, s->cpu.iff2, s->cpu.halted,
                        s->cpu.ei_delay, s->vdp_status, s->vdp_latch, s->vdp_pending, s->vdp_buffer, s->vdp_code,
                        s->cram_latch, s->line_irq, s->lc_count, s->cpu.irq, s->cpu.nmi, s->cpu.fault,
                        s->page[0], s->page[1], s->page[2], s->ram_ctl };
    h = hash_bytes(h, flags, sizeof flags);
    uint32_t counters[] = { s->cyc, s->frames, s->render_line, s->vdp_address };
    h = hash_bytes(h, counters, sizeof counters);
    h = hash_bytes(h, s->ram, sizeof s->ram);
    h = hash_bytes(h, s->vram, sizeof s->vram);
    h = hash_bytes(h, s->cram, sizeof s->cram);
    h = hash_bytes(h, s->vdp, sizeof s->vdp);
    h = hash_bytes(h, s->psg_tone, sizeof s->psg_tone);
    h = hash_bytes(h, s->psg_volume, sizeof s->psg_volume);
    h = hash_bytes(h, &s->psg_noise, 1);
    h = hash_bytes(h, &s->psg_latch, 1);
    if (s->cart_ram && s->cart_ram_used) h = hash_bytes(h, s->cart_ram, GG_CART_RAM_SIZE); /* mapped only when enabled */
    return hash_bytes(h, image, sizeof image);
}

static int sms __attribute__((unused));
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
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(argv[i], "--random") && i + 1 < argc) random_state = (uint32_t)strtoul(argv[++i], 0, 0) | 1u;
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
    if (!rom_path) { fprintf(stderr, "Usage: gg_host cartridge.gg|.sms [--frames N] [--shot file.ppm] [--coverage file]\n"); return 1; }
    const char *dot = strrchr(rom_path, '.');
    sms = dot && (!strcmp(dot, ".sms") || !strcmp(dot, ".SMS"));
    FILE *file = fopen(rom_path, "rb");
    if (!file) { perror(rom_path); return 1; }
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0x400 || ftell(file) > 0x400000) { fclose(file); fprintf(stderr, "Unsupported cartridge size\n"); return 1; }
    size = (size_t)ftell(file);
    long skip = (size & 0x3fff) == 512 ? 512 : 0; /* copier header */
    size -= (size_t)skip;
    fseek(file, skip, SEEK_SET);
    loaded = malloc((size + 0x3fff) & ~(size_t)0x3fff); /* whole pages, padded with FF */
    if (!loaded) { fclose(file); return 1; }
    memset(loaded, 0xff, (size + 0x3fff) & ~(size_t)0x3fff);
    if (fread(loaded, 1, size, file) != size) { fclose(file); free(loaded); return 1; }
    fclose(file);
    rom = loaded;
#endif
    FILE *coverage = 0;
    const uint32_t locations = (uint32_t)((size + 0x3fff) / 0x4000) << 16;
    if (cov_path) {
        coverage = fopen(cov_path, "w");
        hits = calloc(locations, sizeof *hits);
        entries = calloc(locations, 1);
        if (!coverage || !hits || !entries) { perror(cov_path); free(loaded); return 1; }
        machine.coverage = hits;
        machine.entries = entries;
        machine.coverage_size = locations;
#ifdef GG_ROM_TRACE
        rom_sectors = (uint32_t)((size + 511) / 512);
        read_frames = calloc(rom_sectors, sizeof *read_frames);
        read_last = calloc(rom_sectors, sizeof *read_last);
#endif
    }
#ifdef SG_GENERATED
    gg_init(rom, size, cart);
#else
    gg_init_system(rom, size, cart, sms);
#endif
    machine.row = row;
    machine.row_out = row_buffer + GG_ROW_PAD;
    machine.row_tmp = tmp_buffer + GG_ROW_PAD;
    machine.frame = frame;
    if (wav_path) {
        audio = fopen(wav_path, "wb");
        if (!audio) { perror(wav_path); if (coverage) fclose(coverage); free(loaded); return 1; }
        wav_header(audio, 0);
        gg_audio_rate(22050);
    }
    inputs();
    if (frames) gg_run();
    if (audio) {
        if (audio_samples & 1) fputc(0, audio);
        if (fseek(audio, 0, SEEK_SET)) audio_error = 1;
        wav_header(audio, audio_samples);
        if (audio_samples & 1) { fseek(audio, 4, SEEK_SET); le32(audio, 36 + audio_samples + 1); }
        if (ferror(audio)) audio_error = 1;
        if (fclose(audio)) audio_error = 1;
        if (audio_error) { perror("audio"); if (coverage) fclose(coverage); free(loaded); return 1; }
    }
    if (coverage) {
        fprintf(coverage, GG_IS_SMS ? "# m0boy.sms.coverage.v1\n" : "# m0boy.gg.coverage.v1\n");
        if (machine.cart_ram_used) fprintf(coverage, "# cart-ram\n");
        for (uint32_t loc = 0; loc < locations; loc++) /* dynamic jump targets: region entries */
            if (entries[loc]) fprintf(coverage, "# entry %06x\n", loc);
#ifdef GG_ROM_TRACE
        for (uint32_t sec = 0; sec < rom_sectors; sec++) /* data reads: sector, frames */
            if (read_frames[sec]) fprintf(coverage, "# read %04x %u\n", sec, read_frames[sec]);
#endif
        uint32_t n = 0;
        coverage_order = malloc(sizeof *coverage_order * locations);
        if (!coverage_order) return 1;
        for (uint32_t loc = 0; loc < locations; loc++) if (hits[loc]) coverage_order[n++] = loc;
        qsort(coverage_order, n, sizeof *coverage_order, hot_first);
        for (uint32_t i = 0; i < n; i++)
            if (fprintf(coverage, "%06x\n", coverage_order[i]) < 0) { fclose(coverage); free(loaded); return 1; }
        if (fclose(coverage)) { perror("coverage"); free(loaded); return 1; }
    }
    if (shot) {
        FILE *f = fopen(shot, "wb");
        if (!f) { perror(shot); free(loaded); return 1; }
        fprintf(f, "P6\n%d %u\n255\n", GG_WIDTH, GG_WIN_LINES);
        for (unsigned y = 0; y < GG_WIN_LINES; y++)
            for (unsigned x = 0; x < GG_WIDTH; x++) {
                const unsigned c = image[y][x];
                const unsigned r = c >> 11, g = (c >> 5) & 63, b = c & 31;
                uint8_t p[] = { (uint8_t)(r << 3 | r >> 2), (uint8_t)(g << 2 | g >> 4), (uint8_t)(b << 3 | b >> 2) };
                if (fwrite(p, 1, 3, f) != 3) { fclose(f); free(loaded); return 1; }
            }
        if (fclose(f)) { free(loaded); return 1; }
    }
    printf("frames=%u cycles=%u pc=%04x page=%u,%u,%u compiled=%u interpreted=%u run_hash=%08x\n",
           machine.frames, machine.cyc, machine.cpu.pc, machine.page[0], machine.page[1], machine.page[2],
           machine.compiled, machine.interpreted, run_hash());
    free(loaded);
    return machine.cpu.fault ? 1 : 0;
}
