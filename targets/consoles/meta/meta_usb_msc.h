#ifndef META_USB_MSC_H
#define META_USB_MSC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Exclusive MSC mode. The caller has stopped the game, flushed saves and
 * display DMA, and lends a 512-byte scratch buffer until reset. */
void meta_usb_msc_run(uint8_t *sector_buffer) __attribute__((noreturn));
/* Abandon the game's C frames (possibly on the ROM cache's menu stack) and
 * start the never-returning service on the full main stack. */
void meta_usb_msc_enter(uint8_t *sector_buffer) __attribute__((noreturn));
void meta_usb_msc_handler(void);
extern volatile uint8_t meta_usb_msc_active;
#ifdef __cplusplus
}
#endif
#endif
