#include "md.h"
#include "m0_target.h"
#include "m0_clock.h"
#include "m0_lcd.h"
#include "meta_hw.h"

static Md machine;
static const char version[] __attribute__((used)) = "m0boy-megadrive-meta " GBL_GEN_VERSION;
static volatile uint32_t md_frames, md_fault;

int main() {
    meta_spi_init();
    m0_clock_init();
    m0_lcd_init();
    md_reset(&machine);
    uint32_t deadline = m0_micros();
    uint16_t previous = 0xffff;
    __asm__ volatile("" : : "r"(version));
    for (;;) {
        md_run_native(&machine, 4096);
        md_fault = machine.fault;
        uint16_t c = md_background_rgb565(&machine);
        if (c != previous) {
            /* META display accepts big-endian RGB565 halfwords. */
            m0_lcd_fill((uint16_t)(c << 8 | c >> 8));
            previous = c;
        }
        m0_lcd_end_frame();
        ++md_frames;
        deadline += 16667u;
        while ((int32_t)(m0_micros() - deadline) < 0) m0_watchdog_feed();
        if ((int32_t)(m0_micros() - deadline) > 100000) deadline = m0_micros();
        if (machine.fault) {
            /* Solid red identifies an unsupported operation or bus access. */
            m0_lcd_fill(0x00f8);
            for (;;) m0_watchdog_feed();
        }
    }
}
