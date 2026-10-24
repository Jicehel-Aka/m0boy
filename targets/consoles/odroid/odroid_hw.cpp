/* ODROID-GO hardware adapter: the panel's wiring (ILI9341 on the VSPI the
 * microSD shares), the buttons (two analog axes for the d-pad) and the
 * speaker on the ESP32's internal DAC, fed by hardware timer 0 at the audio
 * rate.  Everything else is the shared ESP32 core (esp32_hw.cpp/h). */
#include <Arduino.h>
#include "driver/timer.h"
#include "esp32_hw.h"
#include "odroid_hw.h"
extern "C" {
#include "m0_menu.h"
}

/* ODROID-GO wiring (the wiki's GPIO map): ILI9341 CS 5, DC 21, backlight
 * 14; SD CS 22 on the same bus (esp32_config.h); buttons active low: A 32,
 * B 33, Start 39, Select 27, Menu 13; d-pad axes IO34 (x) and IO35 (y). */
#define LCD_CS 5
#define LCD_DC 21
#define LCD_BL 14
#define BTN_A 32
#define BTN_B 33
#define BTN_START 39
#define BTN_SELECT 27
#define BTN_MENU 13
#define DPAD_X 34
#define DPAD_Y 35

extern "C" void esp32_lcd_pins(void) {
    pinMode(LCD_CS, OUTPUT);
    pinMode(LCD_DC, OUTPUT);
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_CS, HIGH);
    digitalWrite(LCD_DC, LOW);
    digitalWrite(LCD_BL, HIGH);
    pinMode(22 /* SD CS, the bus the panel shares */, OUTPUT);
    digitalWrite(22, HIGH);
    pinMode(BTN_A, INPUT_PULLUP);
    pinMode(BTN_B, INPUT_PULLUP);
    pinMode(BTN_START, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_MENU, INPUT_PULLUP);
}

extern "C" void esp32_lcd_dc(int command) { digitalWrite(LCD_DC, command ? LOW : HIGH); }
extern "C" void esp32_lcd_cs(int selected) { digitalWrite(LCD_CS, selected ? LOW : HIGH); }

/* The d-pad: a voltage divider per axis — idle ~0 V, up/left ~3.3 V,
 * down/right ~1.65 V.  ADC1 channels (IO34/35), no pull-ups. */
static int axis(int pin) {
    uint32_t sum = 0;
    for (unsigned i = 0; i < 4; i++) sum += analogRead(pin);
    return sum / 4;
}
extern "C" uint8_t esp32_buttons(void) {
    uint8_t out = 0;
    if (!digitalRead(BTN_A)) out |= M0_MB_A;
    if (!digitalRead(BTN_B)) out |= M0_MB_B;
    if (!digitalRead(BTN_START)) out |= M0_MB_MENU;
    if (!digitalRead(BTN_SELECT) || !digitalRead(BTN_MENU)) out |= M0_MB_HOME;
    const int y = axis(DPAD_Y), x = axis(DPAD_X);
    if (y > 2800) out |= M0_MB_UP;
    else if (y > 1400) out |= M0_MB_DOWN;
    if (x > 2800) out |= M0_MB_LEFT;
    else if (x > 1400) out |= M0_MB_RIGHT;
    return out;
}
extern "C" void esp32_restart(void) { ESP.restart(); for (;;) {} }

/* ---- audio: the internal 8-bit DAC (GPIO25) at 11025 Hz ------------------ */
static hw_timer_t *audio_timer;
static void IRAM_ATTR audio_isr(void) { esp32_audio_drain(); }
extern "C" void esp32_audio_start(unsigned hz) {
    dacWrite(25, 128);
    audio_timer = timerBegin(0, 80, true); /* 80 MHz / 80 = 1 MHz */
    timerAttachInterrupt(audio_timer, &audio_isr, true);
    timerAlarmWrite(audio_timer, 1000000u / hz, true);
    timerAlarmEnable(audio_timer);
}
extern "C" void esp32_audio_stop(void) {
    if (audio_timer) timerAlarmDisable(audio_timer);
    dacWrite(25, 128);
}
extern "C" void esp32_audio_sample(uint8_t pcm) { dacWrite(25, pcm); }

/* ---- entry: the Arduino task runs the emulation -------------------------- */
void setup(void) {
    setCpuFrequencyMhz(240);
    m0boy_console_main();
}
void loop(void) {}
