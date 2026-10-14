# llm-host (C)

Приложение-хост для LLM-ассистента на чистом C11 (без внешних библиотек).

## Возможности
1. **Доступ к интернету** — собственный HTTP/1.1 клиент поверх BSD-сокетов (`src/http.c`): GET/POST, chunked-encoding, таймауты. Команды `get <url>`, `post <url> <json>`.
2. **Доступ к файлам** — чтение/запись/добавление/листинг (`src/files.c`). Команды `ls`, `cat`, `write`, `append`; действия LLM `file_read`, `file_write`.
3. **TTS и управление голосом** (`src/tts.c`):
   - Windows: SAPI через COM (`ISpVoice`) — голоса, скорость, громкость;
   - Linux: espeak-ng/espeak/festival;
   - Команды `speak`, `rate`, `vol`, `voices`, `voice <i>`, `tts on|off`, `autospeak`.
4. **Терминал Windows** (`src/term.c`): интерактивная persistent-сессия PowerShell/cmd через CreatePipe+CreateProcess с маркерами конца вывода `###LHOST_END_n###`, очистка ANSI; одноразовый запуск, спавн нового окна консоли (`start ... cmd.exe /K`), tasklist/taskkill, env get/set. На Linux тот же API работает в эмуляции через `/bin/sh` — код переносим и тестируем на любой платформе.
5. **Локальные LLM** (`src/llm.c`): клиенты Ollama (`/api/generate`, `/api/tags`) и OpenAI-compatible (LM Studio/KoboldCpp: `/v1/chat/completions`, `/v1/models`). Запрос → ответ как текст + автопарсинг действий из блоков ```action {json}``` и строк `ACTION: {json}` → выполнение через хост (терминал/файлы/голос/http). Системный промпт обучает модель формату действий.

## Сборка
```sh
make          # Linux/macOS → ./lhost
make win      # кросс-компиляция в lhost.exe (x86_64-w64-mingw32-gcc)
```
На Windows нативно: `gcc -std=c11 src/*.c -o lhost.exe -lws2_32 -lole32` (MinGW-w64 или MSVC-аналог).

## Запуск
```sh
./lhost                       # REPL; help — список команд
./lhost "ask какой IP у машины"   # пакетный режим
```
По умолчанию подключено к Ollama `http://localhost:11434`, модель `llama3.2`. Смена: `connect openai http://localhost:1234/v1 <model>`. Конфиг сохраняется командой `save` в `lhost.cfg`.

## Безопасность
`autoact on|off` — тумблер автоисполнения действий из ответов модели (по умолчанию включён; для продакшена рекомендуется выключить и подтверждать вручную).
