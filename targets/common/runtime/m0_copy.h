/* Fixed-size copies (m0_copy.inc on the Cortex-M0+, memcpy elsewhere): the
 * card cache's blocks and the ROM pool's sectors, never newlib-nano's byte
 * loop (10 cycles a byte on the META in gm0).
 *   M0_COPY_ALIGNED(n, dst, src)  both word-aligned
 *   M0_COPY(n, dst, src)          dst word-aligned, src anywhere (the
 *                                 variant of src's misalignment)
 * n: 16, 32, 64, 128, 256 or 512, a constant. */
#ifndef M0_COPY_H
#define M0_COPY_H
#include <stdint.h>
#include <string.h>

#if defined(__ARM_ARCH_6M__)
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*m0_copy_fn)(void *dst, const void *src);
#define M0_COPY_DECLARE(n)                                                                           \
    __attribute__((long_call)) void m0_copy##n##_0(void *dst, const void *src);                     \
    __attribute__((long_call)) void m0_copy##n##_1(void *dst, const void *src);                     \
    __attribute__((long_call)) void m0_copy##n##_2(void *dst, const void *src);                     \
    __attribute__((long_call)) void m0_copy##n##_3(void *dst, const void *src);                     \
    static const m0_copy_fn m0_copy##n##_by[4] __attribute__((unused)) = {                           \
        m0_copy##n##_0, m0_copy##n##_1, m0_copy##n##_2, m0_copy##n##_3 };
M0_COPY_DECLARE(16)
M0_COPY_DECLARE(32)
M0_COPY_DECLARE(64)
M0_COPY_DECLARE(128)
M0_COPY_DECLARE(256)
M0_COPY_DECLARE(512)
#undef M0_COPY_DECLARE
#ifdef __cplusplus
}
#endif
/* one level more: n may be a macro (M0C_BLOCK) */
#define M0_COPY_ALIGNED(n, dst, src) M0_COPY_ALIGNED_(n, dst, src)
#define M0_COPY(n, dst, src) M0_COPY_(n, dst, src)
#define M0_COPY_ALIGNED_(n, dst, src) m0_copy##n##_0((dst), (src))
#define M0_COPY_(n, dst, src) m0_copy##n##_by[(uintptr_t)(src) & 3u]((dst), (src))
#else
#define M0_COPY_ALIGNED(n, dst, src) memcpy((dst), (src), (n))
#define M0_COPY(n, dst, src) memcpy((dst), (src), (n))
#endif
#endif
