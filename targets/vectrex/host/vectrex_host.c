#include "vectrex.h"
#include "pc_frontend.h"
#define VX_W VX_GRID
#define VX_H VX_GRID
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t pixels[VX_W * VX_H];
static uint8_t bios_image[8192];
/* every stroke the core draws, hashed: the run's fingerprint */
static uint32_t run_hash = 2166136261u;
static void hash32(uint32_t x) {
  for (int i = 0; i < 4; i++) {
    run_hash ^= (x >> (i * 8)) & 255;
    run_hash *= 16777619u;
  }
}
static void plot(int x, int y, unsigned z) {
  uint8_t *p = &pixels[(unsigned)y * VX_W + (unsigned)x];
  if (*p < z)
    *p = (uint8_t)z;
}
void vx_draw(Vectrex *v, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
             unsigned z, unsigned cycles) {
  (void)v;
  hash32((uint32_t)x0);
  hash32((uint32_t)y0);
  hash32((uint32_t)x1);
  hash32((uint32_t)y1);
  hash32(z << 16 | cycles);
  x0 >>= 8;
  y0 >>= 8;
  x1 >>= 8;
  y1 >>= 8;
  if (!vx_clip(&x0, &y0, &x1, &y1, VX_W, VX_H))
    return;
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0),
      sy = y0 < y1 ? 1 : -1, err = dx + dy;
  /* the beam's dwell: a dot lit for long is brighter than a stroke */
  unsigned bright = z * 2;
  if (!dx && !dy)
    bright = z * (2 + cycles / 4);
  if (bright > 255)
    bright = 255;
  for (;;) {
    plot(x0, y0, bright);
    if (x0 == x1 && y0 == y1)
      break;
    int e = err * 2;
    if (e >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}
static int load_bios(Vectrex *v, const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return 0;
  }
  uint8_t data[8192];
  size_t n = fread(data, 1, 8192, f);
  int extra = fgetc(f);
  fclose(f);
  if ((n != 4096 && n != 8192) || extra != EOF) {
    fprintf(stderr, "Vectrex BIOS must be exactly 4096 or 8192 bytes\n");
    return 0;
  }
  memset(bios_image, 0xff, 8192);
  memcpy(bios_image + 8192 - n, data, n);
  v->bios = bios_image;
  return 1;
}
static int save_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  fprintf(f, "P6\n%d %d\n255\n", VX_W, VX_H);
  for (unsigned i = 0; i < VX_W * VX_H; i++) {
    uint8_t z = pixels[i];
    uint8_t rgb[] = {(uint8_t)(z / 6), z, (uint8_t)(z / 5)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  return 1;
}
static void le16(FILE *f, unsigned n) {
  fputc(n & 255, f);
  fputc((n >> 8) & 255, f);
}
static void le32(FILE *f, unsigned n) {
  le16(f, n);
  le16(f, n >> 16);
}
static void wav_header(FILE *f, unsigned samples) {
  rewind(f);
  fwrite("RIFF", 1, 4, f);
  le32(f, 36 + samples * 2);
  fwrite("WAVEfmt ", 1, 8, f);
  le32(f, 16);
  le16(f, 1);
  le16(f, 1);
  le32(f, 44100);
  le32(f, 88200);
  le16(f, 2);
  le16(f, 16);
  fwrite("data", 1, 4, f);
  le32(f, samples * 2);
}
static uint32_t ram_hash(const Vectrex *v) {
  uint32_t h = 2166136261u;
  for (unsigned i = 0; i < sizeof v->ram; i++)
    h = (h ^ v->ram[i]) * 16777619u;
  return h;
}
static void input(Vectrex *v, uint16_t k) {
  v->joy_x = k & PC_LEFT ? 0 : k & PC_RIGHT ? 255 : 128;
  v->joy_y = k & PC_UP ? 255 : k & PC_DOWN ? 0 : 128;
  v->buttons = (k & PC_A ? 1 : 0) | (k & PC_B ? 2 : 0) |
               (k & PC_C ? 4 : 0) | (k & PC_D ? 8 : 0);
  v->via.ay[14] = ~v->buttons;
}
#ifdef VX_NATIVE
#include "vx_gen.h"
#ifdef VX_GEN_STATS
extern uint32_t vx_gen_calls, vx_gen_steps;
#endif
#endif
#ifdef VX_COVERAGE
extern uint32_t vx_coverage_hits[65536];
static int save_coverage(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return 0;
  fprintf(f, "# m0boy.vectrex.coverage v1: address, instructions started\n");
  for (unsigned pc = 0; pc < 65536; pc++)
    if (vx_coverage_hits[pc])
      fprintf(f, "%04x %u\n", pc, vx_coverage_hits[pc]);
  return fclose(f) == 0;
}
#endif
int main(int argc, char **argv) {
  const char *bios = 0, *ppm = 0, *wav = 0, *coverage = 0;
  int frames = 0, full_boot = 0, interp = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bios") && i + 1 < argc)
      bios = argv[++i];
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
      frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--ppm") && i + 1 < argc)
      ppm = argv[++i];
    else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
      wav = argv[++i];
    else if (!strcmp(argv[i], "--full-boot"))
      full_boot = 1;
    else if (!strcmp(argv[i], "--interp"))
      interp = 1;
    else if (!strcmp(argv[i], "--coverage") && i + 1 < argc)
      coverage = argv[++i];
    else {
      fprintf(stderr,
              "usage: %s --bios BIOS.vec [--frames N] [--ppm shot.ppm] [--wav "
              "sound.wav] "
              "[--full-boot] [--interp] [--coverage profile.cov]\n",
              argv[0]);
      return 2;
    }
  }
  if (!bios) {
    fprintf(stderr, "--bios is required\n");
    return 2;
  }
  Vectrex *v = calloc(1, sizeof *v);
  if (!v)
    return 1;
  if (!load_bios(v, bios)) {
    free(v);
    return 1;
  }
#ifdef VX_NATIVE
  /* the recompiled code folds the system ROM it was built with */
  if (interp)
    vx_native = 0;
  else if (vx_gen_bios_sum && vx_gen_bios_sum != vx_bios_sum(v->bios)) {
    fprintf(stderr, "this BIOS is not the one the code was recompiled with: "
                    "interpreting\n");
    vx_native = 0;
  }
#else
  (void)interp;
#endif
#ifndef VX_COVERAGE
  if (coverage) {
    fprintf(stderr, "--coverage: this build does not record it\n");
    return 2;
  }
#endif
  vx_reset(v);
  if (full_boot)
    v->fast_boot = 0;
  int headless = frames > 0;
  if (SDL_Init(headless ? SDL_INIT_TIMER
                        : SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }
  PcFrontend pc = {0};
  SDL_AudioDeviceID audio_device = 0;
  FILE *wav_file = 0;
  unsigned wav_samples = 0;
  uint64_t audio_fraction = 0;
  if (wav) {
    wav_file = fopen(wav, "wb+");
    if (!wav_file) {
      perror(wav);
      return 1;
    }
    wav_header(wav_file, 0);
  }
  if (!headless) {
    if (!pc_frontend_open(&pc, "m0boy Vectrex", VX_W, VX_H)) return 1;
    SDL_AudioSpec requested = {0}, obtained = {0};
    requested.freq = 44100;
    requested.format = AUDIO_S16SYS;
    requested.channels = 1;
    requested.samples = 1024;
    audio_device = SDL_OpenAudioDevice(0, 0, &requested, &obtained, 0);
    if (audio_device)
      SDL_PauseAudioDevice(audio_device, 0);
  }
  int frame = 0;
  while (frames <= 0 || frame < frames) {
    Uint32 begin = SDL_GetTicks();
    if (!headless) {
      pc_frontend_poll(&pc, 1);
      if (pc.quit) break;
      input(v, pc_frontend_buttons(&pc));
    }
    for (unsigned i = 0; i < VX_W * VX_H; i++)
      pixels[i] = (uint8_t)(pixels[i] * 3u / 4u);
    vx_frame(v);
    frame++;
    audio_fraction += 44100u * VX_FRAME_CYCLES;
    unsigned sample_count = (unsigned)(audio_fraction / 1500000u);
    audio_fraction %= 1500000u;
    int16_t audio_samples[1024];
    if (sample_count > 1024)
      sample_count = 1024;
    ay_render(v, audio_samples, sample_count, 44100);
    if (audio_device) {
      if (SDL_GetQueuedAudioSize(audio_device) > 44100u * 2u)
        SDL_ClearQueuedAudio(audio_device);
      SDL_QueueAudio(audio_device, audio_samples,
                     sample_count * sizeof(int16_t));
    }
    if (wav_file) {
      fwrite(audio_samples, sizeof(int16_t), sample_count, wav_file);
      wav_samples += sample_count;
    }
    if (!headless) {
      static uint32_t rgb[VX_W * VX_H];
      for (unsigned i = 0; i < VX_W * VX_H; ++i) {
        uint8_t z = pixels[i];
        rgb[i] = 0xff000000u | ((uint32_t)(z / 6) << 16) | ((uint32_t)z << 8) | (z / 5);
      }
      pc_frontend_present(&pc, rgb, VX_W, VX_H, VX_W);
      Uint32 elapsed = SDL_GetTicks() - begin;
      if (elapsed < 20)
        SDL_Delay(20 - elapsed);
    }
  }
  if (ppm && !save_ppm(ppm)) {
    perror(ppm);
    return 1;
  }
  if (wav_file) {
    wav_header(wav_file, wav_samples);
    fclose(wav_file);
  }
#ifdef VX_COVERAGE
  if (coverage && !save_coverage(coverage)) {
    perror(coverage);
    return 1;
  }
#endif
#if defined(VX_NATIVE) && defined(VX_GEN_STATS)
  if (vx_native)
    printf("native calls=%u interpreted=%u\n", vx_gen_calls, vx_gen_steps);
#endif
  printf("frames=%d pc=%04x cycles=%llu illegal=%u segments=%u "
         "a=%02x b=%02x x=%04x y=%04x u=%04x s=%04x dp=%02x cc=%02x "
         "via=%02x,%02x t1=%04x t2=%04x ifr=%02x c825=%02x%02x ram=%08x\n"
         "run_hash=%08x\n",
         frame, v->cpu.pc, (unsigned long long)v->cpu.cycles, v->cpu.illegal,
         v->via.segments, v->cpu.a, v->cpu.b,
         v->cpu.x, v->cpu.y, v->cpu.u, v->cpu.s, v->cpu.dp, v->cpu.cc,
         v->via.ora, v->via.orb, v->via.t1, v->via.t2, v->via.ifr, v->ram[0x25],
         v->ram[0x26], ram_hash(v), run_hash);
  pc_frontend_close(&pc);
  if (audio_device)
    SDL_CloseAudioDevice(audio_device);
  SDL_Quit();
  free(v);
  return 0;
}
