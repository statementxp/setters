/* tts.h — синтез речи и управление голосом */
#ifndef LHOST_TTS_H
#define LHOST_TTS_H

#include "common.h"

typedef struct {
    int  rate_wpm;      /* скорость, слов/мин (SAPI: -1000..1000 относительно) */
    int  volume;        /* 0..100 */
    int  voice_index;   /* индекс выбранного голоса */
    int  enabled;       /* выключен ли вывод в принципе */
} TtsSettings;

void tts_init(TtsSettings *st);
void tts_shutdown(void);

/* Синтезировать речь. Блокирующий вызов. 0 — успех. */
int tts_speak(TtsSettings *st, const char *text);

int  tts_set_rate(TtsSettings *st, int wpm);
int  tts_set_volume(TtsSettings *st, int vol0to100);
int  tts_list_voices(Str *out);   /* человекочитаемый список */
int  tts_select_voice(TtsSettings *st, int index);

#endif
