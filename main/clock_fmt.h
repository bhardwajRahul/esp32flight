/* Shared clock formatting: 24h "14:05" or 12h "2:05 PM" per settings. */
#pragma once

#include <stdio.h>
#include "settings.h"

static inline void clock_fmt(char *dst, size_t n, int hour, int min)
{
    if (settings_get()->clock_12h) {
        int h = hour % 12;
        if (h == 0) {
            h = 12;
        }
        snprintf(dst, n, "%d:%02d %s", h, min, hour < 12 ? "AM" : "PM");
    } else {
        snprintf(dst, n, "%02d:%02d", hour, min);
    }
}
