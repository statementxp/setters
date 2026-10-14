/* gfx.c — встроенный HTTP + WebSocket сервер визуального интерфейса.
 * Один фоновый поток с poll(); события из потока REPL доставляются через
 * самопробрасываемый UDP-сокет (порт слушает тот же поток) — без мьютексов. */
#include "gfx.h"
#include "json.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>

#if LHOST_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define closesocket_ closesocket
#  include <process.h>
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <pthread.h>
   typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define closesocket_ close
#endif

#define MAX_CLIENTS 32
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#if LHOST_WIN
#  define POLLIN 1
#  define POLLHUP 2
#  define POLLERR 4
/* единый pollfd для обеих платформ */
struct pollfd_ws { sock_t fd; short events; short revents; };
typedef int socklen_t;
#  define MSG_DONTWAIT 0
   typedef SSIZE_T ssize_t;
#  ifndef INADDR_LOOPBACK
#    define INADDR_LOOPBACK 0x7F000001UL
#  endif
static int poll_win(struct pollfd_ws *pfds, int n, int ms) {
    fd_set rf, ef; FD_ZERO(&rf); FD_ZERO(&ef);
    sock_t maxv = 0;
    for (int i = 0; i < n; i++) {
        if (pfds[i].fd == SOCK_INVALID) continue;
        FD_SET(pfds[i].fd, &rf);
        if ((int)pfds[i].fd > (int)maxv) maxv = pfds[i].fd;
    }
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    int r = select(maxv + 1, &rf, NULL, &ef, &tv);
    if (r <= 0) return r;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        pfds[i].revents = 0;
        if (pfds[i].fd == SOCK_INVALID) continue;
        if (FD_ISSET(pfds[i].fd, &ef)) pfds[i].revents |= POLLHUP;
        if (FD_ISSET(pfds[i].fd, &rf)) { pfds[i].revents |= POLLIN; cnt++; }
    }
    return cnt ? 1 : 0;
}
#else
/* единый pollfd для обеих платформ */
struct pollfd_ws { sock_t fd; short events; short revents; };
#  define poll_win(p,n,ms) poll((void *)(p),(nfds_t)(n),(ms))
#endif

typedef struct {
    sock_t fd;
    int    is_ws;      /* прошёл handshake */
    int    alive;
    Buf    in;         /* накопитель входящих байтов */
} Client;

struct GfxServer {
    sock_t listen_fd;
    sock_t ev_fd;              /* UDP-сокет доставки событий в поток сервера */
    struct sockaddr_in ev_addr;
    int    port;
    char   url[64];
    volatile int running;
    Client clients[MAX_CLIENTS];
    atomic_long tokens_total;  /* инкрементируется из потока REPL */
    Str    backlog;            /* только поток сервера трогает */
#if LHOST_WIN
    HANDLE th;
#else
    pthread_t th;
#endif
};

/* ---------------- HTML-клиент ---------------- */
static const char *INDEX_HTML =
"<!doctype html><html lang='ru'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>LHost — нейросеть</title><style>"
":root{--bg:#0b0f1a;--panel:#121829;--line:#233052;--fg:#dfe7ff;--acc:#5ad1ff;--act:#ffb454;--err:#ff5c7a}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);"
"font:14px/1.4 'Segoe UI',system-ui,sans-serif}header{padding:10px 16px;"
"display:flex;align-items:center;gap:14px;border-bottom:1px solid var(--line)}"
"h1{font-size:16px;margin:0;color:var(--acc)}"
".dot{width:12px;height:12px;border-radius:50%;background:#444;transition:.2s}"
".dot.think{background:var(--acc);animation:pulse 1s infinite}"
".dot.action,.dot.speak{background:var(--act)}"
".dot.done{background:#6ee7a0}.dot.error{background:var(--err)}"
"@keyframes pulse{50%{transform:scale(1.6);opacity:.5}}"
"#status{flex:1;font-size:13px;opacity:.85;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
"#toks{font-variant-numeric:tabular-nums;color:var(--act)}"
"main{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px}"
"@media(max-width:900px){main{grid-template-columns:1fr}}"
"section{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:10px;min-height:260px}"
"h2{margin:0 0 8px;font-size:12px;text-transform:uppercase;letter-spacing:.08em;opacity:.6}"
"#wave{width:100%;height:120px;display:block}"
"#stream{white-space:pre-wrap;word-break:break-word;max-height:220px;overflow:auto;"
"background:#0a0e18;border-radius:6px;padding:8px;font-family:Consolas,monospace;font-size:13px}"
"#log{max-height:160px;overflow:auto;font-size:12px;font-family:Consolas,monospace}"
"#log div{padding:1px 0;border-bottom:1px dashed #1b2440}"
"canvas#art{width:100%;height:420px;background:#fff;border-radius:8px;display:block}"
"</style></head><body>"
"<header><div id='dot' class='dot'></div><h1>LHost · нейросеть</h1>"
"<div id='status'>ожидание подключения…</div><div id='toks'></div></header>"
"<main>"
"<section><h2>Активность модели</h2><canvas id='wave'></canvas>"
"<div id='stream'></div></section>"
"<section><h2>Журнал действий</h2><div id='log'></div>"
"<h2 style='margin-top:12px'>Холст модели</h2><canvas id='art' width='900' height='640'></canvas></section>"
"</main><script>"
"const $=id=>document.getElementById(id);"
"let ws,busy=false,tokens=0,lastText='',hist=new Array(300).fill(0),hp=0;"
"const wv=$('wave'),wc=wv.getContext('2d');wv.width=wv.clientWidth*devicePixelRatio;wv.height=120*devicePixelRatio;"
"const art=$('art'),ac=art.getContext('2d');"
"function drawWave(){const W=wv.width,H=wv.height;wc.clearRect(0,0,W,H);"
"wc.strokeStyle='#233052';wc.beginPath();wc.moveTo(0,H/2);wc.lineTo(W,H/2);wc.stroke();"
"wc.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue('--acc').trim();"
"wc.lineWidth=2*devicePixelRatio;wc.beginPath();"
"for(let i=0;i<hist.length;i++){const v=hist[(hp+i)%hist.length],x=i/(hist.length-1)*W,y=H/2-v*H*0.45;"
"i?wc.lineTo(x,y):wc.moveTo(x,y);}wc.stroke();requestAnimationFrame(drawWave);}"
"drawWave();"
"function pushWave(v){hist[hp]=v;hp=(hp+1)%hist.length;}"
"function setDot(cls,label){$('dot').className='dot '+cls;$('status').textContent=label;}"
"function logLine(t,c){const d=document.createElement('div');d.style.color=c||'#dfe7ff';"
"d.textContent=new Date().toLocaleTimeString()+'  '+t;$('log').prepend(d);}"
"function appendStream(t){const s=$('stream');s.textContent+=t;s.scrollTop=s.scrollHeight;}"
"function handle(o){switch(o.ev){"
"case 'state':busy=o.busy;setDot(o.kind,o.label);if(!o.busy&&lastText){appendStream('\\n');lastText='';}break;"
"case 'token':tokens++;$('toks').textContent='токенов: '+tokens;pushWave(Math.min(1,t/o.total));"
"if(o.delta){appendStream(o.delta);lastText=o.delta;}break;"
"case 'action':logLine(o.detail,'#ffb454');break;"
"case 'log':logLine(o.detail,'#dfe7ff');break;"
"case 'error':logLine(o.detail,'#ff5c7a');break;"
"case 'clear':ac.fillStyle=o.bg||'#ffffff';ac.fillRect(0,0,art.width,art.height);break;"
"case 'line':ac.strokeStyle=o.color;ac.lineWidth=o.w||2;ac.beginPath();ac.moveTo(o.x1,o.y1);ac.lineTo(o.x2,o.y2);ac.stroke();break;"
"case 'rect':ac.fillStyle=ac.strokeStyle=o.color;if(o.fill){ac.fillRect(o.x,o.y,o.w,o.h);}else{ac.lineWidth=o.w0||2;ac.strokeRect(o.x,o.y,o.w,o.h);}break;"
"case 'ellipse':ac.fillStyle=ac.strokeStyle=o.color;ac.beginPath();ac.ellipse(o.cx,o.cy,Math.abs(o.rx),Math.abs(o.ry),0,0,7);o.fill?ac.fill():ac.stroke();break;"
"case 'text':ac.fillStyle=o.color;ac.font=(o.size||16)+'px sans-serif';ac.fillText(o.text,o.x,o.y);break;}}"
"function connect(){ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');"
"ws.onmessage=e=>{try{handle(JSON.parse(e.data))}catch(_){}};"
"ws.onclose=()=>{setDot('error','WebSocket закрыт, переподключение…');setTimeout(connect,1500);};"
"ws.onopen=()=>{setDot('','соединено');$('log').prepend(Object.assign(document.createElement('div'),{textContent:'— connected —'}));}}"
"connect();</script></body></html>";

/* ---------------- low-level send ---------------- */
static int send_all(sock_t s, const void *data, size_t n) {
    const char *p = (const char *)data;
    while (n > 0) {
        ssize_t w = send(s, p, n, 0);
        if (w < 0) {
#if !LHOST_WIN
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        p += w; n -= (size_t)w;
    }
    return 0;
}

/* ---------------- base64 (для handshake) ---------------- */
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const unsigned char *in, size_t n, Str *out) {
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= in[i+1] << 8;
        if (rem > 2) v |= in[i+2];
        str_appendn(out, &(char){ B64[(v>>18)&63] }, 1);
        str_appendn(out, &(char){ B64[(v>>12)&63] }, 1);
        str_appendn(out, &(char){ rem > 1 ? B64[(v>>6)&63] : '=' }, 1);
        str_appendn(out, &(char){ rem > 2 ? B64[v&63] : '=' }, 1);
    }
}

/* ---------------- SHA-1 (минимальная реализация) ---------------- */
static void sha1(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    size_t total = len + 1;
    while (total % 64 != 56) total++;
    total += 8;
    unsigned char *buf = xmalloc(total);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    memset(buf + len + 1, 0, total - len - 9);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 1 - i] = (unsigned char)(bits >> (8*i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (buf[off+i*4]<<24)|(buf[off+i*4+1]<<16)|(buf[off+i*4+2]<<8)|buf[off+i*4+3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i-3]^w[i-8]^w[i-14]^w[i-16];
            w[i] = (x<<1)|(x>>31);
        }
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f,k;
            if      (i < 20) { f = (b&c)|((~b)&d); k = 0x5A827999; }
            else if (i < 40) { f = b^c^d;          k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b&c)|(b&d)|(c&d); k = 0x8F1BBCDC; }
            else             { f = b^c^d;          k = 0xCA62C1D6; }
            uint32_t t = ((a<<5)|(a>>27)) + f + e + k + w[i];
            e=d; d=c; c=(b<<30)|(b>>2); b=a; a=t;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    free(buf);
    uint32_t hs[5]={h0,h1,h2,h3,h4};
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[i*4+j] = (unsigned char)(hs[i] >> (24 - j*8));
}

/* ---------------- WebSocket frame ---------------- */
static int ws_send_text(Client *c, const char *json_text) {
    size_t n = strlen(json_text);
    unsigned char hdr[10];
    size_t hl = 0;
    hdr[hl++] = 0x81; /* FIN + text */
    if (n < 126) hdr[hl++] = (unsigned char)n;
    else if (n < 65536) { hdr[hl++] = 126; hdr[hl++] = n>>8; hdr[hl++] = n&0xff; }
    else { hdr[hl++] = 127;
           for (int i = 7; i >= 0; i--) hdr[hl++] = (unsigned char)(n >> (8*i)); }
    if (send_all(c->fd, hdr, hl) != 0) return -1;
    return send_all(c->fd, json_text, n);
}

/* экранирование для JSON-строк нашего протокола */
static void jesc(Str *s, const char *t) {
    json_escape(s, t);
}

/* broadcast всем ws-клиентам — вызывается ТОЛЬКО из потока сервера */
static void gfx_broadcast(GfxServer *g, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *c = &g->clients[i];
        if (c->alive && c->is_ws)
            if (ws_send_text(c, msg) != 0) {
                closesocket_(c->fd); c->fd = SOCK_INVALID; c->alive = 0; c->is_ws = 0;
                buf_free(&c->in);
            }
    }
    /* догар для будущих клиентов (ограничиваем размер) */
    if (g->backlog.len < 512u*1024) { str_append(&g->backlog, msg); str_append(&g->backlog, "\n"); }
}

/* доставка события из любого потока: UDP-петля на себя → обработается в poll() */
static void gfx_post(GfxServer *g, const char *msg) {
    if (!g || !g->running) return;
    sendto(g->ev_fd, msg, strlen(msg), 0,
           (struct sockaddr *)&g->ev_addr, sizeof(g->ev_addr));
}


/* ---------------- HTTP-ответы ---------------- */
static int http_ok_page(GfxServer *g, sock_t s) {
    Str resp; str_init(&resp);
    str_appendf(&resp,
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(INDEX_HTML));
    str_append(&resp, INDEX_HTML);
    int r = send_all(s, resp.data, resp.len);
    str_free(&resp);
    (void)g;
    return r;
}

static int http_simple(sock_t s, int code, const char *body) {
    Str resp; str_init(&resp);
    str_appendf(&resp, "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                       "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                code, code == 200 ? "OK" : "Not Found", strlen(body), body);
    int r = send_all(s, resp.data, resp.len);
    str_free(&resp);
    return r;
}

/* ищем заголовок (case-insensitive) в запросе, возвращаем значение в newly malloc */
static char *find_header(const char *req, const char *name) {
    size_t nl = strlen(name);
    const char *p = req;
    while ((p = strstr(p, name)) != NULL) {
        if (p == req || p[-1] == '\n') {
            p += nl;
            while (*p == ' ' || *p == ':') p++;
            const char *e = strpbrk(p, "\r\n");
            if (!e) e = p + strlen(p);
            char *v = xmalloc((size_t)(e - p) + 1);
            memcpy(v, p, (size_t)(e - p)); v[e - p] = 0;
            return v;
        }
        p += nl;
    }
    return NULL;
}

static void gfx_broadcast(GfxServer *g, const char *msg);

static void do_handshake(GfxServer *g, Client *c, const char *req) {
    char *key = find_header(req, "Sec-WebSocket-Key:");
    if (!key) { http_simple(c->fd, 400, "missing key"); goto kill; }
    Str tohash; str_init(&tohash);
    str_append(&tohash, key);
    str_append(&tohash, WS_GUID);
    unsigned char digest[20];
    sha1((const unsigned char *)tohash.data, tohash.len, digest);
    Str b64; str_init(&b64);
    b64_encode(digest, 20, &b64);
    Str resp; str_init(&resp);
    str_appendf(&resp,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", b64.data);
    if (send_all(c->fd, resp.data, resp.len) == 0) {
        c->is_ws = 1;
        /* приветственное состояние + счётчик токенов */
        Str hello; str_init(&hello);
        str_appendf(&hello, "{\"ev\":\"state\",\"kind\":\"idle\",\"label\":\"\\u0433\\u043e\\u0442\\u043e\\u0432\",\"busy\":false}");
        ws_send_text(c, hello.data);
        str_free(&hello);
        /* догар событий */
        { if (g->backlog.len) {
            char *bl = xstrdup(g->backlog.data);
            char *save = NULL;
            for (char *line = strtok_r(bl, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
                ws_send_text(c, line);
            free(bl);
        } }
    } else {
kill:
        free(key);
        c->alive = 0;
    }
    free(key);
    str_free(&tohash); str_free(&b64); str_free(&resp);
}

/* ---------------- публичные API ---------------- */
const char *gfx_url(GfxServer *g) { return g ? g->url : "(no ui)"; }

void gfx_tokens_total(GfxServer *g, long total) {
    if (g) atomic_fetch_add(&g->tokens_total, total);
}

static void emit_json(GfxServer *g, Str *s) {
    gfx_post(g, s->data);
    str_free(s);
}

void gfx_event(GfxServer *g, int kind, const char *detail) {
    static const char *knames[] = {"idle","think","think","action","speak","done","error"};
    static const char *labels[] = {"ожидание","думаю…","генерация","действие","озвучиваю","готово","ошибка"};
    const char *kn = knames[kind <= 6 ? kind : 0];
    const char *lb = labels[kind <= 6 ? kind : 0];
    Str s; str_init(&s);
    str_appendf(&s, "{\"ev\":\"state\",\"kind\":\"%s\",\"label\":\"", kn);
    jesc(&s, detail && *detail ? detail : lb);
    str_appendf(&s, "\",\"busy\":%s}", (kind == GFX_EVT_THINK || kind == GFX_EVT_ACTION || kind == GFX_EVT_SPEAK) ? "true" : "false");
    emit_json(g, &s);
    if (detail && *detail && kind != GFX_EVT_TOKEN) {
        Str l; str_init(&l);
        str_appendf(&l, "{\"ev\":\"%s\",\"detail\":\"", kind == GFX_EVT_ERROR ? "error" :
                        kind == GFX_EVT_ACTION ? "action" : "log");
        jesc(&l, detail);
        str_append(&l, "\"}");
        emit_json(g, &l);
    }
}

void gfx_line(GfxServer *g, double x1, double y1, double x2, double y2, const char *color, double width) {
    Str s; str_init(&s);
    str_appendf(&s, "{\"ev\":\"line\",\"x1\":%.2f,\"y1\":%.2f,\"x2\":%.2f,\"y2\":%.2f,\"w\":%.2f,\"color\":\"", x1, y1, x2, y2, width);
    jesc(&s, color ? color : "#222222"); str_append(&s, "\"}");
    emit_json(g, &s);
}

void gfx_rect(GfxServer *g, double x, double y, double w, double h, const char *color, int fill) {
    Str s; str_init(&s);
    str_appendf(&s, "{\"ev\":\"rect\",\"x\":%.2f,\"y\":%.2f,\"w\":%.2f,\"h\":%.2f,\"fill\":%s,\"color\":\"", x, y, w, h, fill ? "true" : "false");
    jesc(&s, color ? color : "#222222"); str_append(&s, "\"}");
    emit_json(g, &s);
}

void gfx_ellipse(GfxServer *g, double cx, double cy, double rx, double ry, const char *color, int fill) {
    Str s; str_init(&s);
    str_appendf(&s, "{\"ev\":\"ellipse\",\"cx\":%.2f,\"cy\":%.2f,\"rx\":%.2f,\"ry\":%.2f,\"fill\":%s,\"color\":\"", cx, cy, rx, ry, fill ? "true" : "false");
    jesc(&s, color ? color : "#222222"); str_append(&s, "\"}");
    emit_json(g, &s);
}

void gfx_text(GfxServer *g, double x, double y, const char *text, const char *color, double size) {
    Str s; str_init(&s);
    str_appendf(&s, "{\"ev\":\"text\",\"x\":%.2f,\"y\":%.2f,\"size\":%.1f,\"color\":\"", x, y, size > 0 ? size : 16);
    jesc(&s, color ? color : "#222222");
    str_append(&s, "\",\"text\":\"");
    jesc(&s, text ? text : "");
    str_append(&s, "\"}");
    emit_json(g, &s);
}

void gfx_clear(GfxServer *g, const char *bg) {
    Str s; str_init(&s);
    str_append(&s, "{\"ev\":\"clear\",\"bg\":\"");
    jesc(&s, bg ? bg : "#ffffff");
    str_append(&s, "\"}");
    emit_json(g, &s);
}

/* токен-событие: счётчик токенов инкрементируется атомарно здесь же */
void gfx_token(GfxServer *g, const char *delta, long total_hint) {
    if (!g || !g->running) return;
    long idx = atomic_fetch_add(&g->tokens_total, 1) + 1;
    Str s; str_init(&s);
    str_append(&s, "{\"ev\":\"token\",\"delta\":\"");
    jesc(&s, delta ? delta : "");
    str_appendf(&s, "\",\"i\":%ld,\"total\":%ld}", idx,
                total_hint > 0 ? total_hint : idx + 64);
    emit_json(g, &s);
}

/* ---------------- серверный цикл ---------------- */
static void serve_client(GfxServer *g, Client *c) {
    char tmp[4096];
    for (;;) {
        ssize_t n = recv(c->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
        if (n <= 0) return; /* нет данных или ошибка — выходим, poll продолжит позже */
        n = recv(c->fd, tmp, sizeof(tmp), 0);
        if (n <= 0) { c->alive = 0; return; }
        buf_append(&c->in, tmp, (size_t)n);
        if (!c->is_ws) {
            if (strstr((char *)c->in.data, "\r\n\r\n")) {
                char *req = xstrdup((char *)c->in.data);
                if (strstr(req, "GET /ws") && find_header(req, "Upgrade:"))
                    do_handshake(g, c, req);
                else if (strstr(req, "GET / ") || !strncmp(req, "GET / ", 6))
                    http_ok_page(g, c->fd), c->alive = 0;
                else if (strstr(req, "GET /health"))
                    http_simple(c->fd, 200, "ok"), c->alive = 0;
                else
                    http_ok_page(g, c->fd), c->alive = 0;
                free(req);
                return;
            }
            if (c->in.len > 65536) c->alive = 0;
        }
        /* в ws-сессию входящие кадры игнорируем (клиент только слушает) */
    }
}

static void server_loop(GfxServer *g);

#if LHOST_WIN
static unsigned __stdcall win_server_thread(void *arg) { server_loop((GfxServer *)arg); return 0; }
#else
static void *server_thread(void *arg) { server_loop((GfxServer *)arg); return NULL; }
#endif

static void server_loop(GfxServer *g) {
    struct pollfd_ws pfds[MAX_CLIENTS + 2];
    while (g->running) {
        int nf = 0;
        pfds[nf].fd = g->listen_fd; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++;
        pfds[nf].fd = g->ev_fd;     pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &g->clients[i];
            if (!c->alive) continue;
            pfds[nf].fd = c->fd; pfds[nf].events = POLLIN; pfds[nf].revents = 0; nf++;
        }
        int r = poll_win(pfds, nf, 200);
        if (r < 0) { if (errno == EINTR) continue; break; }
        /* UDP-события из потока REPL → broadcast */
        if (pfds[1].revents & POLLIN) {
            char ebuf[8192];
            for (;;) {
                ssize_t n = recvfrom(g->ev_fd, ebuf, sizeof(ebuf) - 1, MSG_DONTWAIT, NULL, NULL);
                if (n <= 0) break;
                ebuf[n] = 0;
                gfx_broadcast(g, ebuf);
            }
        }
        if (pfds[0].revents & POLLIN) {
            sock_t cs = accept(g->listen_fd, NULL, NULL);
            if (cs != SOCK_INVALID) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++) if (!g->clients[i].alive) { slot = i; break; }
                if (slot < 0) { closesocket_(cs); }
                else {
                    Client *c = &g->clients[slot];
                    memset(c, 0, sizeof(*c));
                    c->fd = cs; c->alive = 1; buf_init(&c->in);
                }
            }
        }
        int k = 2;
        for (int i = 0; i < MAX_CLIENTS && k < nf; i++) {
            Client *c = &g->clients[i];
            if (!c->alive) continue;
            if (pfds[k].revents & (POLLIN | POLLERR | POLLHUP)) {
                if (pfds[k].revents & (POLLERR | POLLHUP)) c->alive = 0;
                else serve_client(g, c);
            }
            if (!c->alive) {
                closesocket_(c->fd); c->fd = SOCK_INVALID; buf_free(&c->in);
            }
            k++;
        }
    }
}

int gfx_start(GfxServer **out, int port) {
    if (port <= 0) port = 8765;
    GfxServer *g = xmalloc(sizeof(GfxServer));
    memset(g, 0, sizeof(*g));
    g->port = port;
    snprintf(g->url, sizeof(g->url), "http://localhost:%d/", port);
    str_init(&g->backlog);
    for (int i = 0; i < MAX_CLIENTS; i++) { g->clients[i].fd = SOCK_INVALID; buf_init(&g->clients[i].in); }

    g->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g->listen_fd == SOCK_INVALID) { free(g); return -1; }
    int on = 1;
    setsockopt(g->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(g->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "gfx: bind :%d failed\n", port);
        closesocket_(g->listen_fd); free(g); return -1;
    }
    if (listen(g->listen_fd, 16) != 0) { closesocket_(g->listen_fd); free(g); return -1; }

    /* UDP-сокет событий: bind на порт+1, адрес для sendto из любого потока */
    g->ev_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)(port + 1));
    if (bind(g->ev_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        closesocket_(g->ev_fd); closesocket_(g->listen_fd); free(g); return -1;
    }
    memset(&g->ev_addr, 0, sizeof(g->ev_addr));
    g->ev_addr.sin_family = AF_INET;
    g->ev_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g->ev_addr.sin_port = htons((uint16_t)(port + 1));

    g->running = 1;
#if LHOST_WIN
    g->th = (HANDLE)_beginthreadex(NULL, 0, win_server_thread, g, 0, NULL);
    if (!g->th) { closesocket_(g->ev_fd); closesocket_(g->listen_fd); free(g); return -1; }
#else
    if (pthread_create(&g->th, NULL, server_thread, g) != 0) {
        closesocket_(g->ev_fd); closesocket_(g->listen_fd); free(g); return -1;
    }
#endif
    *out = g;
    return 0;
}

void gfx_stop(GfxServer *g) {
    if (!g) return;
    g->running = 0;
#if LHOST_WIN
    WaitForSingleObject(g->th, 2000);
    CloseHandle(g->th);
#else
    pthread_join(g->th, NULL);
#endif
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g->clients[i].alive) { closesocket_(g->clients[i].fd); buf_free(&g->clients[i].in); }
    closesocket_(g->ev_fd);
    closesocket_(g->listen_fd);
    str_free(&g->backlog);
    free(g);
}
