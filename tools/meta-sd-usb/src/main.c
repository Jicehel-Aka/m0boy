#include "sd.h"
#include "usb_msc.h"
#include "meta_hw.h"

int main(void) {
    (void)sd_init(); /* No medium: enumerate, report NOT READY to the host. */
    usb_msc_init();
    for (;;) {
        usb_msc_poll();
        meta_watchdog_feed();
    }
}
