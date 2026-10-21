/* Host harness for a recompiled PICO-8 cartridge: runs frames with scripted
 * buttons, prints a hash of the screen and optionally writes a picture. */
#include "p8main.h"
#include "p8_audio.h"
#include "m0_host_util.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jmp_buf failure;
static char failure_text[160];
void p8_error(const char *msg, const char *what) {
    snprintf(failure_text, sizeof failure_text, "%s%s%s", msg, *what ? " " : "", what);
    longjmp(failure, 1);
}

void p8_heap_stats(size_t out[8][2]);
void p8_node_usage(size_t *used, size_t *total);
static uint32_t screen_hash(void) { return m0_hash_bytes(M0_RUN_HASH_INIT, p8_screen, sizeof p8_screen); }

static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n128 128\n255\n");
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            const uint8_t b = p8_screen[y * 64 + (x >> 1)];
            const uint32_t rgb = p8_palette_rgb[p8g.pal_screen[(x & 1) ? b >> 4 : b & 15] & 15];
            const uint8_t p[] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb };
            fwrite(p, 1, 3, f);
        }
    fclose(f);
}

int main(int argc, char **argv) {
    unsigned frames = 60, held = 0;
    const char *shot = 0, *script = 0, *every = 0, *wav_path = 0;
    FILE *wav = 0;
    uint32_t wav_samples = 0, wav_frac = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) every = argv[++i]; /* prefix: written every 30 frames */
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav_path = argv[++i];
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc) held = (unsigned)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else { fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]); return 1; }
    }
    if (setjmp(failure)) {
        fprintf(stderr, "error at frame %u: %s\n", p8_frames, failure_text);
        if (shot) write_ppm(shot);
        printf("frames=%u error screen_hash=%08x\n", p8_frames, screen_hash());
        return 2;
    }
    size_t peak_live = 0, peak_stats[8][2];
    unsigned peak_frame = 0;
    if (wav_path) {
        wav = fopen(wav_path, "wb");
        if (!wav) { perror(wav_path); return 1; }
        m0_wav_header(wav, 0);
    }
    p8_start();
    for (unsigned f = 0; f < frames; f++) {
        p8_frame((uint16_t)(held | m0_scripted(script, f)));
        if (wav) { /* the audio of this frame, 22050 Hz */
            uint8_t buffer[1024];
            wav_frac += 22050;
            const unsigned n = wav_frac / p8_fps;
            wav_frac %= p8_fps;
            p8_audio_render(buffer, n);
            fwrite(buffer, 1, n, wav);
            wav_samples += n;
        }
        if (f % (getenv("P8_GC_EVERY") ? 1u : 10u) == (getenv("P8_GC_EVERY") ? 0u : 9u)) { /* live data after a full collection, the most the cartridge has needed so far */
            p8_gc();
            const size_t live = p8_heap_total_bytes() - p8_heap_free_bytes();
            if (live > peak_live) { peak_live = live; peak_frame = f; p8_heap_stats(peak_stats); }
        }
        if (getenv("P8_BINS") && f % 30 == 29) {
            char name[256];
            snprintf(name, sizeof name, "%s%04u.bin", getenv("P8_BINS"), f + 1);
            FILE *bf = fopen(name, "wb");
            if (bf) { fwrite(p8_screen, 1, sizeof p8_screen, bf); fclose(bf); }
        }
        if (every && f % 30 == 29) {
            char name[256];
            snprintf(name, sizeof name, "%s%04u.ppm", every, f + 1);
            write_ppm(name);
        }
    }
    if (getenv("P8_STATS")) {
        size_t (*st)[2] = peak_stats; /* at the peak of the live data */
        fprintf(stderr, "peak at frame %u\n", peak_frame);
        { size_t u, t; p8_node_usage(&u, &t); fprintf(stderr, "node slots used/allocated (now): %zu/%zu\n", u, t); }
#ifdef P8_STACK_STATS
        fprintf(stderr, "stack high-water: %ld values\n", (long)(p8_stack_high - p8_stack));
#endif
        const char *kinds[] = { "free+gap", "str", "tab", "clo", "upv", "vals", "nodes", "chars" };
        for (int k = 0; k < 8; k++) fprintf(stderr, "%-8s blocks %4zu bytes %6zu\n", kinds[k], st[k][0], st[k][1]);
    }
    if (wav && m0_wav_finish(wav, wav_samples)) perror("wav");
    if (shot) write_ppm(shot);
    printf("frames=%u gc=%u peak_live=%zu screen_hash=%08x\n", p8_frames, p8_gc_count, peak_live, screen_hash());
    return 0;
}
