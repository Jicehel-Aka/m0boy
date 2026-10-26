#include "psg.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    psg_t p;
    uint8_t samples[11025];
    psg_reset(&p);
    psg_write(&p, 0x00, 0);
    psg_write(&p, 0x02, 1000 & 255);
    psg_write(&p, 0x03, 1000 >> 8);
    psg_write(&p, 0x04, 0x9F);
    psg_write(&p, 0x07, 0x9F); /* noise is unavailable on channel 0 */
    psg_render(&p, samples, sizeof samples, 11025);
    /* One second advances 3579545/1000 waveform entries. This catches
     * overflow of the 3.58 MHz clock before the 12-bit fixed-point shift. */
    assert(p.ch[0].waveform_index == (3579 & 31));

    psg_reset(&p);
    psg_write(&p, 0x01, 0xF0); /* output to left only */
    psg_write(&p, 0x00, 0);
    psg_write(&p, 0x05, 0xFF);
    psg_write(&p, 0x04, 0xDF); /* enabled DDA, full volume */
    psg_write(&p, 0x06, 31);
    uint8_t pair[2];
    psg_render_stereo(&p, pair, 1, 11025);
    assert(pair[0] > 128 && pair[1] == 128);
    psg_write(&p, 0x01, 0x0F);
    psg_render_stereo(&p, pair, 1, 11025);
    assert(pair[0] == 128 && pair[1] > 128);
    psg_write(&p, 0x05, 0xF0);
    psg_render_stereo(&p, pair, 1, 11025);
    assert(pair[0] == 128 && pair[1] == 128);
    puts("ok: PCE PSG clock and balance");
    return 0;
}
