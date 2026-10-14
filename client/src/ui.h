#ifndef UI_H
#define UI_H

#include <windows.h>
#include "manifest.h"

void SetStatusText(const wchar_t *text);
void render_ui(HDC hdc, Manifest *manifest, DownloadState *state);
void RefreshUI(HWND hwnd);

#endif