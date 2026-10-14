/* common.h — общие утилиты для llm-host */
#ifndef LHOST_COMMON_H
#define LHOST_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  define LHOST_WIN 1
#else
#  define LHOST_WIN 0
#endif

#define LOG(fmt, ...) log_msg(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* --- динамическая строка --- */
typedef struct {
    char  *data;
    size_t len;   /* длина без \0 */
    size_t cap;   /* ёмкость с учётом \0 */
} Str;

void   str_init(Str *s);
void   str_free(Str *s);
void   str_reset(Str *s);
void   str_append(Str *s, const char *text);
void   str_appendn(Str *s, const char *text, size_t n);
void   str_appendf(Str *s, const char *fmt, ...);
char  *str_take(Str *s); /* возвращает владение data, Str очищается */

/* --- динамический буфер байтов --- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} Buf;

void  buf_init(Buf *b);
void  buf_free(Buf *b);
void  buf_append(Buf *b, const void *data, size_t n);

/* --- прочее --- */
void  log_msg(const char *file, int line, const char *fmt, ...);
char *xstrdup(const char *s);
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* Прочитать весь файл в newly-allocated строку. Возвращает NULL при ошибке. */
char *read_file_all(const char *path, size_t *out_len);

/* Записать строку в файл. 0 — успех. */
int   write_file_all(const char *path, const char *data, size_t len);

/* case-insensitive has: str_startswith_ci, lowercase copy */
int   str_iequals(const char *a, const char *b);
int   str_startswith_ci(const char *s, const char *prefix);
void  str_tolower(char *s);
const char *str_trim(char *s);

#endif /* LHOST_COMMON_H */
