/* Single-task ESP8266 entry: keep Arduino's peripheral drivers and image
 * tooling, without starting the WiFi/non-OS SDK. The SDK startup alone does
 * not leave room for a game in a 256 KiB application. eboot loads IRAM/DRAM
 * and jumps here on its system stack (0x3ffffff0); the heap ends at 0x3fffc000.
 * ROM API addresses and image ABI: Arduino ESP8266 3.1.2, eagle.rom.addr.v6.ld
 * and bootloaders/eboot/eboot.c. No OTA commands or SDK tasks are scheduled. */
#include <Arduino.h>

extern "C" {
void Cache_Read_Enable(uint8_t, uint8_t, uint8_t);
void ets_wdt_disable(void);
extern char _bss_start, _bss_end;
extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
}
int espboy_main(void);
static uint8_t cpu_mhz = 80;
static uint32_t last_cycles, clock_fraction;
static uint64_t time_us;

static uint64_t clock_us(void) {
    uint32_t cycles;
    __asm__ volatile("rsr %0, ccount" : "=a"(cycles));
    uint32_t delta = cycles - last_cycles;
    last_cycles = cycles;
    uint32_t whole = delta / cpu_mhz;
    clock_fraction += delta % cpu_mhz;
    time_us += whole + clock_fraction / cpu_mhz;
    clock_fraction %= cpu_mhz;
    return time_us;
}
extern "C" uint32_t __wrap_micros(void) { return (uint32_t)clock_us(); }
extern "C" uint32_t __wrap_millis(void) { return (uint32_t)(clock_us() / 1000); }
extern "C" uint32_t __wrap_system_get_time(void) { return __wrap_micros(); }
extern "C" uint8_t __wrap_system_get_cpu_freq(void) { return cpu_mhz; }
extern "C" bool __wrap_system_update_cpu_freq(uint8_t mhz) {
    clock_us();
    if (mhz != 80 && mhz != 160) return false;
    if (mhz == 160) CPU2X |= 1; else CPU2X &= ~1u;
    ets_update_cpu_frequency(mhz);
    cpu_mhz = mhz;
    clock_fraction = 0;
    __asm__ volatile("rsr %0, ccount" : "=a"(last_cycles));
    return true;
}
extern "C" void __wrap_yield(void) { clock_us(); }
extern "C" void __wrap_optimistic_yield(uint32_t) { clock_us(); }
extern "C" void __wrap_delay(unsigned long ms) {
    uint32_t start = __wrap_millis();
    while ((uint32_t)(__wrap_millis() - start) < ms) {}
}
extern "C" void IRAM_ATTR app_entry(void) {
    /* eboot has loaded .data/.rodata and IRAM, but has not cleared .bss or
     * mapped the application flash. Do not call flash code before this. */
    for (volatile uint32_t *p = (volatile uint32_t *)&_bss_start;
         p < (volatile uint32_t *)&_bss_end; ++p) *p = 0;
    /* Use the boot ROM exception/level-1 IRQ dispatcher and its handler
     * tables (ets_isr_attach), rather than the SDK's relocated vectors. */
    uint32_t rom_vectors = 0x40000000u;
    __asm__ volatile("wsr %0, vecbase; rsync" :: "a"(rom_vectors) : "memory");
    Cache_Read_Enable(0, 0, 1);
    ets_isr_mask(-1);
    ets_wdt_disable();
    CPU2X &= ~1u;
    ets_update_cpu_frequency(80);
    for (auto p = __init_array_end; p != __init_array_start;) (*--p)();
    timer1_isr_init();
    __asm__ volatile("rsil a2, 0" ::: "a2", "memory");
    espboy_main();
    for (;;) {}
}
/* No tone/PWM scheduler is active: GPIO writes must not pull Arduino's
 * waveform scheduler (and, through it, the whole non-OS SDK) into the image. */
extern "C" void IRAM_ATTR __wrap_digitalWrite(uint8_t pin, uint8_t value) {
    if (pin < 16) { if (value) GPOS = 1u << pin; else GPOC = 1u << pin; }
}
extern "C" void IRAM_ATTR __wrap___digitalWrite(uint8_t pin, uint8_t value) {
    __wrap_digitalWrite(pin, value);
}

extern "C" const char core_version[] __attribute__((section(".ver_number"))) = "m0boy-espboy";
extern "C" void IRAM_ATTR abort(void) {
    (*(volatile uint32_t *)0x60000700u) |= 0x80000000u;
    for (;;) {}
}
extern "C" void IRAM_ATTR __wrap_NMI_Handler(void) { abort(); }
extern "C" bool can_yield(void) { return false; }
extern "C" void __panic_func(const char *, int, const char *) { abort(); }
