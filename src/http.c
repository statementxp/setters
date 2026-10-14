/* http.c — простой HTTP/1.1 клиент: connect, запрос, чтение до EOF или Content-Length */
#include "http.h"

#if LHOST_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define closesocket_ closesocket
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <poll.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define closesocket_ close
#endif

static int g_winsock_inited = 0;

void http_global_init(void) {
#if LHOST_WIN
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        fprintf(stderr, "WARN: WSAStartup failed\n");
    g_winsock_inited = 1;
#endif
}

void http_global_cleanup(void) {
#if LHOST_WIN
    if (g_winsock_inited) { WSACleanup(); g_winsock_inited = 0; }
#endif
}

static int wait_writable(sock_t s, int ms) {
#if !LHOST_WIN
    struct pollfd pfd = { .fd = s, .events = POLLOUT };
    return poll(&pfd, 1, ms) > 0;
#else
    fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, NULL, &wfds, NULL, &tv) > 0;
#endif
}

static int wait_readable(sock_t s, int ms) {
#if !LHOST_WIN
    struct pollfd pfd = { .fd = s, .events = POLLIN };
    return poll(&pfd, 1, ms) > 0;
#else
    fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    return select((int)s + 1, &rfds, NULL, NULL, &tv) > 0;
#endif
}

/* разбирает http://host[:port]/path */
static int parse_url(const char *url, char *host, size_t hostsz,
                     char *port, size_t portsz, char *path, size_t pathsz) {
    const char *p = url;
    strcpy(port, "80");
    if (!str_startswith_ci(p, "http://")) {
        return -1; /* только http */
    }
    p += 7;
    const char *slash = strchr(p, '/');
    const char *qmark = strchr(p, '?');
    const char *hostend = slash ? slash : p + strlen(p);
    if (qmark && (!slash || qmark < slash)) hostend = qmark;
    size_t hl = (size_t)(hostend - p);
    if (hl == 0 || hl >= hostsz) return -1;
    memcpy(host, p, hl); host[hl] = '\0';
    /* port? */
    char *colon = strrchr(host, ':');
    if (colon && colon != host) {
        strncpy(port, colon + 1, portsz - 1); port[portsz-1] = 0;
        *colon = '\0';
    }
    /* path */
    if (slash) {
        const char *pend = qmark && qmark > slash ? qmark : slash + strlen(slash);
        size_t pl = (size_t)(pend - slash);
        if (pl >= pathsz) pl = pathsz - 1;
        memcpy(path, slash, pl); path[pl] = '\0';
    } else {
        snprintf(path, pathsz, "/");
    }
    return 0;
}

static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, nl)) return 1;
    return 0;
}

/* общий connect по URL; возвращает сокет или SOCK_INVALID, ошибки — в out->error */
static sock_t http_connect(const char *url, int timeout_ms, HttpResponse *out,
                           char *host, size_t hostsz, char *port, size_t portsz,
                           char *path, size_t pathsz) {
    if (parse_url(url, host, hostsz, port, portsz, path, pathsz) != 0) {
        snprintf(out->error, sizeof(out->error), "bad URL (only http:// supported): %s", url);
        return SOCK_INVALID;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        snprintf(out->error, sizeof(out->error), "DNS failed for %s: %s", host, gai_strerror(gai));
        return SOCK_INVALID;
    }
    sock_t s = SOCK_INVALID;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == SOCK_INVALID) continue;
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket_(s); s = SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (s == SOCK_INVALID)
        snprintf(out->error, sizeof(out->error), "connect failed to %s:%s", host, port);
    return s;
}

static int send_request_line(sock_t s, const char *method, const char *url_path,
                             const char *host, const char *port,
                             const char *body, const char *content_type,
                             int keep_alive, int timeout_ms, HttpResponse *out) {
    Str req; str_init(&req);
    str_appendf(&req, "%s %s HTTP/1.1\r\n", method, url_path);
    str_appendf(&req, "Host: %s:%s\r\n", host, port);
    str_append(&req, "User-Agent: llm-host-c/1.0\r\n");
    str_append(&req, "Accept: */*\r\n");
    str_append(&req, keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");
    if (body) {
        if (content_type) str_appendf(&req, "Content-Type: %s\r\n", content_type);
        str_appendf(&req, "Content-Length: %zu\r\n", strlen(body));
    }
    str_append(&req, "\r\n");
    if (body) str_append(&req, body);
    size_t off = 0;
    while (off < req.len) {
        if (!wait_writable(s, timeout_ms)) { snprintf(out->error, sizeof(out->error), "send timeout"); str_free(&req); return -1; }
        ssize_t n = send(s, req.data + off, req.len - off, 0);
        if (n <= 0) { snprintf(out->error, sizeof(out->error), "send error"); str_free(&req); return -1; }
        off += (size_t)n;
    }
    str_free(&req);
    return 0;
}

int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 int timeout_ms, HttpResponse *out) {
    memset(out, 0, sizeof(*out));
    str_init(&out->body);
    str_init(&out->headers);
    out->status = -1;

    char host[256], port[16], path[2048];
    sock_t s = http_connect(url, timeout_ms, out, host, sizeof(host), port, sizeof(port), path, sizeof(path));
    if (s == SOCK_INVALID) return -1;

    if (send_request_line(s, method, path, host, port, body, content_type, 0, timeout_ms, out) != 0) {
        closesocket_(s); return -1;
    }

    /* читаем всё (Connection: close → до EOF). Ограничение 32 МБ. */
    enum { MAX_BODY = 32u*1024*1024 };
    char chunk[8192];
    Buf raw; buf_init(&raw);
    for (;;) {
        if (!wait_readable(s, timeout_ms)) {
            /* данные могли читаться частично — не фатально, но отмечаем */
            if (raw.len == 0) { snprintf(out->error, sizeof(out->error), "recv timeout"); buf_free(&raw); closesocket_(s); return -1; }
            break;
        }
        ssize_t n = recv(s, chunk, sizeof(chunk), 0);
        if (n < 0) { if (raw.len == 0) { snprintf(out->error, sizeof(out->error), "recv error"); buf_free(&raw); closesocket_(s); return -1; } break; }
        if (n == 0) break;
        buf_append(&raw, chunk, (size_t)n);
        if (raw.len > MAX_BODY) break;
    }
    closesocket_(s);

    /* парсим ответ */
    char *data = (char *)raw.data;
    data[raw.len < raw.cap ? raw.len : raw.cap - 1] = '\0';
    char *hdr_end = strstr(data, "\r\n\r\n");
    if (!hdr_end) { snprintf(out->error, sizeof(out->error), "malformed response (no headers)"); buf_free(&raw); return -1; }
    size_t hdr_len = (size_t)(hdr_end - data);
    str_appendn(&out->headers, data, hdr_len);
    if (sscanf(data, "HTTP/%*s %d", &out->status) != 1)
        out->status = -1;

    size_t blen = raw.len - hdr_len - 4;
    const char *bstart = data + hdr_len + 4;

    if (ci_contains(out->headers.data, "Transfer-Encoding: chunked") ||
        ci_contains(out->headers.data, "transfer-encoding: chunked")) {
        /* декодируем chunked */
        const char *p = bstart, *end = bstart + blen;
        while (p < end) {
            char *le;
            unsigned long csz = strtoul(p, &le, 16);
            if (le == p) break;
            p = strstr(le, "\r\n");
            if (!p) break;
            p += 2;
            if (csz == 0) break;
            if ((size_t)(end - p) < csz) csz = (size_t)(end - p);
            str_appendn(&out->body, p, csz);
            p += csz;
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
        }
    } else {
        str_appendn(&out->body, bstart, blen);
    }
    buf_free(&raw);
    return 0;
}

int http_request_stream(const char *method, const char *url,
                        const char *body, const char *content_type,
                        int idle_timeout_ms, HttpChunkCb chunk_cb, void *ctx,
                        HttpResponse *out) {
    memset(out, 0, sizeof(*out));
    str_init(&out->body);
    str_init(&out->headers);
    out->status = -1;

    char host[256], port[16], path[2048];
    sock_t s = http_connect(url, idle_timeout_ms, out, host, sizeof(host), port, sizeof(port), path, sizeof(path));
    if (s == SOCK_INVALID) return -1;
    if (send_request_line(s, method, path, host, port, body, content_type, 1, idle_timeout_ms, out) != 0) {
        closesocket_(s); return -1;
    }

    Buf acc; buf_init(&acc);          /* накопитель: заголовки + сырое тело */
    int headers_done = 0;
    size_t consumed = 0;              /* байтов аккумулятора уже обработано */
    int is_chunked = 0;
    char chunk[16384];
    int rc = 0;

    for (;;) {
        /* если заголовки ещё не получены — ждём \r\n\r\n */
        if (!headers_done) {
            char *he = acc.len ? strstr((char *)acc.data, "\r\n\r\n") : NULL;
            if (he) {
                size_t hl = (size_t)(he - (char *)acc.data);
                str_appendn(&out->headers, (char *)acc.data, hl);
                if (sscanf((char *)acc.data, "HTTP/%*s %d", &out->status) != 1) out->status = -1;
                is_chunked = ci_contains(out->headers.data, "chunked");
                headers_done = 1;
                consumed = hl + 4;
            }
        }
        /* отдаём готовое (полное) тело целиком, если оно уже в acc и не chunked */
        if (headers_done && !is_chunked) {
            /* сервер должен закрыть соединение (мы просили keep-alive только для заголовков,
             * но Ollama/LMStudio при stream шлют chunked; иначе читаем до EOF ниже) */
        }
        if (headers_done && is_chunked) {
            /* инкрементальное декодирование chunked из acc, начиная с consumed */
            for (;;) {
                size_t avail = acc.len - consumed;
                const char *p = (const char *)acc.data + consumed;
                if (avail < 3) break;                    /* минимум "X\r\n" */
                char *le;
                unsigned long csz = strtoul(p, &le, 16);
                if (le == p) { rc = -1; snprintf(out->error, sizeof(out->error), "bad chunk size"); goto done; }
                size_t hs = (size_t)(le - p);            /* длина hex + возможных расширений */
                const char *nl = NULL;
                for (size_t i = hs; i + 1 < avail; i++)
                    if (p[i] == '\r' && p[i+1] == '\n') { nl = p + i; break; }
                if (!nl) break;                          /* заголовок чанка ещё не цел */
                size_t hline = (size_t)(nl - p) + 2;
                if (csz == 0) { consumed = acc.len; goto drain_done; } /* терминальный чанк */
                if (avail < hline + csz + 2) break;      /* тело чанка ещё не цел */
                const char *payload = nl + 2;
                if (chunk_cb(payload, csz, ctx) != 0) goto drain_done;
                consumed += hline + csz;
                /* пропустить CRLF после чанка */
                if (consumed + 2 <= acc.len && acc.data[consumed] == '\r' && acc.data[consumed+1] == '\n') consumed += 2;
            }
            /* compact: сдвигаем необработанное в начало */
            if (consumed > 0) {
                memmove(acc.data, acc.data + consumed, acc.len - consumed);
                acc.len -= consumed; consumed = 0;
            }
        }
        if (!wait_readable(s, idle_timeout_ms)) {
            if (acc.len == 0 || !headers_done) { snprintf(out->error, sizeof(out->error), "recv timeout"); rc = -1; goto done; }
            /* EOF-стиль: сервер закрыл stream без chunked — считаем конец */
            goto drain_done;
        }
        ssize_t n = recv(s, chunk, sizeof(chunk), 0);
        if (n < 0) {
#if LHOST_WIN
            if (WSAGetLastError() == WSAETIMEDOUT) continue;
#else
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
            if (!headers_done) { snprintf(out->error, sizeof(out->error), "recv error"); rc = -1; goto done; }
            goto drain_done;
        }
        if (n == 0) goto drain_done;                     /* EOF */
        buf_append(&acc, chunk, (size_t)n);
        if (acc.len > 32u*1024*1024) { snprintf(out->error, sizeof(out->error), "stream too large"); rc = -1; goto done; }
    }

drain_done:
    if (headers_done && !is_chunked && acc.len > 0) {
        /* одноразовый ответ без chunked: отдать как один кусок */
        chunk_cb((const char *)acc.data + consumed, acc.len - consumed, ctx);
    }
done:
    buf_free(&acc);
    closesocket_(s);
    return rc;
}

void http_response_free(HttpResponse *r) {
    str_free(&r->body);
    str_free(&r->headers);
}
