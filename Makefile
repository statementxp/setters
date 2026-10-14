# llm-host (C) — сборка для Linux и кросс-сборка для Windows (mingw)
SRC := src/common.c src/json.c src/http.c src/files.c src/tts.c src/term.c src/llm.c src/gfx.c src/main.c
CFLAGS := -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Linux)
  LDFLAGS := -lpthread -lm
else
  LDFLAGS :=
endif

all: lhost

lhost: $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Кросс-сборка exe для Windows (нужен x86_64-w64-mingw32-gcc)
win: $(SRC)
	x86_64-w64-mingw32-gcc -std=c11 -O2 -o lhost.exe $^ -lws2_32 -lole32 -lm

clean:
	rm -f lhost lhost.exe *.o

.PHONY: all win clean
