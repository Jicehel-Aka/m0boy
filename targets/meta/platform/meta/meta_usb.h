/* USB CDC-ACM device of the Gamebuino META (meta_usb.c), with META_USB. */
#ifndef META_USB_H
#define META_USB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void meta_usb_init(void);
void meta_usb_stop(void); /* detach, before the loader takes over */
void meta_usb_write(const void *data, unsigned n);
/* Set by the 1200-baud touch: the frontend saves, then calls
 * meta_reset_to_bootloader (meta_startup.c does it after a second if the
 * frontend does not get to it). */
extern volatile uint8_t meta_usb_bootloader_request;
/* Debugging from the host (tools/meta-usb.py): pad bits it presses, ORed
 * into meta_pad_read; the PCs SysTick interrupted: [0] counts them, sample
 * n is at [1 + n % META_USB_PROF_SAMPLES]. */
#define META_USB_PROF_SAMPLES 32
extern volatile uint8_t meta_usb_buttons;
extern volatile uint32_t meta_usb_prof[1 + META_USB_PROF_SAMPLES];
void meta_reset_to_bootloader(void);

#ifdef __cplusplus
}
#endif

#endif
