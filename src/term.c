/* term.c — интерактивная сессия терминала: пайпы + потоковый читатель + маркеры конца вывода.
 * Windows: CreatePipe/CreateProcess (powershell/cmd). POSIX: fork/exec + poll. */
#include "term.h"

#include <errno.h>

#if LHOST_WIN
#  include <windows.h>

struct TermSession {
    HANDLE in_w, out_r;      /* наши концы */
    PROCESS_INFORMATION pi;
    int    alive;
    long   cmd_counter;
    char   last_marker[64];
};

static int read_avail(HANDLE h, Buf *b, DWORD wait_ms) {
    unsigned char chunk[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;
        if (avail == 0) {
            Sleep(wait_ms);
            break;
        }
        DWORD n = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &n, NULL) || n == 0) break;
        buf_append(b, chunk, n);
    }
    return 0;
}

TermSession *term_open(const char *shell) {
    if (!shell) shell = "powershell.exe -NoProfile -NonInteractive -Command -";
    TermSession *s = xmalloc(sizeof(*s));
    memset(s, 0, sizeof(*s));

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE in_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &s->in_w, &sa, 0)) { free(s); return NULL; }
    if (!CreatePipe(&s->out_r, &out_w, &sa, 0)) { CloseHandle(in_r); free(s); return NULL; }
    SetHandleInformation(in_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(out_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    STARTUPINFOA si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.hStdInput = in_r;
    si.hStdOutput = si.hStdError = out_w;
    si.dwFlags = STARTF_USESTDHANDLES | CREATE_NO_WINDOW;

    /* разбираем shell → exe + cmdline */
    char exe[MAX_PATH], rest[4096];
    const char *sp = strchr(shell, ' ');
    if (!sp) { snprintf(exe, sizeof(exe), "%s", shell); rest[0] = 0; }
    else {
        size_t n = (size_t)(sp - shell);
        if (n >= sizeof(exe)) n = sizeof(exe)-1;
        memcpy(exe, shell, n); exe[n] = 0;
        snprintf(rest, sizeof(rest), " %s", sp + 1);
    }
    char cmdline[4200];
    snprintf(cmdline, sizeof(cmdline), "\"%s\"%s", exe, rest);

    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        DWORD e = GetLastError();
        LOG("CreateProcess('%s') failed: %lu", cmdline, (unsigned long)e);
        CloseHandle(in_r); CloseHandle(out_w);
        CloseHandle(s->in_w); CloseHandle(s->out_r);
        free(s); return NULL;
    }
    CloseHandle(pi.hThread);
    s->pi = pi;
    CloseHandle(in_r); CloseHandle(out_w);
    s->alive = 1;
    return s;
}

void term_close(TermSession *s) {
    if (!s) return;
    if (s->alive) {
        DWORD code = 0;
        if (WaitForSingleObject(s->pi.hProcess, 0) == WAIT_TIMEOUT)
            TerminateProcess(s->pi.hProcess, 0);
        GetExitCodeProcess(s->pi.hProcess, &code);
    }
    CloseHandle(s->in_w);
    CloseHandle(s->out_r);
    CloseHandle(s->pi.hProcess);
    free(s);
}

int term_is_alive(TermSession *s) {
    if (!s) return 0;
    return WaitForSingleObject(s->pi.hProcess, 0) == WAIT_TIMEOUT;
}

long term_pid(TermSession *s) { return s ? (long)s->pi.dwProcessId : -1; }

static void strip_ansi(Str *s) {
    /* удаляем ESC[...буква последовательности */
    Str out; str_init(&out);
    const char *p = s->data;
    while (*p) {
        if (*p == 0x1B && p[1] == '[') {
            p += 2;
            while (*p && !( (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') )) p++;
            if (*p) p++;
        } else str_appendn(&out, p++, 1);
    }
    str_free(s);
    *s = out;
}

int term_exec(TermSession *s, const char *cmd, int timeout_ms, Str *out) {
    if (!s || !s->alive) return -1;
    long id = ++s->cmd_counter;
    snprintf(s->last_marker, sizeof(s->last_marker), "###LHOST_END_%ld###", id);

    Str line; str_init(&line);
    str_appendf(&line, "%s\r\n(echo %s)\r\n", cmd, s->last_marker);
    DWORD wr = 0;
    BOOL ok = WriteFile(s->in_w, line.data, (DWORD)line.len, &wr, NULL);
    str_free(&line);
    if (!ok) { s->alive = 0; return -1; }

    Buf acc; buf_init(&acc);
    Str text; str_init(&text);
    time_t t0 = time(NULL);
    size_t marker_len = strlen(s->last_marker);
    for (;;) {
        read_avail(s->out_r, &acc, 50);
        /* переносим в text и ищем маркер */
        if (acc.len) {
            str_appendn(&text, (char *)acc.data, acc.len);
            buf_free(&acc); buf_init(&acc);
            char *pos = strstr(text.data, s->last_marker);
            if (pos) {
                size_t keep = (size_t)(pos - text.data);
                /* отрезаем сам маркер и хвост */
                text.data[keep] = '\0'; text.len = keep;
                break;
            }
            /* защита: если маркер на границе буфера — ждём ещё */
            if (text.len > 4u*1024*1024) break;
        }
        if ((time(NULL) - t0) * 1000 > timeout_ms) break;
        if (!term_is_alive(s)) break;
    }
    buf_free(&acc);
    strip_ansi(&text);
    /* PowerShell-промпт "> " иногда попадает в вывод — не критично */
    *out = text;
    (void)marker_len;
    return 0;
}

int term_run_once(const char *shell_prefix, const char *cmd, Str *out, int *rc) {
    if (!shell_prefix) shell_prefix = "powershell.exe -NoProfile -NonInteractive -Command";
    Str full; str_init(&full);
    str_appendf(&full, "%s \"%s\" 2>&1", shell_prefix, cmd);
    FILE *p = _popen(full.data, "r");
    str_free(&full);
    if (!p) return -1;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) str_appendn(out, buf, n);
    int code = _pclose(p);
    if (rc) *rc = code;
    return 0;
}

int term_spawn_window(const char *title, const char *cmd) {
    Str c; str_init(&c);
    str_appendf(&c, "start \"%s\" cmd.exe /K %s", title ? title : "LLM Host Console", cmd ? cmd : "");
    int rc = system(c.data);
    str_free(&c);
    return rc == 0 ? 0 : -1;
}

int term_list_processes(Str *out) {
    int rc = 0;
    return term_run_once("cmd.exe /C", "tasklist /FO TABLE", out, &rc);
}

int term_kill_process(const char *pid_or_name, int force, Str *out) {
    Str c; str_init(&c);
    int all_digits = 1;
    for (const char *p = pid_or_name; *p; p++) if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
    if (all_digits) str_appendf(&c, "taskkill /PID %s%s", pid_or_name, force ? " /F" : "");
    else            str_appendf(&c, "taskkill /IM %s%s", pid_or_name, force ? " /F" : "");
    Str o; str_init(&o); int rc = 0;
    term_run_once("cmd.exe /C", c.data, &o, &rc);
    str_append(out, o.data);
    str_free(&c); str_free(&o);
    return rc == 0 ? 0 : -1;
}

int term_getenv(TermSession *s, const char *name, Str *out) {
    Str c; str_init(&c);
    str_appendf(&c, "[Environment]::GetEnvironmentVariable('%s')", name);
    int r = term_exec(s, c.data, 15000, out);
    str_free(&c);
    return r;
}

int term_setenv(TermSession *s, const char *name, const char *value) {
    Str c; str_init(&c);
    str_appendf(&c, "$env:%s='%s'", name, value);
    Str o; str_init(&o);
    int r = term_exec(s, c.data, 15000, &o);
    str_free(&c); str_free(&o);
    return r;
}

#else /* ================= POSIX эмуляция (/bin/sh) ================= */

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>

struct TermSession {
    pid_t pid;
    int   fd_in;   /* пишем команду */
    int   fd_out;  /* читаем вывод */
    int   alive;
    long  cmd_counter;
};

TermSession *term_open(const char *shell) {
    (void)shell;
    TermSession *s = xmalloc(sizeof(*s));
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) || pipe(out_pipe)) { free(s); return NULL; }
    pid_t pid = fork();
    if (pid < 0) { free(s); return NULL; }
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    s->pid = pid; s->fd_in = in_pipe[1]; s->fd_out = out_pipe[0];
    s->alive = 1; s->cmd_counter = 0;
    /* проглотим приветственный баннер/промпты */
    struct pollfd pfd = { .fd = s->fd_out, .events = POLLIN };
    char drain[4096];
    while (poll(&pfd, 1, 200) > 0) { if (read(s->fd_out, drain, sizeof(drain)) <= 0) break; }
    return s;
}

void term_close(TermSession *s) {
    if (!s) return;
    if (s->alive) { write(s->fd_in, "exit\n", 5); kill(s->pid, SIGTERM); int sst; waitpid(s->pid, &sst, 0); }
    close(s->fd_in); close(s->fd_out);
    free(s);
}

int term_is_alive(TermSession *s) {
    if (!s) return 0;
    int st;
    pid_t r = waitpid(s->pid, &st, WNOHANG);
    if (r == s->pid) { s->alive = 0; return 0; }
    return s->alive;
}

long term_pid(TermSession *s) { return s ? (long)s->pid : -1; }

int term_exec(TermSession *s, const char *cmd, int timeout_ms, Str *out) {
    if (!s || !s->alive) return -1;
    long id = ++s->cmd_counter;
    char marker[64];
    snprintf(marker, sizeof(marker), "###LHOST_END_%ld###", id);
    Str line; str_init(&line);
    str_appendf(&line, "{ %s ; } 2>&1; echo %s\n", cmd, marker);
    ssize_t w = write(s->fd_in, line.data, line.len);
    str_free(&line);
    if (w <= 0) { s->alive = 0; return -1; }

    struct pollfd pfd = { .fd = s->fd_out, .events = POLLIN };
    char chunk[4096];
    for (;;) {
        int pr = poll(&pfd, 1, 100);
        if (pr > 0) {
            ssize_t n = read(s->fd_out, chunk, sizeof(chunk) - 1);
            if (n > 0) {
                chunk[n] = 0;
                str_append(out, chunk);
                char *pos = strstr(out->data, marker);
                if (pos) { *pos = 0; out->len = (size_t)(pos - out->data); break; }
            }
        }
        timeout_ms -= 100;
        if (timeout_ms <= 0 || !term_is_alive(s)) break;
    }
    return 0;
}

int term_run_once(const char *shell_prefix, const char *cmd, Str *out, int *rc) {
    (void)shell_prefix;
    Str full; str_init(&full);
    str_appendf(&full, "%s 2>&1", cmd);
    FILE *p = popen(full.data, "r");
    str_free(&full);
    if (!p) return -1;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) str_appendn(out, buf, n);
    int code = pclose(p);
    if (rc) *rc = WIFEXITED(code) ? WEXITSTATUS(code) : -1;
    return 0;
}

int term_spawn_window(const char *title, const char *cmd) {
    LOG("term_spawn_window: only supported on Windows (tried '%s')", title ? title : "");
    (void)cmd;
    return -1;
}

int term_list_processes(Str *out) {
    int rc = 0;
    return term_run_once(NULL, "ps aux | head -60", out, &rc);
}

int term_kill_process(const char *pid_or_name, int force, Str *out) {
    Str c; str_init(&c);
    int all_digits = 1;
    for (const char *p = pid_or_name; *p; p++) if (!isdigit((unsigned char)*p)) { all_digits = 0; break; }
    if (all_digits) str_appendf(&c, "kill %s%s", force ? "-9 " : "", pid_or_name);
    else            str_appendf(&c, "pkill %s%s", force ? "-9 " : "", pid_or_name);
    Str o; str_init(&o); int rc = 0;
    term_run_once(NULL, c.data, &o, &rc);
    if (out) str_append(out, o.data);
    str_free(&c); str_free(&o);
    return rc == 0 ? 0 : -1;
}

int term_getenv(TermSession *s, const char *name, Str *out) {
    Str c; str_init(&c);
    str_appendf(&c, "printenv %s", name);
    int r = term_exec(s, c.data, 5000, out);
    str_free(&c);
    return r;
}

int term_setenv(TermSession *s, const char *name, const char *value) {
    Str c; str_init(&c);
    str_appendf(&c, "export %s='%s'", name, value);
    Str o; str_init(&o);
    int r = term_exec(s, c.data, 5000, &o);
    str_free(&c); str_free(&o);
    return r;
}

#endif
