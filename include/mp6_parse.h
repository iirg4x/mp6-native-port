/* Strict parsers for operational runtime controls (never partial tokens). */
#ifndef MP6_PARSE_H
#define MP6_PARSE_H

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static inline int mp6_parse_i32_strict(const char *text, int minValue,
                                       int maxValue, int *out)
{
    char *end = NULL;
    long value;
    if (text == NULL || text[0] == '\0' || out == NULL || minValue > maxValue) return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value < minValue || value > maxValue) return 0;
    *out = (int)value;
    return 1;
}

static inline int mp6_parse_u32_span(const char *text, size_t len,
                                     uint32_t minValue, uint32_t maxValue,
                                     uint32_t *out)
{
    uint32_t value = 0;
    size_t i;
    if (text == NULL || len == 0 || out == NULL || minValue > maxValue) return 0;
    for (i = 0; i < len; i++) {
        uint32_t digit;
        if (text[i] < '0' || text[i] > '9') return 0;
        digit = (uint32_t)(text[i] - '0');
        if (value > maxValue / 10u ||
            (value == maxValue / 10u && digit > maxValue % 10u)) return 0;
        value = value * 10u + digit;
    }
    if (value < minValue || value > maxValue) return 0;
    *out = value;
    return 1;
}

static inline int mp6_parse_double_strict(const char *text, double minValue,
                                          double maxValue, double *out)
{
    char *end = NULL;
    double value;
    if (text == NULL || text[0] == '\0' || out == NULL || minValue > maxValue) return 0;
    errno = 0;
    value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(value) ||
        value < minValue || value > maxValue) return 0;
    *out = value;
    return 1;
}

#endif /* MP6_PARSE_H */
