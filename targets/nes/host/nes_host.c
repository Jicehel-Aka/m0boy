/* NES host harness: an iNES cartridge in the machine, N frames out, the
 * last frame as a PPM, the sound as a WAV, and the run's fingerprint
 * (CPU, RAM, VRAM, palette, OAM — the same contract shape as the other
 * runtimes).  Built with the recompiled code (NES_NATIVE), --interp runs
 * the interpreter instead: the reference a precise project must match.
 * --coverage writes the interpreter's executions per PRG address.
 * Built with NES_PPU_REFERENCE: every panel row (160x128, ppu.h) is checked
 * against the reference lines it comes from (out_mismatch, rows that
 * differ; hit_mismatch, lines whose sprite-0 differs); --skip n leaves out
 * one frame in n from the panel, as a console's pacing does; --rows n
 * takes another vertical plan (ppu.h: 176 on the Pokitto). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nes.h"
#define M0_PANEL_SWAP 0 /* the harness's projects are desktop ones (nes_config.h) */
#include "m0_scale.h"
#ifdef NES_EMBEDDED
#include "../src/nes_config.h"
extern const uint8_t nes_prg[];
extern const uint8_t nes_chr[];
#endif

#define AUDIO_RATE 11025u

#ifdef NES_NATIVE
extern int nes_native;
#endif
#ifdef NES_COVERAGE
extern uint32_t nes_coverage_hits[0x8000];
extern uint8_t nes_coverage_dynamic[0x8000];
#endif

static uint16_t frame[240][256];
static void on_row(void *host, unsigned line, const uint16_t *pixels) {
    (void)host;
    /* the row is the fetched stream: screen pixel c lives at c + fine x */
    if (line < 240)
        for (unsigned x = 0; x < 256; x++)
            frame[line][x] = pixels[x + machine.ppu.x];
}

/* the panel row against its reference lines (the plan's, ppu.h: the
 * lines y with m0_row_of(y, 240, rows) == r): each through m0_box58_fast,
 * the two of a pair averaged */
static uint16_t panel[160];
static uint32_t out_rows, out_mismatch;
static unsigned rows = 128;
static void on_panel(void *host, unsigned r) {
    (void)host;
    const unsigned a = m0_row_first(r, rows, 240);
    const unsigned pair = a + 1 < 240 && m0_row_of(a + 1, 240, rows) == r;
    uint16_t want[160], other[160];
    for (unsigned x = 0; x < 256; x += 8) {
        uint32_t s[8];
        for (unsigned i = 0; i < 8; i++) s[i] = m0_spread565(frame[a][x + i]);
        m0_box58_fast(s, want + x / 8 * 5);
        if (!pair) continue;
        for (unsigned i = 0; i < 8; i++) s[i] = m0_spread565(frame[a + 1][x + i]);
        m0_box58_fast(s, other + x / 8 * 5);
    }
    if (pair)
        for (unsigned i = 0; i < 160; i++) want[i] = m0_avg2_panel(want[i], other[i]);
    out_rows++;
    if (memcmp(want, panel, sizeof want)) out_mismatch++;
}

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void state_read(void *data, uint32_t bytes, void *ctx) {
    if (fread(data, 1, bytes, (FILE *)ctx) != bytes) {
        fprintf(stderr, "state file too short\n");
        exit(2);
    }
}
static void state_write(void *data, uint32_t bytes, void *ctx) {
    if (fwrite(data, 1, bytes, (FILE *)ctx) != bytes) {
        fprintf(stderr, "cannot write state\n");
        exit(2);
    }
}

int main(int argc, char **argv) {
    const char *rom_path = 0, *ppm = 0, *wav = 0, *input_path = 0, *coverage = 0;
    const char *save_state = 0, *load_state = 0;
    int interp = 0;
    unsigned frames = 60, skip = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) save_state = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc) skip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else rom_path = argv[i];
    }
    uint8_t header[16] = {0};
    uint8_t *prg, *chr;
    uint32_t prg_bytes, chr_bytes;
    if (!rom_path) {
#ifdef NES_EMBEDDED
        prg = (uint8_t *)nes_prg; chr = (uint8_t *)nes_chr;
        prg_bytes = NES_PRG_SIZE; chr_bytes = NES_CHR_SIZE;
        header[6] = (uint8_t)(NES_VERTICAL_MIRROR ? 1 : 0);
#else
        fprintf(stderr, "usage: nes_host rom.nes [--frames n] [--ppm f] [--wav f] [--input f]\n");
        return 2;
#endif
    } else {
    FILE *file = fopen(rom_path, "rb");
    if (!file) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
    if (fread(header, 1, 16, file) != 16 || memcmp(header, "NES\x1a", 4)) {
        fprintf(stderr, "not an iNES image: %s\n", rom_path);
        return 2;
    }
    prg_bytes = header[4] * 16384u; chr_bytes = header[5] * 8192u;
    prg = malloc(prg_bytes); chr = malloc(chr_bytes ? chr_bytes : 8192);
    if (fread(prg, 1, prg_bytes, file) != prg_bytes) { fprintf(stderr, "truncated PRG\n"); return 2; }
    if (chr_bytes && fread(chr, 1, chr_bytes, file) != chr_bytes) { fprintf(stderr, "truncated CHR\n"); return 2; }
    fclose(file);
    }

#ifdef NES_NATIVE
    /* the profile counts the interpreter's steps: every instruction then */
    nes_native = !(interp || coverage);
#endif
#ifndef NES_COVERAGE
    if (coverage) { fprintf(stderr, "built without NES_COVERAGE\n"); return 2; }
#endif
    (void)interp;
    nes_init(prg, prg_bytes, chr, chr_bytes, header[6] & 1);
    machine.row = on_row;
    machine.ppu.panel = on_panel;
    machine.ppu.panel_out = panel;
    if (rows < 120 || rows > 240) { fprintf(stderr, "--rows: 120 to 240 (128 the META, 176 the Pokitto)\n"); return 2; }
    nes_ppu_set_rows(&machine.ppu, rows);
    if (load_state) {
        FILE *in = fopen(load_state, "rb");
        if (!in) { perror(load_state); return 2; }
        nes_state_io(state_read, in, 1);
        fclose(in);
    }
    /* a held-input script: "frame buttons" lines, buttons by name */
    struct { unsigned frame; uint8_t buttons; } script[4096];
    unsigned script_n = 0;
    if (input_path) {
        FILE *in = fopen(input_path, "r");
        if (!in) { fprintf(stderr, "cannot read %s\n", input_path); return 2; }
        unsigned at; char names[64];
        while (script_n < 4096 && fscanf(in, "%u %63[^\n]\n", &at, names) == 2) {
            uint8_t b = 0;
            for (char *tok = strtok(names, "+ "); tok; tok = strtok(NULL, "+ ")) {
                if (!strcmp(tok, "start")) b |= NES_START;
                else if (!strcmp(tok, "a")) b |= NES_A;
                else if (!strcmp(tok, "b")) b |= NES_B;
                else if (!strcmp(tok, "up")) b |= NES_UP;
                else if (!strcmp(tok, "down")) b |= NES_DOWN;
                else if (!strcmp(tok, "left")) b |= NES_LEFT;
                else if (!strcmp(tok, "right")) b |= NES_RIGHT;
                else if (!strcmp(tok, "select")) b |= NES_SELECT;
                else if (strcmp(tok, "none")) {
                    fprintf(stderr, "unknown NES button: %s\n", tok);
                    fclose(in);
                    return 2;
                }
            }
            script[script_n].frame = at;
            script[script_n++].buttons = b;
        }
        fclose(in);
    }
    FILE *audio = wav ? fopen(wav, "wb") : 0;
    if (audio) { /* the header is patched once the length is known */
        const char silence[44] = {0};
        fwrite(silence, 1, 44, audio);
    }
    uint32_t audio_bytes = 0;
    for (unsigned f = 0; f < frames; f++) {
        machine.input = 0;
        for (unsigned i = 0; i < script_n; i++)
            if (script[i].frame == machine.frames) machine.input = script[i].buttons;
        machine.ppu.skip = skip && machine.frames % skip == skip - 1;
        nes_frame();
        if (audio) {
            uint8_t samples[256];
            /* 29780.5 CPU cycles per frame: the samples that covers */
            static unsigned carry;
            carry += AUDIO_RATE * 29780u;
            unsigned count = carry / 1789773u;
            carry %= 1789773u;
            if (count > sizeof samples) count = sizeof samples;
            nes_audio(samples, count, AUDIO_RATE);
            fwrite(samples, 1, count, audio);
            audio_bytes += count;
        }
    }
    if (save_state) {
        FILE *out = fopen(save_state, "wb");
        if (!out) { perror(save_state); return 2; }
        nes_state_io(state_write, out, 0);
        if (fclose(out)) { perror(save_state); return 2; }
    }
    if (audio) {
        fseek(audio, 0, SEEK_SET);
        fprintf(audio, "RIFF");
        const uint32_t riff = 36 + audio_bytes;
        fwrite(&riff, 4, 1, audio);
        fprintf(audio, "WAVEfmt ");
        const uint32_t fmt = 16;
        fwrite(&fmt, 4, 1, audio);
        const uint16_t pcm = 1, ch = 1;
        fwrite(&pcm, 2, 1, audio);
        fwrite(&ch, 2, 1, audio);
        const uint32_t rate = AUDIO_RATE, byte_rate = AUDIO_RATE;
        fwrite(&rate, 4, 1, audio);
        fwrite(&byte_rate, 4, 1, audio);
        const uint16_t block = 1, bits = 8;
        fwrite(&block, 2, 1, audio);
        fwrite(&bits, 2, 1, audio);
        fprintf(audio, "data");
        fwrite(&audio_bytes, 4, 1, audio);
        fclose(audio);
    }
    if (ppm) {
        FILE *out = fopen(ppm, "wb");
        if (out) {
            fprintf(out, "P6\n256 240\n255\n");
            for (unsigned y = 0; y < 240; y++)
                for (unsigned x = 0; x < 256; x++) {
                    const uint16_t c = frame[y][x];
                    const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8), (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                    fwrite(px, 1, 3, out);
                }
            fclose(out);
        }
    }
#ifdef NES_COVERAGE
    if (coverage) {
        FILE *out = fopen(coverage, "w");
        if (!out) { fprintf(stderr, "cannot write %s\n", coverage); return 2; }
        fprintf(out, "# m0boy NES coverage: address executions [d = reached by RTS/RTI/JMP (ind)/interrupt]\n");
        for (unsigned i = 0; i < 0x8000; i++)
            if (nes_coverage_hits[i])
                fprintf(out, "%04X %u%s\n", 0x8000u + i, (unsigned)nes_coverage_hits[i], nes_coverage_dynamic[i] ? " d" : "");
        fclose(out);
    }
#endif
    uint32_t h = 2166136261u;
    /* registers field by field: the struct now carries the bus's fast-path
     * pointers, and hashing its bytes would hash addresses and padding */
    const uint8_t regs[12] = {
        machine.cpu.a, machine.cpu.x, machine.cpu.y, machine.cpu.s, machine.cpu.p,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
        (uint8_t)machine.cpu.cycles, (uint8_t)(machine.cpu.cycles >> 8),
        (uint8_t)(machine.cpu.cycles >> 16), (uint8_t)(machine.cpu.cycles >> 24),
        (uint8_t)(machine.ppu.frame & 0xFFu),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(machine.ram, sizeof machine.ram, h);
    h = fnv(machine.ppu.nametable, 2048, h);
    h = fnv(machine.ppu.palette, 32, h);
    h = fnv(machine.oam, 256, h);
    h = fnv(frame, sizeof frame, h);
#ifdef NES_GEN_STATS
    {
        extern uint32_t nes_gen_calls, nes_gen_steps;
        printf("dispatch: %u calls into recompiled code, %u interpreted steps\n",
               (unsigned)nes_gen_calls, (unsigned)nes_gen_steps);
    }
#endif
    printf("frames=%u cycles=%u pc=%04x frame=%u run_hash=%08x\n",
           frames, (unsigned)machine.cpu.cycles, machine.cpu.pc, machine.ppu.frame, h);
    printf("panel_rows=%u out_mismatch=%u hit_mismatch=%u\n", (unsigned)out_rows, (unsigned)out_mismatch,
           (unsigned)machine.ppu.hit_mismatch);
    return 0;
}
