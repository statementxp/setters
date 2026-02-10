#ifndef DOWNLOADER_H
#define DOWNLOADER_H

#include <windows.h>
#include "manifest.h"

char* download_to_memory(const char *url);
DWORD WINAPI download_thread_proc(LPVOID lpParam);
HBITMAP load_background_image(const char *url_b64, const char *expected_sha256); // ← ЭТА СТРОКА

#endif