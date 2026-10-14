#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include "../globals.h"
#include "ui.h"

void SetStatusText(const wchar_t *text) {
    if (g_hwnd && text) {
        SendMessageW(g_hwnd, WM_SETSTATUS, 0, (LPARAM)text);
    }
}

void render_ui(HDC hdc, Manifest *manifest, DownloadState *state) {
    RECT rect = state->client_rect;
    
    // === Отрисовка фона ===
    if (state->background_bmp) {
        // Рисуем фоновое изображение
        HDC hdc_mem = CreateCompatibleDC(hdc);
        HBITMAP hOld = (HBITMAP)SelectObject(hdc_mem, state->background_bmp);
        
        BITMAP bmp = {0};
        GetObject(state->background_bmp, sizeof(bmp), &bmp);
        
        // Растягиваем на всё окно
        StretchBlt(hdc, 0, 0, rect.right, rect.bottom, 
                  hdc_mem, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY);
        
        SelectObject(hdc_mem, hOld);
        DeleteDC(hdc_mem);
        
        // Полупрозрачный затемняющий слой для читаемости текста
        HBRUSH overlay = CreateSolidBrush(RGB(20, 20, 35));
        FillRect(hdc, &rect, overlay);
        DeleteObject(overlay);
    } else {
        // Резервный цвет фона (#1a1a2e — тёмно-синий по умолчанию)
        COLORREF bg_color = RGB(26, 26, 46);  // ← ОБЪЯВЛЕНИЕ ПЕРЕМЕННОЙ
        
        // Парсим цвет из манифеста (#RRGGBB)
        if (manifest->ui_schema.fallback_color[0] == '#') {
            unsigned int r = 0, g = 0, b = 0;
            if (sscanf(manifest->ui_schema.fallback_color + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
                bg_color = RGB((BYTE)r, (BYTE)g, (BYTE)b);  // ← ИСПОЛЬЗОВАНИЕ ПЕРЕМЕННОЙ
            }
        }
        
        HBRUSH bg_brush = CreateSolidBrush(bg_color);  // ← ПЕРЕДАЧА В КИСТЬ
        FillRect(hdc, &rect, bg_brush);
        DeleteObject(bg_brush);
    }
    
    // === Остальной интерфейс (текст, элементы, кнопки) ===
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    
    // Заголовок окна
    HFONT hFont = CreateFontW(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(hdc, hFont);
    
    wchar_t wtitle[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, manifest->ui_schema.title, -1, wtitle, 256);
    TextOutW(hdc, 40, 20, wtitle, (int)wcslen(wtitle));
    DeleteObject(hFont);
    
    // Подзаголовок
    hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(hdc, hFont);
    
    wchar_t whdr[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, manifest->ui_schema.header, -1, whdr, 256);
    SetTextColor(hdc, RGB(180, 180, 255));
    TextOutW(hdc, 40, 65, whdr, (int)wcslen(whdr));
    SetTextColor(hdc, RGB(255, 255, 255));
    DeleteObject(hFont);
    
    // Версия проекта (справа вверху)
    if (manifest->ui_schema.project_version[0]) {
        wchar_t wver[64] = {0};
        MultiByteToWideChar(CP_UTF8, 0, manifest->ui_schema.project_version, -1, wver, 64);
        
        hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SelectObject(hdc, hFont);
        SetTextColor(hdc, RGB(100, 255, 100));
        
        SIZE ver_size;
        GetTextExtentPoint32W(hdc, wver, (int)wcslen(wver), &ver_size);
        TextOutW(hdc, rect.right - 40 - ver_size.cx, 30, wver, (int)wcslen(wver));
        
        SetTextColor(hdc, RGB(255, 255, 255));
        DeleteObject(hFont);
    }
    
    // Статусная строка
    hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    SelectObject(hdc, hFont);
    SetTextColor(hdc, RGB(100, 255, 100));
    TextOutW(hdc, 40, rect.bottom - 25, state->status_text, (int)wcslen(state->status_text));
    SetTextColor(hdc, RGB(255, 255, 255));
    DeleteObject(hFont);
    
    // Элементы списка
    int y = 120;
    if (state->item_y && manifest->items) {
        for (size_t i = 0; i < manifest->items_count && i < state->items_count; i++) {
            DownloadItem *item = &manifest->items[i];
            state->item_y[i] = y;
            
            // Чекбокс
            HPEN hPen = CreatePen(PS_SOLID, 2, RGB(94, 53, 177));
            HBRUSH hBrush = state->selected && (state->selected[i] || item->required) 
                          ? CreateSolidBrush(RGB(94, 53, 177)) 
                          : GetStockObject(NULL_BRUSH);
            
            SelectObject(hdc, hPen);
            SelectObject(hdc, hBrush);
            Rectangle(hdc, 40, y, 60, y + 20);
            
            if (state->selected && (state->selected[i] || item->required)) {
                MoveToEx(hdc, 44, y + 10, NULL);
                LineTo(hdc, 50, y + 14);
                LineTo(hdc, 56, y + 8);
            }
            
            DeleteObject(hPen);
            DeleteObject(hBrush);
        
        // Имя элемента (жирное для обязательных)
            wchar_t wname[256] = {0};
            MultiByteToWideChar(CP_UTF8, 0, item->name, -1, wname, 256);
            
            HFONT hFont = CreateFontW(18, 0, 0, 0, item->required ? FW_BOLD : FW_NORMAL, 
                                     FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(hdc, hFont);
            TextOutW(hdc, 70, y + 2, wname, (int)wcslen(wname));
            DeleteObject(hFont);
            
            // Размер (справа)
            wchar_t wsize[64] = {0};
            swprintf(wsize, 64, L"%.1f MB", item->size_bytes / (1024.0 * 1024.0));
            
            hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(hdc, hFont);
            SIZE size;
            GetTextExtentPoint32W(hdc, wsize, (int)wcslen(wsize), &size);
            SetTextColor(hdc, RGB(150, 150, 200));
            TextOutW(hdc, rect.right - 40 - size.cx, y + 4, wsize, (int)wcslen(wsize));
            SetTextColor(hdc, RGB(255, 255, 255));
            DeleteObject(hFont);
            
            // Описание (основное) — жёлто-серый цвет
            wchar_t wdesc[512] = {0};
            if (item->description[0]) {
                MultiByteToWideChar(CP_UTF8, 0, item->description, -1, wdesc, 512);
            } else {
            // Если описания нет — показываем путь как описание
                MultiByteToWideChar(CP_UTF8, 0, item->path, -1, wdesc, 512);
            }
            
            SetTextColor(hdc, RGB(200, 200, 220));
            hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SelectObject(hdc, hFont);
            TextOutW(hdc, 70, y + 26, wdesc, (int)wcslen(wdesc));
            SetTextColor(hdc, RGB(255, 255, 255));
            DeleteObject(hFont);
            
        // Путь к файлу (мелким шрифтом, серый) — только если есть отдельное описание
            if (item->description[0] && item->path[0]) {
                wchar_t wpath[512] = {0};
                MultiByteToWideChar(CP_UTF8, 0, item->path, -1, wpath, 512);
                
                SetTextColor(hdc, RGB(130, 130, 160));
                hFont = CreateFontW(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
                SelectObject(hdc, hFont);
                TextOutW(hdc, 70, y + 44, wpath, (int)wcslen(wpath));
                SetTextColor(hdc, RGB(255, 255, 255));
                DeleteObject(hFont);
                y += 75;  // больше места для пути
            } else {
                y += 65;
            }
        
            if (y > rect.bottom - 120) break;
        }
    } else {
        // Отображаем сообщение "Загрузка..." вместо пустого списка
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SelectObject(hdc, hFont);
        SetTextColor(hdc, RGB(200, 200, 255));
        TextOutW(hdc, 40, y, L"Загрузка списка файлов...", 24);
        DeleteObject(hFont);
    }
    
    // Кнопки
    int btn_width = 200, btn_height = 45;
    int btn_y = rect.bottom - btn_height - 40;
    int btn_exit_x = rect.right - btn_width - 30;
    int btn_download_x = btn_exit_x - btn_width - 20;
    
    HPEN hPenBtn = CreatePen(PS_SOLID, 2, RGB(100, 100, 120));
    HBRUSH hBrushBtn = CreateSolidBrush(RGB(60, 60, 80));
    SelectObject(hdc, hPenBtn);
    SelectObject(hdc, hBrushBtn);
    RoundRect(hdc, btn_exit_x, btn_y, btn_exit_x + btn_width, btn_y + btn_height, 8, 8);
    
    SetTextColor(hdc, RGB(220, 220, 240));
    hFont = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(hdc, hFont);
    TextOutW(hdc, btn_exit_x + 60, btn_y + 12, L"Выход", 5);
    DeleteObject(hFont);
    
    hBrushBtn = CreateSolidBrush(RGB(94, 53, 177));
    SelectObject(hdc, hBrushBtn);
    RoundRect(hdc, btn_download_x, btn_y, btn_download_x + btn_width, btn_y + btn_height, 8, 8);
    
    SetTextColor(hdc, RGB(255, 255, 255));
    wchar_t *btn_text = state->is_downloading ? L"Загрузка..." : L"Скачать выбранные";
    hFont = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SelectObject(hdc, hFont);
    TextOutW(hdc, btn_download_x + 25, btn_y + 12, btn_text, (int)wcslen(btn_text));
    DeleteObject(hFont);
    DeleteObject(hPenBtn);
    DeleteObject(hBrushBtn);
}

void RefreshUI(HWND hwnd) {
    if (hwnd) {
        InvalidateRect(hwnd, NULL, TRUE);
        UpdateWindow(hwnd);
    }
}