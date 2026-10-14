/* gfx.h — графическое отображение активности нейросети + «холст», на котором
 * модель может рисовать изображения напрямую (примитивы: линии, прямоугольники,
 * эллипсы, текст). Реализация: встраиваемый HTTP-сервер + WebSocket (RFC 6455),
 * клиентская страница рендерит всё на HTML5 Canvas. Без внешних зависимостей. */
#ifndef LHOST_GFX_H
#define LHOST_GFX_H

#include "common.h"

typedef struct GfxServer GfxServer;

/* Запуск фонового сервера интерфейса. port=0 → 8765. 0 — успех. */
int  gfx_start(GfxServer **out, int port);
void gfx_stop(GfxServer *g);
const char *gfx_url(GfxServer *g); /* http://localhost:<port>/ */

/* --- события активности нейросети (дизпетчер рисует «живой» график) --- */
enum {
    GFX_EVT_IDLE   = 0,
    GFX_EVT_THINK  = 1,   /* идёт генерация токенов */
    GFX_EVT_TOKEN  = 2,   /* получен токен (delta — фрагмент текста) */
    GFX_EVT_ACTION = 3,   /* выполняется действие из ответа модели */
    GFX_EVT_SPEAK  = 4,   /* идёт озвучивание */
    GFX_EVT_DONE   = 5,   /* цикл завершён */
    GFX_EVT_ERROR  = 6
};

void gfx_event(GfxServer *g, int kind, const char *detail);
void gfx_tokens_total(GfxServer *g, long delta); /* += дельта к счётчику токенов за сессию */

/* Событие получения токена из потока генерации (delta — фрагмент текста).
 * Индекс tok_idx присваивается внутренним атомарным счётчиком. */
void gfx_token(GfxServer *g, const char *delta, long total_hint);

/* --- холст: команды рисования от модели/хоста --- */
/* color: "#rrggbb", "#rgb" или "name". x2,y2 для line; w,h для rect/ellipse. */
void gfx_line   (GfxServer *g, double x1, double y1, double x2, double y2, const char *color, double width);
void gfx_rect   (GfxServer *g, double x, double y, double w, double h, const char *color, int fill);
void gfx_ellipse(GfxServer *g, double cx, double cy, double rx, double ry, const char *color, int fill);
void gfx_text   (GfxServer *g, double x, double y, const char *text, const char *color, double size);
void gfx_clear  (GfxServer *g, const char *bg);

#endif
