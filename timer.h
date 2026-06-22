/* =========================================================================
 * TIMER — кроссплатформенный высокоточный таймер
 * =========================================================================
 *
 * macOS/Linux: clock_gettime(CLOCK_MONOTONIC)
 * Windows:     QueryPerformanceCounter
 * ========================================================================= */

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)

#include <windows.h>

static inline double timer_sec(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

#else

#include <time.h>

static inline double timer_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#endif

#endif /* TIMER_H */