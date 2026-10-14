#ifndef GLOBALS_H
#define GLOBALS_H

#include <windows.h>
#include "src/manifest.h"

// Единые глобальные переменные
extern HWND g_hwnd;
extern DownloadState g_state;
extern Manifest g_manifest;

// Пользовательское сообщение для статуса
#define WM_SETSTATUS (WM_USER + 100)

#endif