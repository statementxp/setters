/* term.h — внутренний интерфейс управления терминалом Windows (cmd/PowerShell).
 * На не-Windows платформах работает в режиме эмуляции через /bin/sh,
 * чтобы код можно было собирать и тестировать где угодно. */
#ifndef LHOST_TERM_H
#define LHOST_TERM_H

#include "common.h"

typedef struct TermSession TermSession;

TermSession *term_open(const char *shell); /* NULL → powershell.exe -NoProfile -Command - */
void         term_close(TermSession *s);
int          term_is_alive(TermSession *s);
long         term_pid(TermSession *s);

/* Выполнить команду в интерактивной сессии, дождаться вывода. timeout_ms. */
int term_exec(TermSession *s, const char *cmd, int timeout_ms, Str *out);

/* Одноразовое выполнение вне сессии (popen-подобное), с кодом возврата. */
int term_run_once(const char *shell_prefix, const char *cmd, Str *out, int *rc);

/* Открыть новое окно консоли с командой (только Windows). */
int term_spawn_window(const char *title, const char *cmd);

/* Управление процессами (Windows: tasklist/taskkill; POSIX: ps/kill). */
int term_list_processes(Str *out);
int term_kill_process(const char *pid_or_name, int force, Str *out);

/* Переменные окружения */
int term_getenv(TermSession *s, const char *name, Str *out);
int term_setenv(TermSession *s, const char *name, const char *value);

#endif
