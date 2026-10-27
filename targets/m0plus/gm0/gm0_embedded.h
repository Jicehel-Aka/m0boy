#ifndef M0BOY_GM0_EMBEDDED_H
#define M0BOY_GM0_EMBEDDED_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { GM0_EMBEDDED_META = 0, GM0_EMBEDDED_POKITTO = 1 };
enum { GM0_EMBEDDED_FLASH_BYTES = 0x40000, GM0_EMBEDDED_PIXELS = 320 * 240 };
/* The caller places flash and pixels in PSRAM; the guest SRAM and peripheral
 * state remain in fast memory. Firmware is copied at its original link base. */
int gm0_embedded_init(const uint8_t *firmware, size_t bytes, int system,
                      uint8_t *flash_storage, uint16_t *pixel_storage);
void gm0_embedded_frame(void);
void gm0_embedded_buttons(uint8_t pressed);
void gm0_embedded_sd_image(uint8_t *image, size_t bytes);
const uint16_t *gm0_embedded_pixels(void);
unsigned gm0_embedded_width(void);
unsigned gm0_embedded_height(void);
uint32_t gm0_embedded_pc(void);
uint32_t gm0_embedded_cycles(void);

#ifdef __cplusplus
}
#endif
#endif
