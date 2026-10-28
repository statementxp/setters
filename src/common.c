/* common.c — реализация утилит */
#include "common.h"

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "FATAL: out of memory (%zu bytes)\n", n); exit(2); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "FATAL: out of memory (realloc %zu)\n", n); exit(2); }
    return q;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

void log_msg(const char *file, int line, const char *fmt, ...) {
    time_t t = time(NULL);
    struct tm tmv;
#if LHOST_WIN
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    const char *base = strrchr(file, '/');
    if (!base) base = strrchr(file, '\\');
    base = base ? base + 1 : file;
    fprintf(stderr, "[%s] %s:%d: ", ts, base, line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---------- Str ---------- */
static void str_grow(Str *s, size_t need /* всего байт с \0 */) {
    if (s->cap >= need) return;
    size_t nc = s->cap ? s->cap : 64;
    while (nc < need) nc *= 2;
    s->data = xrealloc(s->data, nc);
    s->cap = nc;
}

void str_init(Str *s) { s->data = NULL; s->len = 0; s->cap = 0; str_reset(s); }

void str_reset(Str *s) {
    str_grow(s, 64);
    s->data[0] = '\0';
    s->len = 0;
}

void str_free(Str *s) { free(s->data); s->data = NULL; s->len = s->cap = 0; }

void str_appendn(Str *s, const char *text, size_t n) {
    if (!n) return;
    str_grow(s, s->len + n + 1);
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void str_append(Str *s, const char *text) {
    if (text) str_appendn(s, text, strlen(text));
}

void str_appendf(Str *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { str_appendn(s, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    str_appendn(s, big, (size_t)n);
    free(big);
}

char *str_take(Str *s) {
    if (!s->data) { s->data = xmalloc(1); s->data[0] = 0; s->cap = 1; }
    char *r = s->data;
    s->data = NULL; s->len = 0; s->cap = 0;
    return r;
}

/* ---------- Buf ---------- */
void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
void buf_append(Buf *b, const void *data, size_t n) {
    if (!n) return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) nc *= 2;
        b->data = xrealloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

/* ---------- файлы ---------- */
char *read_file_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b; buf_init(&b);
    unsigned char chunk[8192];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_append(&b, chunk, r);
    fclose(f);
    buf_append(&b, "", 1); /* \0 */
    if (out_len) *out_len = b.len - 1;
    return (char *)b.data; /* владение переходит вызывающему */
}

int write_file_all(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

/* ---------- строки ---------- */
int str_iequals(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int str_startswith_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return 0;
        s++; prefix++;
    }
    return 1;
}

void str_tolower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

const char *str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}
