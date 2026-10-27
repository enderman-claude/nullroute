/* util.c - byte buffers and small string helpers. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr.h"

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

static void buf_grow(Buf *b, size_t need)
{
    if (b->len + need + 1 <= b->cap) {
        return;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + need + 1) {
        cap *= 2;
    }
    b->data = realloc(b->data, cap);
    if (b->data == NULL) {
        abort();
    }
    b->cap = cap;
}

void buf_put(Buf *b, const char *s, size_t n)
{
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_str(Buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

void buf_char(Buf *b, char c)
{
    buf_put(b, &c, 1);
}

void buf_fmt(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        buf_grow(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

char *buf_steal(Buf *b)
{
    char *s = nr_strndup(b->data ? b->data : "", b->len);
    buf_free(b);
    return s;
}

char *nr_strndup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d == NULL) {
        abort();
    }
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

char *nr_strdup(const char *s)
{
    return nr_strndup(s, strlen(s));
}

/* FNV-1a. Not secure: the point is that the wordlist attack is quick. */
unsigned int nr_hash(const char *s)
{
    unsigned int h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}
