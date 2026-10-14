/* llm.h — клиент локальных LLM-серверов: Ollama, LM Studio / KoboldCpp (OpenAI-compatible) */
#ifndef LHOST_LLM_H
#define LHOST_LLM_H

#include "common.h"
#include "json.h"
#include "gfx.h"

typedef enum { BACKEND_OLLAMA = 0, BACKEND_OPENAI_COMPAT = 1 } LlmBackend;

typedef struct {
    LlmBackend backend;
    char base_url[256];   /* http://localhost:11434 или http://localhost:1234/v1 */
    char model[128];
    char system_prompt[4096];
    double temperature;
    int  max_tokens;
    int  timeout_ms;
} LlmConfig;

void llm_config_default(LlmConfig *c);

/* Проверка доступности сервера + список моделей (через /api/tags или /v1/models). */
int llm_check(LlmConfig *c, Str *out_models);

/* Запрос к модели. Возвращает текст ответа в newly-allocated строку (*reply). */
int llm_chat(LlmConfig *c, const char *user_prompt, char **reply, char **error);

/* Потоковый запрос: на каждый фрагмент генерации вызывается on_delta
 * (text — кусок текста, n — длина, user_ctx — контекст колбэка).
 * Полный ответ возвращается в *reply. Если gfx != NULL, дельты и состояние
 * («думаю/генерация») транслируются в визуальный интерфейс.
 * Колбэк может вернуть -1 для досрочной остановки генерации. */
typedef int (*LlmDeltaCb)(const char *text, size_t n, void *user_ctx);
int llm_chat_stream(LlmConfig *c, GfxServer *gfx, const char *user_prompt,
                    LlmDeltaCb on_delta, void *user_ctx,
                    char **reply, char **error);

/* Парсинг «ответа → действия». Ищем в тексте блоки:
 *   ```action
 *   {"type":"terminal_exec","cmd":"ipconfig"}
 *   ```
 * либо строки ACTION: <json>. Выполнение делегируется callback.
 * Возвращает число выполненных действий. */
typedef int (*ActionRunner)(const JVal *action, void *ctx);
int llm_apply_actions(const char *reply_text, ActionRunner runner, void *ctx, Str *log_out);

#endif
