/* ST7735 output for the Gamebuino META (160x128, RGB565 big-endian). */
#ifndef META_LCD_H
#define META_LCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void m0_lcd_init(void);
/* Send `rows` full-width rows starting at display row row0.  With
 * META_LCD_DMA the transfer is queued and started by the DMA interrupt as
 * soon as the bus is free; the call only waits when the queue is full.
 * Returns a sequence number: the buffer may be reused once
 * m0_lcd_done_seq() reaches it.  With META_LCD_12BPP the rows are packed
 * in place (RGB444): the buffer no longer holds RGB565 afterwards. */
uint32_t m0_lcd_send_rows(const void *buf, uint8_t row0, uint8_t rows);
uint32_t m0_lcd_done_seq(void);
int m0_lcd_busy(void);
void m0_lcd_wait(void);
/* Release the SPI bus (for the buttons) if no transfer is pending. */
int m0_lcd_release(void);
void m0_lcd_end_frame(void);
void m0_lcd_fill(uint16_t color_be);

#ifdef __cplusplus
}
#endif

#endif
