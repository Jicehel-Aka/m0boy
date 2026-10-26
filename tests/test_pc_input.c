/* The PC windows' input scripts and their recording (pc_input.h): the
 * recorded files' exact bytes, and their readers, per dialect — the
 * momentary-named one the NES and PC Engine harnesses read, the colon one
 * of the PICO-8 harness.  A session recorded here must read back the same
 * frames, buttons first and last. */
#include "pc_input.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s failed at line %d\n", #expr, __LINE__); return 1; } } while (0)

static char text[8192];
static const char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

/* the names both momentary dialects share, bit i = names[i] */
static const char *const pad_names[8] = {"a", "b", "select", "start", "up", "down", "left", "right"};

int main(void) {
    /* ---- momentary recording: one line per frame with buttons ---- */
    FILE *f = fopen("rec_momentary.input", "w");
    CHECK(f);
    fprintf(f, "# NES input recording: <frame> <buttons>\n");
    const unsigned frames[] = {0, 3, 4, 60, 61, 90};
    const unsigned pressed[] = {0x11, 0x11, 0x08, 0x08, 0x82, 0x00};
    for (unsigned i = 0; i < sizeof frames / sizeof *frames; i++)
        pc_record_buttons(f, frames[i], pressed[i], pad_names, 8);
    fclose(f);
    CHECK(!strcmp(slurp("rec_momentary.input"),
                  "# NES input recording: <frame> <buttons>\n"
                  "0 a+up\n"
                  "3 a+up\n"
                  "4 start\n"
                  "60 start\n"
                  "61 b+right\n"));

    /* ---- the reader: comments, "none", unsorted lines, momentary lookup ---- */
    pc_input_t in;
    f = fopen("rec_unsorted.input", "w");
    CHECK(f);
    fputs("# a comment\n7 none\n2 right\n1 up\n61 b+right\n0 a+up\n", f);
    fclose(f);
    CHECK(pc_input_load(&in, "rec_unsorted.input", pad_names, 8));
    CHECK(in.count == 5);
    CHECK(pc_input_keys(&in, 0) == 0x11);
    CHECK(pc_input_keys(&in, 1) == 0x10);   /* the one-frame press of frame 1 */
    CHECK(pc_input_keys(&in, 2) == 0x80);   /* unsorted file, frame order kept */
    CHECK(pc_input_keys(&in, 3) == 0);
    CHECK(pc_input_keys(&in, 5) == 0);      /* momentary: nothing held across frames */
    CHECK(pc_input_keys(&in, 7) == 0);      /* "none" is zero buttons */
    CHECK(pc_input_keys(&in, 61) == 0x82);
    CHECK(pc_input_keys(&in, 62) == 0);
    pc_input_free(&in);

    /* ---- the recorded file reads back as it was written ---- */
    CHECK(pc_input_load(&in, "rec_momentary.input", pad_names, 8));
    for (unsigned fr = 0; fr <= 95; fr++) {
        unsigned recorded = 0;
        for (unsigned i = 0; i < sizeof frames / sizeof *frames; i++)
            if (frames[i] == fr) recorded = pressed[i];
        CHECK(pc_input_keys(&in, fr) == recorded);
    }
    pc_input_free(&in);

    /* ---- an unknown button refuses the file ---- */
    f = fopen("rec_bad.input", "w");
    CHECK(f);
    fputs("3 right+wheel\n", f);
    fclose(f);
    CHECK(pc_input_load(&in, "rec_bad.input", pad_names, 8) == 0);

    /* ---- colon recording: a segment closes at every change ---- */
    pc_segments_t segs;
    CHECK(pc_segments_open(&segs, "rec_segments.input"));
    for (unsigned fr = 0; fr < 10; fr++) pc_segments_note(&segs, fr, 0);
    for (unsigned fr = 10; fr < 15; fr++) pc_segments_note(&segs, fr, 0x10); /* 10:16:5 */
    for (unsigned fr = 15; fr < 30; fr++) pc_segments_note(&segs, fr, 0x11); /* one segment through the 60-frame flush */
    for (unsigned fr = 30; fr < 34; fr++) pc_segments_note(&segs, fr, 0);    /* release: no segment */
    CHECK(pc_segments_finish(&segs, 34) == 0);
    CHECK(!strcmp(slurp("rec_segments.input"), "10:16:5,15:17:15\n"));

    /* ---- the file plays back through the reader's own evaluator ---- */
    char *script = pc_script_file("rec_segments.input");
    CHECK(script);
    for (unsigned fr = 0; fr < 34; fr++) {
        unsigned want = 0;
        if (fr >= 10 && fr < 15) want = 0x10;
        if (fr >= 15 && fr < 30) want = 0x11;
        CHECK(pc_scripted(script, fr) == want);
    }
    free(script);

    /* ---- a colon file: comments, newlines as separators ---- */
    f = fopen("script_segments.input", "w");
    CHECK(f);
    fputs("# a session\n2:1:3\n9:32:2,20:4:1\n", f);
    fclose(f);
    script = pc_script_file("script_segments.input");
    CHECK(script);
    CHECK(pc_scripted(script, 0) == 0);
    CHECK(pc_scripted(script, 3) == 1);
    CHECK(pc_scripted(script, 4) == 1);     /* at 2 for 3 frames: 2, 3, 4 */
    CHECK(pc_scripted(script, 5) == 0);
    CHECK(pc_scripted(script, 9) == 32 && pc_scripted(script, 10) == 32);
    CHECK(pc_scripted(script, 11) == 0);
    CHECK(pc_scripted(script, 20) == 4);
    free(script);

    remove("rec_momentary.input");
    remove("rec_unsorted.input");
    remove("rec_bad.input");
    remove("rec_segments.input");
    remove("script_segments.input");
    return 0;
}
