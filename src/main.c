/* main.c — точка входа: конфигурация, REPL-команды, маршрутизация действий LLM */
#include "common.h"
#include "http.h"
#include "json.h"
#include "files.h"
#include "tts.h"
#include "term.h"
#include "llm.h"
#include "gfx.h"
#include <math.h>

typedef struct {
    LlmConfig    llm;
    TtsSettings  tts;
    TermSession *term;
    int          auto_speak;   /* озвучивать ответы LLM автоматически */
    int          auto_actions; /* выполнять действия из ответов автоматически */
    int          stream;       /* потоковая генерация (по умолчанию вкл) */
    GfxServer   *gfx;          /* визуальный интерфейс активности/рисования */
} Host;

static const char *CFG_PATH = "lhost.cfg";

/* ---------- конфиг (key=value) ---------- */
static void cfg_save(Host *h) {
    Str s; str_init(&s);
    str_appendf(&s, "backend=%d\n", (int)h->llm.backend);
    str_appendf(&s, "base_url=%s\n", h->llm.base_url);
    str_appendf(&s, "model=%s\n", h->llm.model);
    str_appendf(&s, "temperature=%g\n", h->llm.temperature);
    str_appendf(&s, "max_tokens=%d\n", h->llm.max_tokens);
    str_appendf(&s, "timeout_ms=%d\n", h->llm.timeout_ms);
    str_appendf(&s, "tts_rate=%d\n", h->tts.rate_wpm);
    str_appendf(&s, "tts_volume=%d\n", h->tts.volume);
    str_appendf(&s, "auto_speak=%d\n", h->auto_speak);
    str_appendf(&s, "auto_actions=%d\n", h->auto_actions);
    str_appendf(&s, "stream=%d\n", h->stream);
    write_file_all(CFG_PATH, s.data, s.len);
    str_free(&s);
}

static void cfg_load(Host *h) {
    size_t n; char *raw = read_file_all(CFG_PATH, &n);
    if (!raw) return;
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if      (!strcmp(k, "backend"))     h->llm.backend = (LlmBackend)atoi(v);
        else if (!strcmp(k, "base_url"))    snprintf(h->llm.base_url, sizeof(h->llm.base_url), "%s", v);
        else if (!strcmp(k, "model"))       snprintf(h->llm.model, sizeof(h->llm.model), "%s", v);
        else if (!strcmp(k, "temperature")) h->llm.temperature = atof(v);
        else if (!strcmp(k, "max_tokens"))  h->llm.max_tokens = atoi(v);
        else if (!strcmp(k, "timeout_ms"))  h->llm.timeout_ms = atoi(v);
        else if (!strcmp(k, "tts_rate"))    tts_set_rate(&h->tts, atoi(v));
        else if (!strcmp(k, "tts_volume"))  tts_set_volume(&h->tts, atoi(v));
        else if (!strcmp(k, "auto_speak"))  h->auto_speak = atoi(v);
        else if (!strcmp(k, "auto_actions"))h->auto_actions = atoi(v);
        else if (!strcmp(k, "stream"))      h->stream = atoi(v);
    }
    free(raw);
}

/* ---------- runner действий из ответов LLM ---------- */
static double jnum0(const JVal *a, const char *k, double dflt) {
    const JVal *v = json_get(a, k);
    return v ? json_num(v, dflt) : dflt;
}

static int action_runner(const JVal *a, void *vctx) {
    Host *h = (Host *)vctx;
    const char *type = json_str(json_get(a, "type"));
    if (!type) return -1;
    gfx_event(h->gfx, GFX_EVT_ACTION, type);

    /* --- рисование на холсте интерфейса --- */
    if (!strcmp(type, "canvas_clear")) {
        gfx_clear(h->gfx, json_str(json_get(a, "bg")));
        printf("[canvas] cleared\n");
        return 0;
    }
    if (!strcmp(type, "canvas_line")) {
        gfx_line(h->gfx, jnum0(a,"x1",0), jnum0(a,"y1",0), jnum0(a,"x2",0), jnum0(a,"y2",0),
                 json_str(json_get(a, "color")), jnum0(a, "width", 2));
        printf("[canvas] line (%.0f,%.0f)-(%.0f,%.0f)\n", jnum0(a,"x1",0), jnum0(a,"y1",0), jnum0(a,"x2",0), jnum0(a,"y2",0));
        return 0;
    }
    if (!strcmp(type, "canvas_rect")) {
        gfx_rect(h->gfx, jnum0(a,"x",0), jnum0(a,"y",0), jnum0(a,"w",0), jnum0(a,"h",0),
                 json_str(json_get(a, "color")), (int)jnum0(a, "fill", 0));
        printf("[canvas] rect %.0fx%.0f @ (%.0f,%.0f)\n", jnum0(a,"w",0), jnum0(a,"h",0), jnum0(a,"x",0), jnum0(a,"y",0));
        return 0;
    }
    if (!strcmp(type, "canvas_ellipse")) {
        gfx_ellipse(h->gfx, jnum0(a,"cx",0), jnum0(a,"cy",0), jnum0(a,"rx",0), jnum0(a,"ry",0),
                    json_str(json_get(a, "color")), (int)jnum0(a, "fill", 0));
        printf("[canvas] ellipse r=(%.0f,%.0f) @ (%.0f,%.0f)\n", jnum0(a,"rx",0), jnum0(a,"ry",0), jnum0(a,"cx",0), jnum0(a,"cy",0));
        return 0;
    }
    if (!strcmp(type, "canvas_text")) {
        gfx_text(h->gfx, jnum0(a,"x",0), jnum0(a,"y",20), json_str(json_get(a, "text")),
                 json_str(json_get(a, "color")), jnum0(a, "size", 16));
        printf("[canvas] text \"%s\"\n", json_str(json_get(a, "text")) ?: "");
        return 0;
    }

    if (!strcmp(type, "terminal_exec")) {
        const char *cmd = json_str(json_get(a, "cmd"));
        if (!cmd) return -1;
        if (!h->term && !(h->term = term_open(NULL))) { printf("[terminal] failed to open session\n"); return -1; }
        Str out; str_init(&out);
        int r = term_exec(h->term, cmd, 30000, &out);
        printf("[terminal_exec] rc=%d\n%s\n", r, out.data);
        str_free(&out);
        return r;
    }
    if (!strcmp(type, "terminal_run")) {
        const char *cmd = json_str(json_get(a, "cmd"));
        if (!cmd) return -1;
        Str out; str_init(&out); int rc = 0;
        term_run_once(NULL, cmd, &out, &rc);
        printf("[terminal_run] rc=%d\n%s\n", rc, out.data);
        str_free(&out);
        return 0;
    }
    if (!strcmp(type, "file_read")) {
        const char *path = json_str(json_get(a, "path"));
        if (!path) return -1;
        char *data; size_t n;
        if (host_file_read(path, &data, &n) != 0) { printf("[file_read] %s\n", host_last_fs_error()); return -1; }
        printf("[file_read] %s (%zu bytes):\n%.4096s%s\n", path, n, data, n > 4096 ? "\n...[truncated]" : "");
        free(data);
        return 0;
    }
    if (!strcmp(type, "file_write")) {
        const char *path = json_str(json_get(a, "path"));
        const char *content = json_str(json_get(a, "content"));
        if (!path || !content) return -1;
        int r = host_file_write(path, content, strlen(content));
        printf("[file_write] %s -> %s\n", path, r == 0 ? "ok" : host_last_fs_error());
        return r;
    }
    if (!strcmp(type, "speak")) {
        const char *text = json_str(json_get(a, "text"));
        if (!text) return -1;
        return tts_speak(&h->tts, text);
    }
    if (!strcmp(type, "http_get")) {
        const char *url = json_str(json_get(a, "url"));
        if (!url) return -1;
        HttpResponse r;
        if (http_request("GET", url, NULL, NULL, 20000, &r) != 0) {
            printf("[http_get] error: %s\n", r.error); http_response_free(&r); return -1;
        }
        printf("[http_get] HTTP %d, body %.2048s%s\n", r.status, r.body.data,
               r.body.len > 2048 ? "\n...[truncated]" : "");
        http_response_free(&r);
        return 0;
    }
    printf("[actions] unknown type '%s'\n", type);
    return -1;
}

/* ---------- отправка запроса в LLM ---------- */
static int print_delta(const char *text, size_t n, void *ctx) {
    (void)ctx;
    fwrite(text, 1, n, stdout);
    fflush(stdout);
    return 0;
}

static void do_ask(Host *h, const char *prompt) {
    printf("[llm] querying %s @ %s %s...\n", h->llm.model, h->llm.base_url,
           h->stream ? "(stream)" : "");
    char *reply = NULL, *error = NULL;
    int rc;
    if (h->stream) {
        printf("--- answer (streaming) ---\n");
        rc = llm_chat_stream(&h->llm, h->gfx, prompt, print_delta, NULL, &reply, &error);
        if (rc == 0) putchar('\n');
    } else {
        rc = llm_chat(&h->llm, prompt, &reply, &error);
    }
    if (rc != 0) {
        printf("[llm] ERROR: %s\n", error ? error : "?");
        gfx_event(h->gfx, GFX_EVT_ERROR, error ? error : "llm error");
        free(error);
        return;
    }
    gfx_tokens_total(h->gfx, 0); /* no-op, счётчик ведёт gfx_token */
    gfx_event(h->gfx, GFX_EVT_DONE, NULL);
    if (!h->stream) printf("--- answer ---\n%s\n--------------\n", reply);
    else            printf("--------------\n");
    if (h->auto_speak) {
        /* озвучиваем без блоков действий */
        tts_speak(&h->tts, reply);
    }
    if (h->auto_actions) {
        Str log; str_init(&log);
        int n = llm_apply_actions(reply, action_runner, h, &log);
        if (log.len) printf("--- actions ---\n%s(%d executed)\n", log.data, n);
        str_free(&log);
    }
    free(reply);
}

/* ---------- справка ---------- */
static void help(void) {
    printf(
"=== llm-host (C) commands ===\n"
" ask <prompt>          send request to local LLM (reply as text + actions)\n"
" models                list models of the configured server\n"
" connect [ollama|openai] <url> [model]   switch backend/url/model + check\n"
" temp <x> | tokens <n> | timeout <ms>    tune generation\n"
" speak <text>          TTS output\n"
" rate <wpm> | vol <0-100> | voices | voice <i> | tts on|off   voice control\n"
" autospeak on|off      auto-speak LLM answers\n"
" stream on|off         streaming generation (tokens arrive live)\n"
" ui [port]             open visual interface (activity graph + model canvas)\n"
" draw demo             host-side demo drawing on the model canvas\n"
" autoact on|off        auto-execute actions from LLM answers\n"
" term start|stop|status|exec <cmd>|run <cmd>|ps|kill <pid|name>|env <NAME>|setenv <K> <V>\n"
" term spawn <title> <cmd>                new console window (Windows)\n"
" ls [dir] | cat <path> | write <path> <text> | append <path> <text>\n"
" get <url>             HTTP GET (internet access)\n"
" post <url> <json>     HTTP POST\n"
" save                  save config | quit\n");
}

/* ---------- REPL ---------- */
int main(int argc, char **argv) {
    Host h;
    memset(&h, 0, sizeof(h));
    llm_config_default(&h.llm);
    h.auto_speak = 0;
    h.auto_actions = 1;
    h.stream = 1;

    http_global_init();
    tts_init(&h.tts);
    cfg_load(&h);
    if (gfx_start(&h.gfx, 0) == 0)
        printf("visual interface: %s (activity graph + drawing canvas)\n", gfx_url(h.gfx));
    else
        printf("note: visual interface not started (port busy?), use 'ui [port]'\n");

    printf("llm-host v1.0 (C). Backend=%s model=%s url=%s\n",
           h.llm.backend == BACKEND_OLLAMA ? "ollama" : "openai-compat",
           h.llm.model, h.llm.base_url);
    printf("Type 'help' or Ctrl-D to exit.\n");

    char line[8192];
    FILE *in = stdin;
    if (argc > 1) { /* пакетный режим: lhost "ask ...": exec ... по строкам файла? просто один ask */
        /* собираем все аргументы как один промпт */
        Str p; str_init(&p);
        for (int i = 1; i < argc; i++) { if (i>1) str_append(&p, " "); str_append(&p, argv[i]); }
        do_ask(&h, p.data);
        str_free(&p);
        goto cleanup;
    }

    for (;;) {
        printf("lhost> "); fflush(stdout);
        if (!fgets(line, sizeof(line), in)) break;
        const char *cmd = str_trim(line);
        if (!*cmd) continue;

        if (!strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) break;
        else if (!strcmp(cmd, "save")) { cfg_save(&h); printf("config saved to %s\n", CFG_PATH); }
        else if (str_startswith_ci(cmd, "ask ")) do_ask(&h, cmd + 4);
        else if (!strcmp(cmd, "models")) {
            Str o; str_init(&o);
            int r = llm_check(&h.llm, &o);
            printf("%s%s\n", o.data, r == 0 ? "" : "(server unreachable)");
            str_free(&o);
        }
        else if (str_startswith_ci(cmd, "connect ")) {
            char b[16], u[256], m[128] = "";
            int nf = sscanf(cmd + 8, "%15s %255s %127s", b, u, m);
            if (nf >= 2) {
                h.llm.backend = !str_iequals(b, "openai") && !str_iequals(b, "lmstudio") && !str_iequals(b, "kobold")
                              ? BACKEND_OLLAMA : BACKEND_OPENAI_COMPAT;
                snprintf(h.llm.base_url, sizeof(h.llm.base_url), "%s", u);
                if (nf == 3) snprintf(h.llm.model, sizeof(h.llm.model), "%s", m);
                Str o; str_init(&o);
                int r = llm_check(&h.llm, &o);
                printf("%s\n", o.data);
                str_free(&o);
                if (r == 0) printf("connected.\n");
            } else printf("usage: connect [ollama|openai] <http://host:port> [model]\n");
        }
        else if (str_startswith_ci(cmd, "temp "))    { h.llm.temperature = atof(cmd+5); printf("temperature=%g\n", h.llm.temperature); }
        else if (str_startswith_ci(cmd, "tokens "))  { h.llm.max_tokens = atoi(cmd+7); printf("max_tokens=%d\n", h.llm.max_tokens); }
        else if (str_startswith_ci(cmd, "timeout ")) { h.llm.timeout_ms = atoi(cmd+8); printf("timeout_ms=%d\n", h.llm.timeout_ms); }
        else if (str_startswith_ci(cmd, "speak "))   { printf("speaking...\n"); tts_speak(&h.tts, cmd+6); }
        else if (str_startswith_ci(cmd, "rate "))    { printf("%s\n", tts_set_rate(&h.tts, atoi(cmd+5))==0?"ok":"bad rate"); }
        else if (str_startswith_ci(cmd, "vol "))     { printf("%s\n", tts_set_volume(&h.tts, atoi(cmd+4))==0?"ok":"bad volume"); }
        else if (!strcmp(cmd, "voices")) {
            Str o; str_init(&o);
            tts_list_voices(&o);
            printf("%s\n", o.data);
            str_free(&o);
        }
        else if (str_startswith_ci(cmd, "voice "))   { printf("%s\n", tts_select_voice(&h.tts, atoi(cmd+6))==0?"ok":"failed"); }
        else if (str_startswith_ci(cmd, "tts "))     { h.tts.enabled = !strcmp(cmd+4,"on"); printf("tts %s\n", cmd+4); }
        else if (str_startswith_ci(cmd, "autospeak ")) { h.auto_speak = !strcmp(cmd+9,"on"); printf("autospeak %s\n", cmd+9); }
        else if (str_startswith_ci(cmd, "autoact "))  { h.auto_actions = !strcmp(cmd+8,"on"); printf("autoact %s\n", cmd+8); }
        else if (str_startswith_ci(cmd, "stream "))   { h.stream = !strcmp(cmd+7,"on"); printf("stream %s\n", cmd+7); }
        else if (!strcmp(cmd, "ui") || str_startswith_ci(cmd, "ui ")) {
            int port = cmd[2] ? atoi(cmd+3) : 0;
            if (h.gfx) { printf("UI already running: %s\n", gfx_url(h.gfx)); }
            else if (gfx_start(&h.gfx, port) == 0) {
                printf("visual interface: %s (open in browser)\n", gfx_url(h.gfx));
                gfx_event(h.gfx, GFX_EVT_IDLE, "host ready");
            } else printf("failed to start UI server (port busy?)\n");
        }
        else if (!strcmp(cmd, "draw demo")) {
            if (!h.gfx && gfx_start(&h.gfx, 0) != 0) { printf("cannot start UI\n"); }
            else {
                gfx_clear(h.gfx, "#ffffff");
                gfx_rect(h.gfx, 40, 40, 820, 560, "#233052", 0);
                gfx_ellipse(h.gfx, 700, 130, 60, 60, "#ffd23f", 1);          /* солнце */
                for (int i = 0; i < 8; i++) {                                /* лучи */
                    double a = i * 3.14159265 / 4;
                    gfx_line(h.gfx, 700 + cos(a)*75, 130 + sin(a)*75, 700 + cos(a)*100, 130 + sin(a)*100, "#ffd23f", 3);
                }
                gfx_ellipse(h.gfx, 200, 160, 70, 28, "#e8eefc", 1);          /* облака */
                gfx_ellipse(h.gfx, 250, 145, 55, 24, "#e8eefc", 1);
                gfx_rect(h.gfx, 0, 460, 900, 180, "#2e8b57", 1);             /* земля */
                gfx_rect(h.gfx, 380, 300, 180, 160, "#b0413e", 1);           /* дом */
                gfx_line(h.gfx, 360, 300, 470, 220, "#7a2c28", 4);
                gfx_line(h.gfx, 470, 220, 580, 300, "#7a2c28", 4);
                gfx_rect(h.gfx, 440, 380, 50, 80, "#5c3a21", 1);             /* дверь */
                gfx_rect(h.gfx, 400, 330, 40, 40, "#9ad0ec", 1);             /* окно */
                gfx_text(h.gfx, 60, 80, "LHost: нейросеть рисует сама", "#233052", 28);
                printf("demo drawn -> %s\n", gfx_url(h.gfx));
            }
        }
        else if (str_startswith_ci(cmd, "term ")) {
            const char *sub = cmd + 5;
            if (!strncmp(sub, "start", 5)) {
                if (h.term) printf("session already open (pid %ld)\n", term_pid(h.term));
                else { h.term = term_open(NULL); printf(h.term ? "session opened, pid %ld\n" : "FAILED to open session\n", term_pid(h.term)); }
            } else if (!strncmp(sub, "stop", 4)) { term_close(h.term); h.term = NULL; printf("closed\n"); }
            else if (!strncmp(sub, "status", 6)) { printf(h.term ? "alive=%d pid=%ld\n" : "no session\n", term_is_alive(h.term), term_pid(h.term)); }
            else if (!strncmp(sub, "exec ", 5)) {
                if (!h.term && !(h.term = term_open(NULL))) { printf("cannot open session\n"); }
                else { Str o; str_init(&o); int r = term_exec(h.term, sub+5, 30000, &o); printf("rc=%d\n%s\n", r, o.data); str_free(&o); }
            } else if (!strncmp(sub, "run ", 4)) {
                Str o; str_init(&o); int rc = 0;
                term_run_once(NULL, sub+4, &o, &rc);
                printf("rc=%d\n%s\n", rc, o.data); str_free(&o);
            } else if (!strncmp(sub, "ps", 2)) {
                Str o; str_init(&o); term_list_processes(&o); printf("%s\n", o.data); str_free(&o);
            } else if (!strncmp(sub, "kill ", 5)) {
                Str o; str_init(&o); int r = term_kill_process(sub+5, 1, &o); printf("rc=%d %s\n", r, o.data); str_free(&o);
            } else if (!strncmp(sub, "spawn ", 6)) {
                char t[128], c[1024];
                if (sscanf(sub+6, "%127[^ ] %1023[^\n]", t, c) >= 1)
                    printf("%s\n", term_spawn_window(t, c)==0 ? "window spawned" : "spawn failed (Windows only?)");
            } else if (!strncmp(sub, "env ", 4)) {
                if (!h.term) { printf("no session; use 'term start'\n"); }
                else { Str o; str_init(&o); term_getenv(h.term, sub+4, &o); printf("%s\n", o.data); str_free(&o); }
            } else if (!strncmp(sub, "setenv ", 7)) {
                char k[128], v[512];
                if (sscanf(sub+7, "%127s %511[^\n]", k, v) == 2 && h.term) {
                    printf("%s\n", term_setenv(h.term, k, v)==0 ? "ok" : "failed");
                } else printf("usage: term setenv NAME VALUE (needs session)\n");
            } else printf("unknown term subcommand\n");
        }
        else if (str_startswith_ci(cmd, "ls ") || !strcmp(cmd, "ls")) {
            FileEntry *arr = NULL;
            int n = host_list_dir(cmd[2] ? cmd+3 : ".", &arr);
            if (n < 0) printf("ls: %s\n", host_last_fs_error());
            for (int i = 0; i < n; i++)
                printf("%c %10lld  %s\n", arr[i].is_dir ? 'd' : '-', arr[i].size, arr[i].path);
            for (int i = 0; i < n; i++) free(arr[i].path);
            free(arr);
        }
        else if (str_startswith_ci(cmd, "cat ")) {
            char *data; size_t len;
            if (host_file_read(str_trim(cmd+4), &data, &len) == 0) { fwrite(data, 1, len, stdout); putchar('\n'); free(data); }
            else printf("cat: %s\n", host_last_fs_error());
        }
        else if (str_startswith_ci(cmd, "write ")) {
            char path[512], rest[6000];
            if (sscanf(cmd+6, "%511s %6000[^\n]", path, rest) == 2) {
                int r = host_file_write(path, rest, strlen(rest));
                printf("write %s -> %s\n", path, r==0?"ok":host_last_fs_error());
            } else printf("usage: write <path> <text>\n");
        }
        else if (str_startswith_ci(cmd, "append ")) {
            char path[512], rest[6000];
            if (sscanf(cmd+7, "%511s %6000[^\n]", path, rest) == 2) {
                Str withnl; str_init(&withnl); str_appendf(&withnl, "%s\n", rest);
                int r = host_file_append(path, withnl.data, withnl.len);
                printf("append %s -> %s\n", path, r==0?"ok":host_last_fs_error());
                str_free(&withnl);
            } else printf("usage: append <path> <text>\n");
        }
        else if (str_startswith_ci(cmd, "get ")) {
            HttpResponse r;
            if (http_request("GET", cmd+4, NULL, NULL, 20000, &r) == 0)
                printf("HTTP %d\n%.4096s%s\n", r.status, r.body.data, r.body.len>4096?"\n...[truncated]":"");
            else printf("error: %s\n", r.error);
            http_response_free(&r);
        }
        else if (str_startswith_ci(cmd, "post ")) {
            char url[512], body[4000];
            if (sscanf(cmd+5, "%511s %4000[^\n]", url, body) >= 1) {
                HttpResponse r;
                if (http_request("POST", url, body[0]?body:NULL, "application/json", 20000, &r) == 0)
                    printf("HTTP %d\n%.4096s\n", r.status, r.body.data);
                else printf("error: %s\n", r.error);
                http_response_free(&r);
            }
        }
        else printf("unknown command, try 'help'\n");
    }

cleanup:
    if (h.gfx) { gfx_event(h.gfx, GFX_EVT_IDLE, "host stopped"); gfx_stop(h.gfx); }
    if (h.term) term_close(h.term);
    tts_shutdown();
    http_global_cleanup();
    printf("bye.\n");
    return 0;
}
