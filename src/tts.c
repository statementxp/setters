/* tts.c — Windows: SAPI (CoCreateInstance); Linux/espeak; macOS: say */
#include "tts.h"

#if LHOST_WIN
#  define COBJMACROS
#  include <windows.h>
#  include <sapi.h>   /* ISpVoice */

static ISpVoice *g_voice = NULL;
static BOOL g_com_ready = FALSE;

static int sapi_ensure(void) {
    if (g_voice) return 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) g_com_ready = TRUE;
    hr = CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL,
                          &IID_ISpVoice, (void **)&g_voice);
    if (FAILED(hr) || !g_voice) {
        LOG("SAPI unavailable, CoCreateInstance hr=0x%08lx", (unsigned long)hr);
        if (g_com_ready) CoUninitialize();
        g_com_ready = FALSE;
        return -1;
    }
    return 0;
}

static void utf8_to_wchar(const char *src, WCHAR *dst, size_t dstsz) {
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dstsz);
}

int tts_speak(TtsSettings *st, const char *text) {
    if (!st->enabled) return 0;
    if (sapi_ensure() != 0) return -1;
    LONG rate = st->rate_wpm - 180; /* SAPI: относительно 180 wpm */
    if (rate < -1000) rate = -1000;
    if (rate > 1000)  rate = 1000;
    ISpVoice_SetRate(g_voice, rate, SPVOICESTATIC);
    WORD vol = (WORD)(st->volume * 100); /* 0..10000 */
    ISpVoice_SetVolume(g_voice, vol, SPVOICESTATIC);
    WCHAR wbuf[4096];
    utf8_to_wchar(text, wbuf, 4096);
    HRESULT hr = ISpVoice_Speak(g_voice, wbuf, SPF_DEFAULT, NULL);
    return SUCCEEDED(hr) ? 0 : -1;
}

static int list_win_voices(Str *out) {
    if (sapi_ensure() != 0) return -1;
    ISpEnumList *e = NULL;
    if (FAILED(ISpVoice_GetVoices(g_voice, &e)) || !e) return -1;
    int i = 0;
    WCHAR *tok = NULL;
    while (ISpEnumList_Next(e, 1, (void **)&tok, NULL, NULL) == S_OK) {
        char abuf[1024];
        WideCharToMultiByte(CP_UTF8, 0, tok, -1, abuf, sizeof(abuf), NULL, NULL);
        str_appendf(out, "[%d] %s\n", i++, abuf);
        CoTaskMemFree(tok);
    }
    ISpEnumList_Release(e);
    return i;
}

int tts_list_voices(Str *out) { return list_win_voices(out); }

int tts_select_voice(TtsSettings *st, int index) {
    st->voice_index = index;
    ISpEnumList *e = NULL;
    if (!g_voice || FAILED(ISpVoice_GetVoices(g_voice, &e)) || !e) return -1;
    int i = 0; HRESULT hr = E_FAIL; WCHAR *tok = NULL;
    while (ISpEnumList_Next(e, 1, (void **)&tok, NULL, NULL) == S_OK) {
        if (i == index) { hr = ISpVoice_SetVoice(g_voice, tok, SPVOICESTATIC); CoTaskMemFree(tok); break; }
        CoTaskMemFree(tok); i++;
    }
    ISpEnumList_Release(e);
    return SUCCEEDED(hr) ? 0 : -1;
}

void tts_init(TtsSettings *st) {
    st->rate_wpm = 180; st->volume = 100; st->voice_index = -1; st->enabled = 1;
    if (sapi_ensure() != 0) LOG("TTS: SAPI init failed, speak will be no-op-ish");
}

void tts_shutdown(void) {
    if (g_voice) { ISpVoice_Release(g_voice); g_voice = NULL; }
    if (g_com_ready) { CoUninitialize(); g_com_ready = FALSE; }
}

int tts_set_rate(TtsSettings *st, int wpm) {
    if (wpm < 40 || wpm > 700) return -1;
    st->rate_wpm = wpm;
    return 0;
}

int tts_set_volume(TtsSettings *st, int v) {
    if (v < 0 || v > 100) return -1;
    st->volume = v;
    return 0;
}

#else /* ---------- POSIX ---------- */

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

static pid_t g_speak_pid = -1;

static const char *pick_engine(void) {
    /* приоритет: espeak-ng → espeak → festival → pico2wave */
    static const char *cands[] = { "espeak-ng", "espeak", "festival", "picospeaker", NULL };
    for (int i = 0; cands[i]; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", cands[i]);
        if (system(cmd) == 0) return cands[i];
    }
    return NULL;
}

int tts_speak(TtsSettings *st, const char *text) {
    if (!st->enabled) return 0;
    const char *eng = pick_engine();
    if (!eng) { LOG("TTS: no engine found (install espeak-ng)"); return -1; }
    Str cmd; str_init(&cmd);
    /* экранируем текст в одинарных кавычках для shell */
    str_appendf(&cmd, "%s -s %d -a %d ", eng, st->rate_wpm, st->volume * 2);
    if (strcmp(eng, "festival") == 0)
        str_appendf(&cmd, "--batch-mode --language=speech '(SayText \"%s\")' 2>/dev/null", text);
    else {
        str_append(&cmd, "'");
        for (const char *p = text; *p; p++) {
            if (*p == '\'') str_append(&cmd, "'\\''");
            else str_appendn(&cmd, p, 1);
        }
        str_append(&cmd, "' >/dev/null 2>&1");
    }
    int rc = system(cmd.data);
    str_free(&cmd);
    return rc == 0 ? 0 : -1;
}

int tts_list_voices(Str *out) {
    const char *eng = pick_engine();
    if (!eng) { str_append(out, "(no TTS engine installed; try: apt install espeak-ng)\n"); return 0; }
    Str cmd; str_init(&cmd);
    str_appendf(&cmd, "%s --voices 2>/dev/null | head -40", eng);
    FILE *f = popen(cmd.data, "r");
    str_free(&cmd);
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) str_append(out, line);
    pclose(f);
    return 1;
}

int tts_select_voice(TtsSettings *st, int index) { st->voice_index = index; return 0; }

void tts_init(TtsSettings *st) {
    st->rate_wpm = 175; st->volume = 80; st->voice_index = -1; st->enabled = 1;
    if (!pick_engine())
        LOG("TTS: engine not found. On Debian/Ubuntu: sudo apt install espeak-ng");
}

void tts_shutdown(void) {
    if (g_speak_pid > 0) { kill(g_speak_pid, SIGTERM); waitpid(g_speak_pid, NULL, 0); g_speak_pid = -1; }
}

int tts_set_rate(TtsSettings *st, int wpm) { if (wpm < 40 || wpm > 700) return -1; st->rate_wpm = wpm; return 0; }
int tts_set_volume(TtsSettings *st, int v) { if (v < 0 || v > 100) return -1; st->volume = v; return 0; }

#endif
