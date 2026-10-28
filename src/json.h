/* json.h — минимальный JSON: парсер + генератор (без внешних зависимостей) */
#ifndef LHOST_JSON_H
#define LHOST_JSON_H

#include "common.h"

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } JType;

typedef struct JVal {
    JType type;
    /* значение */
    char   *s;        /* JV_STR (decoded), JV_NUM как исходный текст */
    double  num;      /* JV_NUM */
    int     boolean;  /* JV_BOOL */
    /* контейнеры */
    struct JPair *pairs; /* JV_OBJ: список key:value */
    struct JVal  *items; /* JV_ARR: связный список через .next */
    struct JVal  *next;  /* для элементов массива/пар */
} JVal;

typedef struct JPair {
    char  *key;
    JVal  *val;
    struct JPair *next;
} JPair;

JVal       *json_parse(const char *text, char **err);
void        json_free(JVal *v);
const JVal *json_get(const JVal *obj, const char *key);       /* рекурсивно не ищет */
const JVal *json_path(const JVal *root, const char *a, const char *b, const char *c);
const char *json_str(const JVal *v);                          /* NULL если не строка */
double      json_num(const JVal *v, double dflt);

/* Экранирование строки для вставки в JSON */
void json_escape(Str *out, const char *s);

#endif
