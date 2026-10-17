/* The menu only needs the watchdog hook; the host has no SAMD21 registers. */
#ifndef META_TEST_HW_H
#define META_TEST_HW_H
#ifdef __cplusplus
extern "C" {
#endif
void meta_watchdog_feed(void);
#ifdef __cplusplus
}
#endif
#endif
