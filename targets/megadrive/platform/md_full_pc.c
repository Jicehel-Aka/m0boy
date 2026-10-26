#include "md_full.h"
#include "pc_frontend.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 224 };
typedef struct { unsigned frame, mask; } Event;
static int read_events(const char *path, Event **out, size_t *count) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    unsigned frame, previous = 0;
    int mask;
    while (fscanf(f, "%u %i", &frame, &mask) == 2) {
        if ((*count && frame <= previous) || mask < 0 || mask > 255) { fclose(f); return 0; }
        Event *p = realloc(*out, (*count + 1u) * sizeof **out);
        if (!p) { fclose(f); return 0; }
        *out = p; p[(*count)++] = (Event){frame, (unsigned)mask};
        previous = frame;
    }
    int okay = feof(f);
    fclose(f);
    return okay;
}
static int write_ppm(const char *path, const uint32_t *pixels) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        fputc((p >> 16) & 255u, f); fputc((p >> 8) & 255u, f); fputc(p & 255u, f);
    }
    return fclose(f) == 0;
}
static void wav_header(FILE *f, uint32_t samples) {
    const uint32_t bytes = samples * 2u;
    const uint8_t h[44] = {'R','I','F','F', (uint8_t)(bytes + 36u), (uint8_t)((bytes + 36u) >> 8),
        (uint8_t)((bytes + 36u) >> 16), (uint8_t)((bytes + 36u) >> 24), 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0, 0x44,0xac,0,0, 0x88,0x58,1,0,
        2,0, 16,0, 'd','a','t','a', (uint8_t)bytes, (uint8_t)(bytes >> 8),
        (uint8_t)(bytes >> 16), (uint8_t)(bytes >> 24)};
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
}
static uint32_t image_hash(const uint32_t *pixels) {
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < W * H; ++i) {
        uint32_t p = pixels[i];
        for (unsigned b = 0; b < 3; ++b) { h = (h ^ (p & 255u)) * 16777619u; p >>= 8; }
    }
    return h;
}
static int save_coverage(const char *path, const MdFull *m) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 0; }
    for (uint32_t pc = 0; pc < md_rom_size; pc += 2)
        if (m->coverage[pc >> 3] & (1u << (pc & 7u))) fprintf(f, "%06x\n", pc);
    return fclose(f) == 0;
}
int main(int argc, char **argv) {
    unsigned limit = 0;
    int interp = 0;
    const char *ppm = NULL, *script = NULL, *coverage = NULL, *wav = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) limit = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else if (!strcmp(argv[i], "--coverage") && i + 1 < argc) coverage = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else { fprintf(stderr, "usage: md_pc [--frames N] [--input SCRIPT] [--ppm FILE] [--wav FILE] [--coverage FILE] [--interp]\n"); return 2; }
    }
    Event *events = NULL;
    size_t event_count = 0, event_at = 0;
    if (script && !read_events(script, &events, &event_count)) {
        fprintf(stderr, "invalid input script\n"); free(events); return 2;
    }
    MdFull *m = calloc(1, sizeof *m);
    uint32_t *pixels = malloc(W * H * sizeof *pixels);
    if (!m || !pixels) { free(m); free(pixels); free(events); return 1; }
    md_full_reset(m);
    if (coverage) {
        m->coverage_bytes = (md_rom_size + 7u) / 8u;
        m->coverage = calloc(m->coverage_bytes, 1);
        if (!m->coverage) { free(m); free(pixels); free(events); return 1; }
    }
    PcFrontend pc = {0};
    SDL_AudioDeviceID audio_device = 0;
    FILE *wav_file = NULL;
    uint32_t wav_samples = 0;
    if (wav) {
        wav_file = fopen(wav, "wb+");
        if (!wav_file) { perror(wav); return 1; }
        wav_header(wav_file, 0);
    }
    if (!limit) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_AUDIO)) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
        }
        if (!pc_frontend_open(&pc, "m0boy Mega Drive", W, H)) return 1;
        SDL_AudioSpec wanted = {0};
        wanted.freq = 44100; wanted.format = AUDIO_S16SYS; wanted.channels = 1; wanted.samples = 1024;
        audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted, NULL, 0);
        if (audio_device) SDL_PauseAudioDevice(audio_device, 0);
        else fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
    }
    unsigned scripted = 0;
    int running = 1;
    uint64_t deadline = SDL_GetTicks64();
    unsigned frame_fraction = 0;
    while (running && !m->fault && (!limit || m->frames < limit)) {
        if (!limit) {
            pc_frontend_poll(&pc, 1);
            if (pc.quit) break;
        }
        while (event_at < event_count && events[event_at].frame == m->frames)
            scripted = events[event_at++].mask;
        m->pad = (uint8_t)(scripted | (limit ? 0u : pc_frontend_buttons(&pc)));
        md_full_frame(m, !interp);
        int16_t pcm[735];
        md_full_audio_render(m, pcm, 735);
        if (wav_file) { fwrite(pcm, sizeof *pcm, 735, wav_file); wav_samples += 735; }
        if (audio_device && SDL_GetQueuedAudioSize(audio_device) < 44100u * sizeof *pcm / 4u)
            SDL_QueueAudio(audio_device, pcm, sizeof pcm);
        if (!limit && !m->fault) {
            md_full_render(m, pixels, W);
            if (!pc_frontend_present(&pc, pixels, W, H, W)) { running = 0; break; }
            deadline += 16u;
            frame_fraction += 2u;
            if (frame_fraction >= 3u) { ++deadline; frame_fraction -= 3u; }
            uint64_t now = SDL_GetTicks64();
            if (deadline > now) SDL_Delay((uint32_t)(deadline - now));
            else if (now - deadline > 100u) { deadline = now; frame_fraction = 0; }
        }
    }
    md_full_render(m, pixels, W);
    int okay = (!ppm || write_ppm(ppm, pixels)) && (!coverage || save_coverage(coverage, m));
    if (wav_file) { wav_header(wav_file, wav_samples); if (fclose(wav_file)) okay = 0; }
    printf("frames=%u pc=%06x fault=%u fault_pc=%06x opcode=%04x native_hits=%u fallback_hits=%u run_hash=%08x image_hash=%08x audio_hash=%08x\n",
        m->frames, m->pc, m->fault, m->fault_pc, m->last_opcode,
        m->native_hits, m->fallback_hits, md_full_hash(m), image_hash(pixels), m->audio_hash);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    pc_frontend_close(&pc);
    if (!limit) SDL_Quit();
    const int failed = !okay || m->fault;
    free(m->coverage); free(m); free(pixels); free(events);
    return failed ? 1 : 0;
}
