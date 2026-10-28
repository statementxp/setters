/* json.c — рекурсивный спуск, без выделений под каждый токен лишнего */
#include "json.h"

typedef struct { const char *p; const char *end; Str err; int ok; } P;

static JVal *parse_value(P *ps);

static void skip_ws(P *ps) {
    while (ps->p < ps->end && (*ps->p==' '||*ps->p=='\t'||*ps->p=='\n'||*ps->p=='\r')) ps->p++;
}

static JVal *jnew(JType t) {
    JVal *v = xmalloc(sizeof(JVal));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static unsigned hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void utf8_encode(Str *out, unsigned cp) {
    if (cp < 0x80) str_appendn(out, (char[]){(char)cp}, 1);
    else if (cp < 0x800) {
        char b[2] = { (char)(0xC0|(cp>>6)), (char)(0x80|(cp&0x3F)) };
        str_appendn(out, b, 2);
    } else if (cp < 0x10000) {
        char b[3] = { (char)(0xE0|(cp>>12)), (char)(0x80|((cp>>6)&0x3F)), (char)(0x80|(cp&0x3F)) };
        str_appendn(out, b, 3);
    } else {
        char b[4] = { (char)(0xF0|(cp>>18)), (char)(0x80|((cp>>12)&0x3F)),
                      (char)(0x80|((cp>>6)&0x3F)), (char)(0x80|(cp&0x3F)) };
        str_appendn(out, b, 4);
    }
}

static char *parse_string_raw(P *ps) { /* на '"' */
    if (*ps->p != '"') { str_appendf(&ps->err, "expected string at offset %ld", (long)(ps->p)); return NULL; }
    ps->p++;
    Str out; str_init(&out);
    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') return str_take(&out);
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case '"': str_appendn(&out, "\"", 1); break;
                case '\\': str_appendn(&out, "\\", 1); break;
                case '/': str_appendn(&out, "/", 1); break;
                case 'b': str_appendn(&out, "\b", 1); break;
                case 'f': str_appendn(&out, "\f", 1); break;
                case 'n': str_appendn(&out, "\n", 1); break;
                case 'r': str_appendn(&out, "\r", 1); break;
                case 't': str_appendn(&out, "\t", 1); break;
                case 'u': {
                    if (ps->end - ps->p < 4) { str_free(&out); return NULL; }
                    unsigned cp = 0;
                    for (int i = 0; i < 4; i++) cp = (cp<<4) | hexval((unsigned char)ps->p[i]);
                    ps->p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && ps->end - ps->p >= 6 &&
                        ps->p[0]=='\\' && ps->p[1]=='u') {
                        unsigned lo = 0;
                        for (int i = 0; i < 4; i++) lo = (lo<<4) | hexval((unsigned char)ps->p[2+i]);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            ps->p += 6;
                        }
                    }
                    utf8_encode(&out, cp);
                    break;
                }
                default: str_free(&out); return NULL;
            }
        } else {
            str_appendn(&out, &c, 1);
        }
    }
    str_append(&ps->err, "unterminated string");
    str_free(&out);
    return NULL;
}

static JVal *parse_number(P *ps) {
    const char *start = ps->p;
    if (*ps->p == '-' || *ps->p == '+') ps->p++;
    while (ps->p < ps->end && (isdigit((unsigned char)*ps->p) || *ps->p=='.' ||
           *ps->p=='e' || *ps->p=='E' || *ps->p=='-' || *ps->p=='+')) ps->p++;
    JVal *v = jnew(JV_NUM);
    size_t n = (size_t)(ps->p - start);
    v->s = xmalloc(n + 1);
    memcpy(v->s, start, n); v->s[n] = 0;
    v->num = strtod(v->s, NULL);
    return v;
}

static JVal *parse_array(P *ps) {
    ps->p++; /* [ */
    JVal *arr = jnew(JV_ARR);
    JVal **tail = &arr->items;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    for (;;) {
        JVal *item = parse_value(ps);
        if (!item) { json_free(arr); return NULL; }
        *tail = item; tail = &item->next;
        skip_ws(ps);
        if (ps->p >= ps->end) { str_append(&ps->err, "unterminated array"); json_free(arr); return NULL; }
        if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
        if (*ps->p == ']') { ps->p++; return arr; }
        str_append(&ps->err, "expected , or ] in array"); json_free(arr); return NULL;
    }
}

static JVal *parse_object(P *ps) {
    ps->p++; /* { */
    JVal *obj = jnew(JV_OBJ);
    JPair **tail = &obj->pairs;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (!key) { json_free(obj); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); str_append(&ps->err, "expected :"); json_free(obj); return NULL; }
        ps->p++;
        JVal *val = parse_value(ps);
        if (!val) { free(key); json_free(obj); return NULL; }
        JPair *pr = xmalloc(sizeof(JPair));
        pr->key = key; pr->val = val; pr->next = NULL;
        *tail = pr; tail = &pr->next;
        skip_ws(ps);
        if (ps->p >= ps->end) { str_append(&ps->err, "unterminated object"); json_free(obj); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return obj; }
        str_append(&ps->err, "expected , or } in object"); json_free(obj); return NULL;
    }
}

static JVal *parse_value(P *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { str_append(&ps->err, "unexpected end"); return NULL; }
    char c = *ps->p;
    if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s) return NULL;
        JVal *v = jnew(JV_STR); v->s = s; return v;
    }
    if (c == '{') return parse_object(ps);
    if (c == '[') return parse_array(ps);
    if (c == 't' && ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4))  { ps->p+=4; JVal*v=jnew(JV_BOOL); v->boolean=1; return v; }
    if (c == 'f' && ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) { ps->p+=5; JVal*v=jnew(JV_BOOL); v->boolean=0; return v; }
    if (c == 'n' && ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4))  { ps->p+=4; return jnew(JV_NULL); }
    if (c == '-' || c == '+' || isdigit((unsigned char)c)) return parse_number(ps);
    str_appendf(&ps->err, "bad token '%c' at offset %ld", c, (long)(ps->p));
    return NULL;
}

JVal *json_parse(const char *text, char **err) {
    P ps = { text, text + strlen(text), {0}, 0 };
    str_init(&ps.err);
    JVal *v = parse_value(&ps);
    if (!v && err) *err = str_take(&ps.err);
    str_free(&ps.err);
    return v;
}

void json_free(JVal *v) {
    while (v) {
        JVal *nx = v->next;
        free(v->s);
        while (v->pairs) { JPair *p = v->pairs; v->pairs = p->next; free(p->key); json_free(p->val); free(p); }
        json_free(v->items);
        free(v);
        v = nx;
    }
}

const JVal *json_get(const JVal *obj, const char *key) {
    if (!obj || obj->type != JV_OBJ) return NULL;
    for (JPair *p = obj->pairs; p; p = p->next)
        if (!strcmp(p->key, key)) return p->val;
    return NULL;
}

const JVal *json_path(const JVal *root, const char *a, const char *b, const char *c) {
    const JVal *v = root;
    if (a) { v = json_get(v, a); if (!v) return NULL; }
    if (b) { v = json_get(v, b); if (!v) return NULL; }
    if (c) { v = json_get(v, c); if (!v) return NULL; }
    return v;
}

const char *json_str(const JVal *v) { return (v && v->type == JV_STR) ? v->s : NULL; }

double json_num(const JVal *v, double dflt) {
    if (!v) return dflt;
    if (v->type == JV_NUM) return v->num;
    if (v->type == JV_BOOL) return v->boolean;
    return dflt;
}

void json_escape(Str *out, const char *s) {
    str_append(out, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  str_append(out, "\\\""); break;
            case '\\': str_append(out, "\\\\"); break;
            case '\n': str_append(out, "\\n");  break;
            case '\r': str_append(out, "\\r");  break;
            case '\t': str_append(out, "\\t");  break;
            default:
                if (*p < 0x20) str_appendf(out, "\\u%04x", *p);
                else str_appendn(out, (const char *)p, 1);
        }
    }
    str_append(out, "\"");
}
