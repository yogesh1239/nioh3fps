/* Array-of-bytes patterns: "48 8B 05 ?? ?? ?? ??" ('?' or '??' = wildcard). */
#include "common.h"
#include <string.h>
#include <ctype.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int aob_parse(const char *s, aob_t *a)
{
    a->n = 0;
    while (*s) {
        if (*s == ' ' || *s == '\t' || *s == ',') { s++; continue; }
        if (a->n >= (int)sizeof a->b) return -1;
        if (*s == '?') {
            a->b[a->n] = 0; a->m[a->n] = 0; a->n++;
            s++; if (*s == '?') s++;
            continue;
        }
        if (hexval(s[0]) < 0 || hexval(s[1]) < 0) return -1;
        a->b[a->n] = (uint8_t)(hexval(s[0]) * 16 + hexval(s[1]));
        a->m[a->n] = 0xFF; a->n++;
        s += 2;
    }
    return a->n > 0 ? 0 : -1;
}

int aob_match_at(const uint8_t *p, const aob_t *a)
{
    for (int i = 0; i < a->n; i++)
        if ((p[i] & a->m[i]) != a->b[i]) return 0;
    return 1;
}

int aob_scan(const uint8_t *start, size_t len, const aob_t *a, uintptr_t *hits, int maxhits)
{
    int first = -1, nh = 0;
    size_t i;
    for (int k = 0; k < a->n; k++) if (a->m[k]) { first = k; break; }
    if (first < 0 || len < (size_t)a->n) return 0;
    for (i = 0; i + (size_t)a->n <= len;) {
        const uint8_t *q = memchr(start + i + first, a->b[first], len - (size_t)a->n - i + 1);
        size_t pos;
        if (!q) break;
        pos = (size_t)(q - start) - (size_t)first;
        if (aob_match_at(start + pos, a)) {
            if (nh < maxhits) hits[nh] = (uintptr_t)(start + pos);
            nh++;
        }
        i = pos + 1;
    }
    return nh;
}
