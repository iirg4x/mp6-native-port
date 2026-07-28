/* Small strict JSON lexical helpers for the launcher's flat config file. */
#ifndef MP6_JSON_H
#define MP6_JSON_H

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline const char *mp6_json_skip_ws(const char *p)
{
    if (p == NULL) return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static inline int mp6_json_hex4(const char *p, uint32_t *out)
{
    uint32_t value = 0;
    int i;
    if (p == NULL || out == NULL) return 0;
    for (i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)p[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return 0;
        value = (value << 4) | digit;
    }
    *out = value;
    return 1;
}

static inline int mp6_json_append_utf8(char *dst, size_t dstSize, size_t *used,
                                        uint32_t cp)
{
    unsigned char encoded[4];
    size_t count;
    if (used == NULL || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu) ||
        (dst != NULL && cp == 0)) return 0;
    if (cp <= 0x7Fu) {
        encoded[0] = (unsigned char)cp; count = 1;
    } else if (cp <= 0x7FFu) {
        encoded[0] = (unsigned char)(0xC0u | (cp >> 6));
        encoded[1] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 2;
    } else if (cp <= 0xFFFFu) {
        encoded[0] = (unsigned char)(0xE0u | (cp >> 12));
        encoded[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[2] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 3;
    } else {
        encoded[0] = (unsigned char)(0xF0u | (cp >> 18));
        encoded[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        encoded[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        encoded[3] = (unsigned char)(0x80u | (cp & 0x3Fu)); count = 4;
    }
    if (dst != NULL) {
        if (dstSize == 0 || *used > dstSize - 1 || count > dstSize - 1 - *used) return 0;
        memcpy(dst + *used, encoded, count);
    }
    *used += count;
    return 1;
}

/* Parses and UTF-8-decodes one JSON string. Passing dst=NULL validates and
 * skips without imposing an output-size cap. */
static inline const char *mp6_json_parse_string(const char *p, char *dst,
                                                 size_t dstSize)
{
    size_t used = 0;
    if (p == NULL || *p != '"' || (dst != NULL && dstSize == 0)) return NULL;
    ++p;
    while (*p != '\0' && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        uint32_t cp;
        if (c < 0x20u) return NULL;
        if (c == '\\') {
            unsigned char esc = (unsigned char)*p++;
            switch (esc) {
            case '"': cp = '"'; break;
            case '\\': cp = '\\'; break;
            case '/': cp = '/'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'u': {
                uint32_t first;
                if (!mp6_json_hex4(p, &first)) return NULL;
                p += 4;
                if (first >= 0xD800u && first <= 0xDBFFu) {
                    uint32_t second;
                    if (p[0] != '\\' || p[1] != 'u' ||
                        !mp6_json_hex4(p + 2, &second) ||
                        second < 0xDC00u || second > 0xDFFFu) return NULL;
                    p += 6;
                    cp = 0x10000u + ((first - 0xD800u) << 10) +
                         (second - 0xDC00u);
                } else {
                    if (first >= 0xDC00u && first <= 0xDFFFu) return NULL;
                    cp = first;
                }
                break;
            }
            default: return NULL;
            }
        } else if (c < 0x80u) {
            cp = c;
        } else {
            unsigned int trailing;
            uint32_t minCp;
            unsigned int i;
            if (c >= 0xC2u && c <= 0xDFu) {
                trailing = 1; cp = c & 0x1Fu; minCp = 0x80u;
            } else if (c >= 0xE0u && c <= 0xEFu) {
                trailing = 2; cp = c & 0x0Fu; minCp = 0x800u;
            } else if (c >= 0xF0u && c <= 0xF4u) {
                trailing = 3; cp = c & 0x07u; minCp = 0x10000u;
            } else return NULL;
            for (i = 0; i < trailing; ++i) {
                unsigned char next = (unsigned char)*p++;
                if ((next & 0xC0u) != 0x80u) return NULL;
                cp = (cp << 6) | (next & 0x3Fu);
            }
            if (cp < minCp || cp > 0x10FFFFu ||
                (cp >= 0xD800u && cp <= 0xDFFFu)) return NULL;
        }
        if (!mp6_json_append_utf8(dst, dstSize, &used, cp)) return NULL;
    }
    if (*p != '"') return NULL;
    if (dst != NULL) dst[used] = '\0';
    return p + 1;
}

/* Encode one already-NUL-terminated UTF-8 string for use between JSON quote
 * characters.  Never truncates: callers either receive a complete escaped
 * string or an empty destination and failure.  Bytes >= 0x20 are preserved;
 * the strict parser above validates UTF-8 when config text is loaded. */
static inline int mp6_json_escape_string(const char *src, char *dst,
                                         size_t dstSize)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0;
    if (src == NULL || dst == NULL || dstSize == 0) return 0;
    while (*src != '\0') {
        const unsigned char c = (unsigned char)*src++;
        const char *escape = NULL;
        size_t escapeSize = 0;
        char unicodeEscape[6];
        switch (c) {
        case '"':  escape = "\\\""; escapeSize = 2; break;
        case '\\': escape = "\\\\"; escapeSize = 2; break;
        case '\b': escape = "\\b";  escapeSize = 2; break;
        case '\f': escape = "\\f";  escapeSize = 2; break;
        case '\n': escape = "\\n";  escapeSize = 2; break;
        case '\r': escape = "\\r";  escapeSize = 2; break;
        case '\t': escape = "\\t";  escapeSize = 2; break;
        default:
            if (c < 0x20u) {
                unicodeEscape[0] = '\\'; unicodeEscape[1] = 'u';
                unicodeEscape[2] = '0';  unicodeEscape[3] = '0';
                unicodeEscape[4] = hex[c >> 4];
                unicodeEscape[5] = hex[c & 0x0Fu];
                escape = unicodeEscape;
                escapeSize = sizeof(unicodeEscape);
            }
            break;
        }
        if (escape != NULL) {
            if (used > dstSize - 1 || escapeSize > dstSize - 1 - used) goto too_small;
            memcpy(dst + used, escape, escapeSize);
            used += escapeSize;
        } else {
            if (used >= dstSize - 1) goto too_small;
            dst[used++] = (char)c;
        }
    }
    dst[used] = '\0';
    return 1;

too_small:
    dst[0] = '\0';
    return 0;
}

static inline const char *mp6_json_parse_number(const char *p, double *out)
{
    const char *start = p;
    char *convertedEnd = NULL;
    double value;
    if (p == NULL || out == NULL) return NULL;
    if (*p == '-') ++p;
    if (*p == '0') {
        ++p;
        if (*p >= '0' && *p <= '9') return NULL;
    } else {
        if (*p < '1' || *p > '9') return NULL;
        do { ++p; } while (*p >= '0' && *p <= '9');
    }
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return NULL;
        do { ++p; } while (*p >= '0' && *p <= '9');
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (*p < '0' || *p > '9') return NULL;
        do { ++p; } while (*p >= '0' && *p <= '9');
    }
    errno = 0;
    value = strtod(start, &convertedEnd);
    if (errno == ERANGE || convertedEnd != p || !isfinite(value)) return NULL;
    *out = value;
    return p;
}

static inline const char *mp6_json_skip_value_depth(const char *p,
                                                     unsigned int depth)
{
    if (p == NULL || depth > 32u) return NULL;
    p = mp6_json_skip_ws(p);
    if (*p == '"') return mp6_json_parse_string(p, NULL, 0);
    if (*p == '{') {
        p = mp6_json_skip_ws(p + 1);
        if (*p == '}') return p + 1;
        for (;;) {
            p = mp6_json_parse_string(p, NULL, 0);
            if (p == NULL) return NULL;
            p = mp6_json_skip_ws(p);
            if (*p++ != ':') return NULL;
            p = mp6_json_skip_value_depth(p, depth + 1u);
            if (p == NULL) return NULL;
            p = mp6_json_skip_ws(p);
            if (*p == '}') return p + 1;
            if (*p++ != ',') return NULL;
            p = mp6_json_skip_ws(p);
        }
    }
    if (*p == '[') {
        p = mp6_json_skip_ws(p + 1);
        if (*p == ']') return p + 1;
        for (;;) {
            p = mp6_json_skip_value_depth(p, depth + 1u);
            if (p == NULL) return NULL;
            p = mp6_json_skip_ws(p);
            if (*p == ']') return p + 1;
            if (*p++ != ',') return NULL;
            p = mp6_json_skip_ws(p);
        }
    }
    if (strncmp(p, "true", 4) == 0) return p + 4;
    if (strncmp(p, "false", 5) == 0) return p + 5;
    if (strncmp(p, "null", 4) == 0) return p + 4;
    {
        double ignored;
        return mp6_json_parse_number(p, &ignored);
    }
}

static inline const char *mp6_json_skip_value(const char *p)
{
    return mp6_json_skip_value_depth(p, 0);
}

#endif /* MP6_JSON_H */
