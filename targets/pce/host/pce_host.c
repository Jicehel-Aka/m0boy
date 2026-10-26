/* PCE host harness: a HuCard image in the machine, N frames out, the
 * display's window as a PPM, the sound as a WAV, and the run's
 * fingerprint (CPU, WRAM, VRAM, SATB, palette, frame — the same contract
 * shape as the other runtimes). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/pce.h"
#include "../../consoles/pc/pc_input.h"
#ifdef PCE_EMBEDDED
#include "../src/pce_config.h"
extern const uint8_t pce_rom[];
#endif

#define AUDIO_RATE 11025u

static uint16_t frame[262][512];
static void on_row(void *host, unsigned line, const uint16_t *pixels, unsigned width) {
    (void)host;
    if (line < 262 && width <= 512)
        memcpy(frame[line], pixels, width * sizeof frame[line][0]);
}

static uint32_t fnv(const void *data, size_t n, uint32_t h) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* the HuC6280 interpreter calls this every instruction when the build
 * defines PCE_TRACE (the generated host projects do); --coverage dumps it */
static uint32_t pc_hits[65536];
void pce_trace_step(uint16_t pc) { pc_hits[pc]++; }

int main(int argc, char **argv) {
    const char *rom_path = 0, *ppm = 0, *wav = 0, *input_path = 0, *coverage = 0;
    unsigned frames = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else rom_path = argv[i];
    }
    const uint8_t *rom;
    uint32_t rom_bytes;
    if (!rom_path) {
#ifdef PCE_EMBEDDED
        rom = pce_rom;
        rom_bytes = PCE_ROM_SIZE;
#else
        fprintf(stderr, "usage: pce_host rom.pce [--frames n] [--ppm f] [--wav f] [--input f] [--coverage f]\n");
        return 2;
#endif
    } else {
        FILE *file = fopen(rom_path, "rb");
        if (!file) { fprintf(stderr, "cannot read %s\n", rom_path); return 2; }
        fseek(file, 0, SEEK_END);
        const long size = ftell(file);
        fseek(file, size % 131072 == 128 ? 128 : 0, SEEK_SET); /* a 128-byte header, some collections carry one */
        rom_bytes = (uint32_t)((size - (size % 131072 == 128 ? 128 : 0) + 0x1FFFu) & ~0x1FFFu);
        uint8_t *bytes = calloc(1, rom_bytes);
        if (fread(bytes, 1, (size_t)(size - (size % 131072 == 128 ? 128 : 0)), file) == 0) {
            fprintf(stderr, "truncated image: %s\n", rom_path);
            return 2;
        }
        fclose(file);
        rom = bytes;
    }

    pce_init(rom, rom_bytes);
    machine.row = on_row;
    /* a momentary-input script: "<frame> buttons" lines held for that one
     * frame, buttons by whole name ("i+right" — strstr would read "right"
     * as "i"), '#' lines are comments */
    static const char *const pce_names[8] = {
        "i", "ii", "select", "run", "up", "right", "down", "left"};
    pc_input_t script = {0};
    if (input_path && !pc_input_load(&script, input_path, pce_names, 8)) return 2;
    FILE *audio = wav ? fopen(wav, "wb") : 0;
    if (audio) { /* the header is patched once the length is known */
        const char silence[44] = {0};
        fwrite(silence, 1, 44, audio);
    }
    uint32_t audio_bytes = 0;
    for (unsigned f = 0; f < frames; f++) {
        machine.input = (uint8_t)pc_input_keys(&script, f);
        pce_frame();
        if (audio) {
            uint8_t samples[512];
            /* 119210 CPU cycles per frame: the samples that covers */
            static unsigned carry;
            carry += AUDIO_RATE * 119210u;
            unsigned count = carry / 7159090u;
            carry %= 7159090u;
            if (count > sizeof samples / 2) count = sizeof samples / 2;
            psg_render_stereo(&machine.psg, samples, count, AUDIO_RATE);
            fwrite(samples, 2, count, audio);
            audio_bytes += count * 2;
        }
    }
    if (audio) {
        fseek(audio, 0, SEEK_SET);
        fprintf(audio, "RIFF");
        const uint32_t riff = 36 + audio_bytes;
        fwrite(&riff, 4, 1, audio);
        fprintf(audio, "WAVEfmt ");
        const uint32_t fmt = 16;
        fwrite(&fmt, 4, 1, audio);
        const uint16_t pcm = 1, ch = 2;
        fwrite(&pcm, 2, 1, audio);
        fwrite(&ch, 2, 1, audio);
        const uint32_t rate = AUDIO_RATE, byte_rate = AUDIO_RATE * 2;
        fwrite(&rate, 4, 1, audio);
        fwrite(&byte_rate, 4, 1, audio);
        const uint16_t block = 2, bits = 8;
        fwrite(&block, 2, 1, audio);
        fwrite(&bits, 2, 1, audio);
        fprintf(audio, "data");
        fwrite(&audio_bytes, 4, 1, audio);
        fclose(audio);
    }
    if (ppm && machine.have_frame) {
        FILE *out = fopen(ppm, "wb");
        if (out) {
            const unsigned width = machine.vdc.active_width, height = machine.vdc.active_height;
            fprintf(out, "P6\n%u %u\n255\n", width, height);
            for (unsigned y = 0; y < height; y++)
                for (unsigned x = 0; x < width; x++) {
                    const uint16_t c = frame[machine.first_active_line + y][x];
                    const uint8_t px[3] = {(uint8_t)(c >> 8 & 0xF8), (uint8_t)(c >> 3 & 0xFC), (uint8_t)(c << 3 & 0xF8)};
                    fwrite(px, 1, 3, out);
                }
            fclose(out);
        }
    }
    uint32_t h = 2166136261u;
    /* registers field by field: the struct carries the bus's fast-path
     * pointers, and hashing its bytes would hash addresses and padding */
    const uint8_t regs[12] = {
        machine.cpu.a, machine.cpu.x, machine.cpu.y, machine.cpu.s, machine.cpu.p,
        (uint8_t)machine.cpu.pc, (uint8_t)(machine.cpu.pc >> 8),
        (uint8_t)machine.cpu.cycles, (uint8_t)(machine.cpu.cycles >> 8),
        (uint8_t)(machine.cpu.cycles >> 16), (uint8_t)(machine.cpu.cycles >> 24),
        (uint8_t)(machine.frames & 0xFFu),
    };
    h = fnv(regs, sizeof regs, h);
    h = fnv(machine.wram, sizeof machine.wram, h);
    h = fnv(machine.vdc.vram, sizeof machine.vdc.vram, h);
    h = fnv(machine.vdc.sat, sizeof machine.vdc.sat, h);
    h = fnv(machine.vce_colors, sizeof machine.vce_colors, h);
    if (machine.have_frame) {
        for (unsigned y = 0; y < machine.vdc.active_height; y++)
            h = fnv(frame[machine.first_active_line + y],
                    machine.vdc.active_width * sizeof frame[0][0], h);
    }
    if (coverage) {
        FILE *out = fopen(coverage, "w");
        if (!out) { fprintf(stderr, "cannot write %s\n", coverage); return 2; }
        fprintf(out, "# m0boy PCE coverage: address, HuC6280 instructions started\n");
        for (unsigned pc = 0; pc < 65536; pc++)
            if (pc_hits[pc])
                fprintf(out, "%04x %u\n", pc, pc_hits[pc]);
        fclose(out);
    }
    printf("frames=%u cycles=%u pc=%04x run_hash=%08x\n",
           frames, (unsigned)machine.cpu.cycles, machine.cpu.pc, h);
    return 0;
}
