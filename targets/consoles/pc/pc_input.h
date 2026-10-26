/* Input scripts and their recording for the PC windows (SDL2): each core
 * keeps the dialect its host harness reads with --input, so a session
 * recorded in the window replays there untouched.
 *
 * - momentary-named (NES, PC Engine): "<frame> <names>" lines, buttons by
 *   name joined with '+', held for their one frame only.  The recorder
 *   writes a line for every frame whose buttons are not zero — a held pad
 *   becomes one line per frame, the only faithful shape for a momentary
 *   script.
 * - colon (PICO-8): "at:mask:len" segments held from at for len frames;
 *   the recorder closes a segment at every button change.
 *
 * The readers play a script in the window (env M0_INPUT, --input on the
 * PCE shell), merged with the live pad; the recorders write env
 * M0_RECORD_INPUT (NES, PICO-8) or --record-input (PCE).  A restart or a
 * save-state load ends the recording: the harness replays from power-on,
 * so what follows could not replay. */
#ifndef PC_INPUT_H
#define PC_INPUT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- momentary-named: NES and PC Engine --------------------------------- */

typedef struct { unsigned frame, buttons, order; } pc_step_t;

typedef struct { unsigned count, pos; pc_step_t *steps; } pc_input_t;

static int pc_step_order(const void *a, const void *b) {
    const pc_step_t *x = (const pc_step_t *)a, *y = (const pc_step_t *)b;
    if (x->frame != y->frame) return x->frame < y->frame ? -1 : 1;
    return x->order < y->order ? -1 : 1; /* file order within a frame: last wins */
}

/* "<frame> <names>" lines, names joined with '+' or "none"; '#' comments. */
static int pc_input_load(pc_input_t *in, const char *path,
                         const char *const *names, unsigned n_names) {
    memset(in, 0, sizeof *in);
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 0; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char list[200];
        unsigned at;
        if (line[0] == '#' || sscanf(line, "%u %199[^\n]", &at, list) != 2) continue;
        unsigned buttons = 0;
        for (char *tok = strtok(list, "+ "); tok; tok = strtok(NULL, "+ ")) {
            unsigned i = 0;
            while (i < n_names && strcmp(tok, names[i])) i++;
            if (i == n_names && strcmp(tok, "none")) {
                fprintf(stderr, "%s: unknown button '%s'\n", path, tok);
                fclose(f);
                free(in->steps);
                memset(in, 0, sizeof *in);
                return 0;
            }
            if (i < n_names) buttons |= 1u << i;
        }
        pc_step_t *steps = (pc_step_t *)realloc(in->steps, (in->count + 1) * sizeof *steps);
        if (!steps) { fclose(f); free(in->steps); memset(in, 0, sizeof *in); return 0; }
        in->steps = steps;
        in->steps[in->count].frame = at;
        in->steps[in->count].buttons = buttons;
        in->steps[in->count].order = in->count;
        in->count++;
    }
    fclose(f);
    qsort(in->steps, in->count, sizeof *in->steps, pc_step_order);
    return 1;
}

/* the entries for this one frame: momentary scripts know no "held" */
static unsigned pc_input_keys(pc_input_t *in, unsigned frame) {
    unsigned keys = 0;
    while (in->pos < in->count && in->steps[in->pos].frame <= frame) {
        if (in->steps[in->pos].frame == frame) keys = in->steps[in->pos].buttons;
        in->pos++;
    }
    return keys;
}

static void pc_input_free(pc_input_t *in) { free(in->steps); memset(in, 0, sizeof *in); }

/* One line per frame whose buttons are not zero, so the file replays
 * without gaps; flushed as it goes, a killed window keeps its session. */
static void pc_record_buttons(FILE *file, unsigned frame, unsigned buttons,
                              const char *const *names, unsigned n_names) {
    if (!file || !buttons) return;
    fprintf(file, "%u ", frame);
    for (unsigned i = 0, first = 1; i < n_names; i++)
        if (buttons & (1u << i)) {
            fprintf(file, "%s%s", first ? "" : "+", names[i]);
            first = 0;
        }
    fputc('\n', file);
    if (frame % 60 == 0) fflush(file);
}

static int pc_record_close(FILE *file) { return file && fclose(file) != 0; }

/* ---- colon: PICO-8 ------------------------------------------------------- */

/* the segments one text holds, "at:mask:len" joined with ','; spaces and
 * newlines separate as a comma would, '#' lines are comments */
static unsigned pc_scripted(const char *script, unsigned frame) {
    unsigned keys = 0;
    for (const char *p = script; p && *p;) {
        unsigned at, mask, len;
        if (sscanf(p, "%u:%u:%u", &at, &mask, &len) == 3 && frame >= at && frame < at + len)
            keys |= mask;
        p = strchr(p, ',');
        if (p) p++;
    }
    return keys;
}

/* the file's segments as one --input string (malloc'd), or NULL */
static char *pc_script_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    char *text = NULL;
    size_t size = 0, cap = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        const size_t n = strlen(line);
        if (size + n + 1 > cap) {
            cap = (size + n + 1) * 2;
            char *next = (char *)realloc(text, cap);
            if (!next) { fclose(f); free(text); return NULL; }
            text = next;
        }
        memcpy(text + size, line, n);
        size += n;
    }
    fclose(f);
    if (!text) return NULL;
    text[size] = '\0';
    for (char *p = text; *p; p++) /* a newline separates like a comma */
        if (*p == '\n' || *p == '\r') *p = ',';
    return text;
}

typedef struct { FILE *file; unsigned start, mask, next; int open, first; } pc_segments_t;

static int pc_segments_open(pc_segments_t *s, const char *path) {
    memset(s, 0, sizeof *s);
    s->file = fopen(path, "w");
    if (!s->file) { perror(path); return 0; }
    s->first = 1;
    return 1;
}

/* every played frame reports here; a change closes the running segment */
static void pc_segments_note(pc_segments_t *s, unsigned frame, unsigned mask) {
    if (!s->file) return;
    if (frame % 60 == 0) fflush(s->file); /* a killed window keeps its session */
    if (s->open && mask == s->mask) {
        s->next = frame + 1;
        return;
    }
    if (s->open && s->mask) {
        fprintf(s->file, "%s%u:%u:%u", s->first ? "" : ",", s->start, s->mask, s->next - s->start);
        s->first = 0;
    }
    s->start = frame;
    s->mask = mask;
    s->next = frame + 1;
    s->open = 1;
}

/* end: one past the last frame played */
static int pc_segments_finish(pc_segments_t *s, unsigned end) {
    if (!s->file) return 0;
    if (s->open && s->mask)
        fprintf(s->file, "%s%u:%u:%u\n", s->first ? "" : ",", s->start, s->mask, end - s->start);
    else if (!s->first)
        fputc('\n', s->file);
    const int failed = fflush(s->file) != 0 || fclose(s->file) != 0;
    s->file = NULL;
    return failed;
}

#endif
