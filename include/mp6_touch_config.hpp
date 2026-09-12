/* Shared, dependency-free defaults and strict layout codec for the flat config. */
#pragma once
#include "mp6_touch_pad.h"
#include <cstdio>
#include <cstring>

inline Mp6TouchConfig mp6_touch_defaults()
{
    Mp6TouchConfig c = {};
    c.enabled = 1;
    c.size = 100;
    c.opacity = 55;
    c.visible = (1u << MP6_TOUCH_COUNT) - 1;
    for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
        c.x[i] = c.y[i] = -1;
    return c;
}

inline const char *mp6_touch_name(int i)
{
    const char *names[] = {"Main Stick", "C-stick", "D-pad", "A", "B", "X", "Y", "Z", "L", "R", "Start"};
    return i >= 0 && i < MP6_TOUCH_COUNT ? names[i] : "";
}

/* Numeric ASCII only: no JSON escaping necessary. Versioned and transactional;
 * malformed/oversized integers, partial layouts and trailing data are rejected. */
inline bool mp6_touch_layout_read(const char *s, Mp6TouchConfig &c)
{
    if (std::strncmp(s, "1;", 2))
        return false;
    s += 2;
    auto number = [&s](int &v, char delimiter)
    {
        bool neg = *s == '-';
        if (neg)
            ++s;
        if (*s < '0' || *s > '9')
            return false;
        int n = 0;
        while (*s >= '0' && *s <= '9')
        {
            n = n * 10 + *s++ - '0';
            if (n > 10000)
                return false;
        }
        if (*s != delimiter)
            return false;
        ++s;
        v = neg ? -n : n;
        return true;
    };
    Mp6TouchConfig parsed = c;
    int mask;
    if (!number(mask, ';') || mask < 0 || mask >= (1 << MP6_TOUCH_COUNT))
        return false;
    parsed.visible = (unsigned)mask;
    for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
    {
        if (!number(parsed.x[i], ',') || !number(parsed.y[i], ';'))
            return false;
        if (parsed.x[i] < -1 || parsed.y[i] < -1 || ((parsed.x[i] == -1) != (parsed.y[i] == -1)))
            return false;
    }
    if (*s)
        return false;
    c = parsed;
    return true;
}

inline void mp6_touch_layout_write(const Mp6TouchConfig &c, char (&out)[256])
{
    size_t used = (size_t)std::snprintf(out, sizeof(out), "1;%u;", c.visible);
    for (int i = 0; i < MP6_TOUCH_COUNT; ++i)
        used += (size_t)std::snprintf(out + used, sizeof(out) - used, "%d,%d;", c.x[i], c.y[i]);
}
