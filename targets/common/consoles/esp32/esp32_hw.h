/* The ESP32 family's shared drivers (esp32_hw.cpp), after espboy_hw.h's
 * model: each console's own file implements the pins and the audio path
 * declared here and provides esp32_config.h (panel, SD bus, audio kind). */
#ifndef M0_ESP32_HW_H
#define M0_ESP32_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Panel wiring: data/command and chip select (the shared SPI runs the
 * bytes).  m0_lcd_init calls the pin setup first. */
void esp32_lcd_pins(void);
void esp32_lcd_dc(int command);
void esp32_lcd_cs(int selected);

/* Audio: one unsigned sample at a time, called at the rate given to
 * esp32_audio_start; stop silences the path. */
void esp32_audio_start(unsigned hz);
void esp32_audio_stop(void);
void esp32_audio_sample(uint8_t pcm);

/* The emulation's entry (gb_main.cpp / sg_main.cpp define it), called from
 * the Arduino task by the console's setup(). */
int m0boy_console_main(void);

/* The panel's SPI (created by m0_lcd_init): the SD hooks ride the same
 * bus (void *: SPIClass is C++). */
void *esp32_lcd_bus(void);

/* One step of the audio path's drain (the ring in esp32_hw.cpp), called by
 * the console's interrupt or I2S callback. */
void esp32_audio_drain(void);

#ifdef __cplusplus
}
#endif

#endif
