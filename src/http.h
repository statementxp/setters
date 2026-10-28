/* http.h — HTTP/1.1 клиент поверх BSD-сокетов (без libcurl) */
#ifndef LHOST_HTTP_H
#define LHOST_HTTP_H

#include "common.h"

typedef struct {
    int status;
    Str body;
    Str headers;   /* сырые заголовки ответа */
    char error[256];
} HttpResponse;

void http_global_init(void);
void http_global_cleanup(void);

/* method: "GET"/"POST"; url: http://host[:port]/path (https НЕ поддерживается этим
 * модулем — для локальных серверов Ollama/LM Studio используется http).
 * body/content_type могут быть NULL. timeout_ms — общий таймаут. */
int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 int timeout_ms, HttpResponse *out); /* 0 — успех */

/* Потоковый вариант: chunk_cb вызывается по мере поступления декодированного
 * тела (chunked/SSE/обычного). Статус и заголовки парсятся из первой пачки.
 * Возвращает 0 при успехе; chunk_cb может вернуть -1 для досрочного прекращения. */
typedef int (*HttpChunkCb)(const char *data, size_t n, void *ctx);
int http_request_stream(const char *method, const char *url,
                        const char *body, const char *content_type,
                        int idle_timeout_ms, HttpChunkCb chunk_cb, void *ctx,
                        HttpResponse *out);

void http_response_free(HttpResponse *r);

#endif
