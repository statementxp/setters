/* llm.c — HTTP-запросы к локальным серверам моделей + экстрактор действий */
#include "llm.h"
#include "http.h"

void llm_config_default(LlmConfig *c) {
    memset(c, 0, sizeof(*c));
    c->backend = BACKEND_OLLAMA;
    strcpy(c->base_url, "http://localhost:11434");
    strcpy(c->model, "llama3.2");
    strcpy(c->system_prompt,
        "You are LHost, an assistant embedded in a Windows host application. "
        "You can act on the host by emitting JSON action blocks. To request an action, "
        "output a fenced block exactly like:\n"
        "```action\n"
        "{\"type\":\"terminal_exec\",\"cmd\":\"ipconfig /all\"}\n"
        "```\n"
        "Supported types: terminal_exec (run command in persistent shell), "
        "terminal_run (one-shot cmd), file_read {path}, file_write {path,content}, "
        "speak {text}, http_get {url}. You may combine prose and actions.\n"
        "You also have a live drawing canvas in the GUI. Create images THERE by drawing "
        "(not generating files). Drawing action types: "
        "canvas_clear {bg}, canvas_line {x1,y1,x2,y2,color,width}, "
        "canvas_rect {x,y,w,h,color,fill}, canvas_ellipse {cx,cy,rx,ry,color,fill}, "
        "canvas_text {x,y,text,color,size}. Coordinates are pixels on a 900x640 canvas, "
        "origin top-left. Colors are CSS names or #rrggbb. Emit many action blocks to "
        "compose a picture.");
    c->temperature = 0.7;
    c->max_tokens = 1024;
    c->timeout_ms = 120000;
}

static void url_join(Str *out, const char *base, const char *path) {
    size_t bl = strlen(base);
    str_append(out, base);
    if (bl && base[bl-1] == '/') out->len--, out->data[out->len] = 0;
    str_append(out, path);
}

int llm_check(LlmConfig *c, Str *out_models) {
    HttpResponse r;
    Str url; str_init(&url);
    if (c->backend == BACKEND_OLLAMA) url_join(&url, c->base_url, "/api/tags");
    else                              url_join(&url, c->base_url, "/models");
    int rc = http_request("GET", url.data, NULL, NULL, 8000, &r);
    str_free(&url);
    if (rc != 0) {
        str_appendf(out_models, "UNREACHABLE: %s\n", r.error);
        http_response_free(&r);
        return -1;
    }
    char *jerr = NULL;
    JVal *root = json_parse(r.body.data, &jerr);
    if (!root) {
        str_appendf(out_models, "HTTP %d, bad JSON: %s\n", r.status, jerr ? jerr : "?");
        free(jerr); http_response_free(&r); return -1;
    }
    const JVal *arr = c->backend == BACKEND_OLLAMA
                    ? json_path(root, "models", NULL, NULL)
                    : json_path(root, "data", NULL, NULL);
    if (arr && arr->type == JV_ARR) {
        int i = 0;
        for (const JVal *m = arr->items; m; m = m->next, i++) {
            const char *name = c->backend == BACKEND_OLLAMA
                ? json_str(json_get(m, "name"))
                : json_str(json_get(m, "id"));
            if (name) str_appendf(out_models, "[%d] %s\n", i, name);
        }
        if (i == 0) str_append(out_models, "(server reachable, no models listed)\n");
    } else {
        str_appendf(out_models, "HTTP %d, unexpected format\n", r.status);
    }
    json_free(root);
    http_response_free(&r);
    return 0;
}

static void build_body(LlmConfig *c, const char *prompt, int stream, Str *body) {
    if (c->backend == BACKEND_OLLAMA) {
        str_append(body, "{\"model\":");
        json_escape(body, c->model);
        str_appendf(body, ",\"stream\":%s,\"options\":{\"temperature\":%g,\"num_predict\":%d}",
                    stream ? "true" : "false", c->temperature, c->max_tokens);
        if (c->system_prompt[0]) { str_append(body, ",\"system\":"); json_escape(body, c->system_prompt); }
        str_append(body, ",\"prompt\":");
        json_escape(body, prompt);
        str_append(body, "}");
    } else {
        str_append(body, "{\"model\":");
        json_escape(body, c->model);
        str_appendf(body, ",\"stream\":%s,\"temperature\":%g,\"max_tokens\":%d,\"messages\":[",
                    stream ? "true" : "false", c->temperature, c->max_tokens);
        if (c->system_prompt[0]) {
            str_append(body, "{\"role\":\"system\",\"content\":");
            json_escape(body, c->system_prompt);
            str_append(body, "},");
        }
        str_append(body, "{\"role\":\"user\",\"content\":");
        json_escape(body, prompt);
        str_append(body, "}]}" );
    }
}

int llm_chat(LlmConfig *c, const char *user_prompt, char **reply, char **error) {
    *reply = NULL; *error = NULL;

    Str body; str_init(&body);
    build_body(c, user_prompt, 0, &body);

    Str url; str_init(&url);
    if (c->backend == BACKEND_OLLAMA) url_join(&url, c->base_url, "/api/generate");
    else                               url_join(&url, c->base_url, "/chat/completions");

    HttpResponse r;
    int rc = http_request("POST", url.data, body.data, "application/json", c->timeout_ms, &r);
    str_free(&url); str_free(&body);
    if (rc != 0) {
        Str e; str_init(&e); str_appendf(&e, "HTTP request failed: %s", r.error);
        *error = str_take(&e); http_response_free(&r); return -1;
    }
    char *jerr = NULL;
    JVal *root = json_parse(r.body.data, &jerr);
    if (!root) {
        Str e; str_init(&e); str_appendf(&e, "bad JSON from server (HTTP %d): %s | body head: %.200s",
                                          r.status, jerr ? jerr : "?", r.body.data);
        *error = str_take(&e); free(jerr); http_response_free(&r); return -1;
    }
    free(jerr);
    const JVal *txt = NULL;
    if (c->backend == BACKEND_OLLAMA) txt = json_get(root, "response");
    else {
        const JVal *choices = json_get(root, "choices");
        if (choices && choices->type == JV_ARR && choices->items) {
            const JVal *msg = json_get(choices->items, "message");
            txt = json_get(msg, "content");
        }
    }
    if (!txt || txt->type != JV_STR) {
        Str e; str_init(&e); str_appendf(&e, "no text field in response (HTTP %d)", r.status);
        *error = str_take(&e);
    } else {
        *reply = xstrdup(txt->s);
    }
    json_free(root);
    http_response_free(&r);
    return *reply ? 0 : -1;
}

/* ---------- потоковый запрос (SSE / NDJSON) ---------- */

/* извлекаем текстовый фрагмент из одной JSON-строки потока */
static int extract_delta(const char *line, LlmBackend backend, Str *out_text) {
    char *jerr = NULL;
    JVal *v = json_parse(line, &jerr);
    if (!v) { free(jerr); return -1; }
    const char *t = NULL;
    if (backend == BACKEND_OLLAMA) {
        t = json_str(json_get(v, "response"));                       /* /api/generate */
        if (!t) {
            const JVal *msg = json_get(v, "message");
            if (msg) t = json_str(json_get(msg, "content"));         /* /api/chat */
        }
    } else {
        const JVal *ch = json_get(v, "choices");
        if (ch && ch->type == JV_ARR && ch->items) {
            const JVal *d = json_get(ch->items, "delta");
            if (d) t = json_str(json_get(d, "content"));
            if (!t) {
                const JVal *msg = json_get(ch->items, "message");
                if (msg) t = json_str(json_get(msg, "content"));     /* some servers */
            }
        }
    }
    if (t) str_append(out_text, t);
    json_free(v);
    return t ? 0 : -1;
}

typedef struct {
    LlmBackend backend;
    GfxServer *gfx;
    LlmDeltaCb on_delta;
    void *user_ctx;
    Str linebuf;   /* недописанная строка между чанками */
    Str text;      /* накопленный полный ответ */
    int stopped;
} SseCtx;

static void sse_process_line(SseCtx *sc, const char *line, size_t len) {
    while (len && (line[len-1] == '\r' || line[len-1] == '\n')) len--;
    /* копия, чтобы гарантированно NUL-terminated */
    char *copy = xmalloc(len + 1);
    memcpy(copy, line, len); copy[len] = 0;
    const char *payload = copy;
    if (!strncmp(payload, "data:", 5)) { payload += 5; while (*payload == ' ') payload++; }
    if (payload[0] == '{') {
        Str delta; str_init(&delta);
        extract_delta(payload, sc->backend, &delta);
        if (delta.len) {
            str_append(&sc->text, delta.data);
            gfx_token(sc->gfx, delta.data, 0);
            if (sc->on_delta && sc->on_delta(delta.data, delta.len, sc->user_ctx) != 0)
                sc->stopped = 1;
        }
        str_free(&delta);
    }
    free(copy);
}

static int sse_chunk_cb(const char *data, size_t n, void *ctx) {
    SseCtx *sc = (SseCtx *)ctx;
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            size_t seg = i - start;
            Str cur; str_init(&cur);
            if (sc->linebuf.len) str_appendn(&cur, sc->linebuf.data, sc->linebuf.len);
            if (seg) str_appendn(&cur, data + start, seg);
            if (cur.len) sse_process_line(sc, cur.data, cur.len);
            str_free(&cur);
            sc->linebuf.len = 0;
            start = i + 1;
        }
    }
    if (start < n) str_appendn(&sc->linebuf, data + start, n - start);
    return sc->stopped ? -1 : 0;
}

int llm_chat_stream(LlmConfig *c, GfxServer *gfx, const char *user_prompt,
                    LlmDeltaCb on_delta, void *user_ctx,
                    char **reply, char **error) {
    *reply = NULL; *error = NULL;

    Str url; str_init(&url);
    if (c->backend == BACKEND_OLLAMA) url_join(&url, c->base_url, "/api/generate");
    else                               url_join(&url, c->base_url, "/chat/completions");

    Str body; str_init(&body);
    build_body(c, user_prompt, 1, &body);

    SseCtx sc;
    memset(&sc, 0, sizeof(sc));
    sc.backend = c->backend;
    sc.gfx = gfx;
    sc.on_delta = on_delta;
    sc.user_ctx = user_ctx;
    str_init(&sc.linebuf);
    str_init(&sc.text);

    gfx_event(gfx, GFX_EVT_THINK, user_prompt);

    HttpResponse r;
    int rc = http_request_stream("POST", url.data, body.data, "application/json",
                                 c->timeout_ms > 0 ? c->timeout_ms : 120000,
                                 sse_chunk_cb, &sc, &r);
    int status = r.status;
    char errbuf[256];
    snprintf(errbuf, sizeof(errbuf), "%s", r.error[0] ? r.error : "stream error");
    http_response_free(&r);
    str_free(&url); str_free(&body);

    if (status >= 400) {
        Str e; str_init(&e);
        str_appendf(&e, "model server HTTP %d%s%s", status,
                    errbuf[0] ? ": " : "", errbuf);
        *error = str_take(&e);
        str_free(&sc.linebuf); str_free(&sc.text);
        gfx_event(gfx, GFX_EVT_ERROR, "http error from model server");
        return -1;
    }
    if (rc != 0 && sc.text.len == 0) {
        *error = xstrdup(errbuf[0] ? errbuf : "stream failed");
        str_free(&sc.linebuf); str_free(&sc.text);
        gfx_event(gfx, GFX_EVT_ERROR, *error);
        return -1;
    }
    /* остаток буфера без завершающего \n */
    if (sc.linebuf.len) sse_process_line(&sc, sc.linebuf.data, sc.linebuf.len);
    str_free(&sc.linebuf);
    *reply = str_take(&sc.text);
    return 0;
}

/* ---------- экстрактор действий ---------- */

/* извлекает содержимое ```action ... ``` блоков и одиночные ACTION: {...} строки */
static char **extract_action_jsons(const char *text, int *count) {
    *count = 0;
    int cap = 8;
    char **list = xmalloc(sizeof(char*) * cap);
    const char *p = text;
    while ((p = strstr(p, "```action")) != NULL) {
        p += 9;
        const char *end = strstr(p, "```");
        if (!end) break;
        size_t n = (size_t)(end - p);
        while (n && (*p=='\r'||*p=='\n'||*p==' '||*p=='\t')) { p++; n--; }
        char *js = xmalloc(n + 1);
        memcpy(js, p, n); js[n] = 0;
        if (*count == cap) { cap *= 2; list = xrealloc(list, sizeof(char*)*cap); }
        list[(*count)++] = js;
        p = end + 3;
    }
    /* формат ACTION: {json} до конца строки */
    p = text;
    while ((p = strstr(p, "ACTION:")) != NULL) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '{') continue;
        /* поиск баланса скобок с учётом строк */
        const char *st = p; int depth = 0, instr = 0, esc = 0;
        for (; *p; p++) {
            if (esc) { esc = 0; continue; }
            if (*p == '\\') { esc = 1; continue; }
            if (*p == '"') instr = !instr;
            if (instr) continue;
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
        }
        if (depth != 0) continue;
        size_t n = (size_t)(p - st);
        char *js = xmalloc(n + 1);
        memcpy(js, st, n); js[n] = 0;
        if (*count == cap) { cap *= 2; list = xrealloc(list, sizeof(char*)*cap); }
        list[(*count)++] = js;
    }
    return list;
}

int llm_apply_actions(const char *reply_text, ActionRunner runner, void *ctx, Str *log_out) {
    int count = 0;
    char **jsons = extract_action_jsons(reply_text, &count);
    int executed = 0;
    for (int i = 0; i < count; i++) {
        char *jerr = NULL;
        JVal *a = json_parse(jsons[i], &jerr);
        if (!a) {
            str_appendf(log_out, "! invalid action JSON: %s\n", jerr ? jerr : "?");
            free(jerr); free(jsons[i]); continue;
        }
        const char *type = json_str(json_get(a, "type"));
        if (!type) { str_append(log_out, "! action without 'type'\n"); json_free(a); free(jsons[i]); continue; }
        str_appendf(log_out, "> action: %s\n", type);
        int r = runner(a, ctx);
        str_appendf(log_out, "  result: %s (%d)\n", r == 0 ? "ok" : "failed", r);
        if (r == 0) executed++;
        json_free(a);
        free(jsons[i]);
    }
    free(jsons);
    return executed;
}
