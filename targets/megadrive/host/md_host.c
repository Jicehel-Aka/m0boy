#include "md.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    unsigned frames = 60;
    int interp = 0;
    const char *ppm = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--interp")) interp = 1;
        else if (!strcmp(argv[i], "--ppm") && i + 1 < argc) ppm = argv[++i];
        else { fprintf(stderr, "usage: md_host [--frames N] [--interp] [--ppm FILE]\n"); return 2; }
    }
    Md *m = (Md *)malloc(sizeof *m);
    if (!m) return 1;
    md_reset(m);
    for (unsigned f = 0; f < frames && !m->fault; ++f) {
        if (interp) md_run_interp(m, 4096);
        else md_run_native(m, 4096);
        m->frames++;
    }
    if (ppm && !m->fault) {
        FILE *out = fopen(ppm, "wb");
        if (!out) { perror(ppm); free(m); return 1; }
        fprintf(out, "P6\n160 128\n255\n");
        uint16_t c = md_background_rgb565(m);
        unsigned char rgb[3] = {(unsigned char)((c >> 11) * 255u / 31u),
                                (unsigned char)(((c >> 5) & 63u) * 255u / 63u),
                                (unsigned char)((c & 31u) * 255u / 31u)};
        for (unsigned p = 0; p < 160 * 128; ++p) fwrite(rgb, 1, 3, out);
        fclose(out);
    }
    printf("frames=%u instructions=%u pc=%06x fault=%u bg=%04x run_hash=%08x\n",
           m->frames, m->instructions, m->pc, m->fault, md_background_rgb565(m), md_hash(m));
    int result = m->fault ? 1 : 0;
    free(m);
    return result;
}
