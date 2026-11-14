/* The updaters' shared parts (switch/update.c, win/update_win.c): version compare, SHA-256 hex, and just enough
 * of GitHub's releases/latest JSON, never trusting its shape (NUL-terminated text). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* "1.10.2" > "1.9.9": numeric parts, missing ones 0. */
static int version_cmp(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* 64 hex digits -> 32 bytes */
static bool parse_sha(const char *hex, uint8_t out[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hexval(hex[2 * i]), lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Past the value at p (strings, nesting), NULL when it's cut off or malformed. */
static const char *skip(const char *p) {
    int depth = 0;
    p = ws(p);
    do {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return NULL;
            p++;
        } else if (*p == '{' || *p == '[') {
            if (++depth > 64) return NULL;
            p++;
        } else if (*p == '}' || *p == ']') {
            if (!depth--) return NULL;
            p++;
        } else if (!*p) {
            return NULL;
        } else {
            p += depth ? 1 : strcspn(p, ",}] \t\r\n"); /* a number, true, null */
        }
    } while (depth > 0);
    return p;
}

/* The value of the object at p's own member key (not a nested object's), NULL when absent. */
static const char *member(const char *p, const char *key) {
    size_t k = strlen(key);
    if (!p || *(p = ws(p)) != '{') return NULL;
    for (p++;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *name = p + 1, *v = skip(p);
        if (!v || *(v = ws(v)) != ':') return NULL;
        v = ws(v + 1);
        if (!strncmp(name, key, k) && name[k] == '"') return v;
        if (!(p = skip(v)) || *(p = ws(p)) != ',') return NULL;
        p++;
    }
}

/* A string value without escapes into out. */
static bool str_val(const char *v, char *out, size_t n) {
    if (!v || *v != '"') return false;
    size_t i = 0;
    for (v++; *v != '"'; v++) {
        if (!*v || *v == '\\' || i + 1 >= n) return false;
        out[i++] = *v;
    }
    out[i] = 0;
    return true;
}
