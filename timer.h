/* =========================================================================
 * TIMER — кроссплатформенный, int64 тики
 * =========================================================================
 *
 * int64_t timer_ticks(void)  — текущее значение счётчика
 * int64_t timer_freq(void)   — тиков в секунду
 * elapsed_ms = (t1 - t0) * 1000 / freq
 * ========================================================================= */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

static inline int64_t timer_freq(void) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (int64_t)f.QuadPart;
}

static inline int64_t timer_ticks(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (int64_t)c.QuadPart;
}

#else

#include <time.h>

static inline int64_t timer_freq(void) {
    return 1000000000LL;  /* CLOCK_MONOTONIC = наносекунды */
}

static inline int64_t timer_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

#endif

#endif /* TIMER_H */