/* Gamebuino AKA hardware adapter: the panel's wiring (ST7789-class on the
 * SPI), seven digital buttons and the speaker behind I2S (the ESP32-S3
 * has no DAC to play it on).  The pins are the working assumption until
 * the console is documented; they live here and in esp32_config.h. */
#include <Arduino.h>
#include <driver/i2s.h>
#include "esp32_hw.h"
#include "aka_hw.h"
#include "esp32_config.h"
extern "C" {
#include "m0_menu.h"
}

#define LCD_CS 9
#define LCD_DC 8
#define LCD_BL 10
#define I2S_BCLK 15
#define I2S_LRC 16
#define I2S_DOUT 7
#define BTN_UP 2
#define BTN_DOWN 3
#define BTN_LEFT 4
#define BTN_RIGHT 5
#define BTN_A 6
#define BTN_B 11
#define BTN_HOME 12
#define BTN_MENU 14

extern "C" void esp32_lcd_pins(void) {
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_DC, LOW);
    digitalWrite(LCD_BL, HIGH);
    pinMode(ESP32_SD_CS, OUTPUT);
    digitalWrite(ESP32_SD_CS, HIGH);
    for (uint8_t pin : {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_HOME, BTN_MENU})
        pinMode(pin, INPUT_PULLUP);
}
extern "C" void esp32_lcd_dc(int command) { digitalWrite(LCD_DC, command ? LOW : HIGH); }
extern "C" void esp32_lcd_cs(int selected) { digitalWrite(LCD_CS, selected ? LOW : HIGH); }

extern "C" uint8_t esp32_buttons(void) {
    uint8_t out = 0;
    if (!digitalRead(BTN_DOWN)) out |= M0_MB_DOWN;
    if (!digitalRead(BTN_LEFT)) out |= M0_MB_LEFT;
    if (!digitalRead(BTN_RIGHT)) out |= M0_MB_RIGHT;
    if (!digitalRead(BTN_UP)) out |= M0_MB_UP;
    if (!digitalRead(BTN_A)) out |= M0_MB_A;
    if (!digitalRead(BTN_B)) out |= M0_MB_B;
    if (!digitalRead(BTN_MENU)) out |= M0_MB_MENU;
    if (!digitalRead(BTN_HOME)) out |= M0_MB_HOME;
    return out;
}
extern "C" void esp32_restart(void) { ESP.restart(); for (;;) {} }

/* ---- audio: I2S ------------------------------------------------------------
 * The ring pulled a buffer at a time by the I2S DMA, kept fed by this task
 * (the samples are unsigned 8-bit; the wire wants them 16-bit little-endian
 * scaled to full range). */
extern "C" void esp32_audio_pull(uint8_t *dst, unsigned n);
static TaskHandle_t audio_task_handle;
static void audio_task(void *) {
    uint8_t source[256];
    int16_t wire[256];
    for (;;) {
        esp32_audio_pull(source, sizeof source);
        for (unsigned i = 0; i < sizeof source; i++)
            wire[i] = (int16_t)((source[i] - 128) << 8);
        size_t written = 0;
        i2s_write(I2S_NUM_0, wire, sizeof wire, &written, portMAX_DELAY);
    }
}
extern "C" void esp32_audio_start(unsigned hz) {
    i2s_config_t config = {};
    config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    config.sample_rate = hz;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count = 4;
    config.dma_buf_len = 256;
    config.use_apll = false;
    i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
    i2s_pin_config_t pins = {};
    pins.bck_io_num = I2S_BCLK;
    pins.ws_io_num = I2S_LRC;
    pins.data_out_num = I2S_DOUT;
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
    xTaskCreate(audio_task, "m0audio", 3072, nullptr, 5, &audio_task_handle);
}
extern "C" void esp32_audio_stop(void) { i2s_zero_dma_buffer(I2S_NUM_0); }
extern "C" void esp32_audio_sample(uint8_t) {} /* the task pulls, nothing pushes */

/* ---- entry: the Arduino task runs the emulation -------------------------- */
void setup(void) {
    setCpuFrequencyMhz(240);
    m0boy_console_main();
}
void loop(void) {}
