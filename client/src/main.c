#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include "../globals.h"
#include "manifest.h"
#include "downloader.h"
#include "ui.h"

// Инициализация глобальных переменных (единственное место определения)
HWND g_hwnd = NULL;
DownloadState g_state = {0};
Manifest g_manifest = {0};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            // GetClientRect(hwnd, &g_state.client_rect);
            return 0;
        
        case WM_SIZE:
            GetClientRect(hwnd, &g_state.client_rect);
            RefreshUI(g_hwnd);
            return 0;
        
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            render_ui(hdc, &g_manifest, &g_state);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            
            // Чекбоксы
            for (size_t i = 0; i < g_manifest.items_count; i++) {
                if (x >= 40 && x <= 60 && y >= g_state.item_y[i] && y <= g_state.item_y[i] + 20) {
                    if (!g_manifest.items[i].required) {
                        g_state.selected[i] = !g_state.selected[i];
                        RefreshUI(g_hwnd);
                    }
                    return 0;
                }
            }
            
            // Кнопки
            int btn_width = 180, btn_height = 40;
            int btn_exit_x = g_state.client_rect.right - btn_width - 40;
            int btn_exit_y = g_state.client_rect.bottom - btn_height - 30;
            int btn_download_x = btn_exit_x - btn_width - 20;
            int btn_download_y = btn_exit_y;
            
            if (x >= btn_exit_x && x <= btn_exit_x + btn_width &&
                y >= btn_exit_y && y <= btn_exit_y + btn_height) {
                PostMessage(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            
            if (!g_state.is_downloading &&
                x >= btn_download_x && x <= btn_download_x + btn_width &&
                y >= btn_download_y && y <= btn_download_y + btn_height) {
                
                g_state.is_downloading = TRUE;
                g_state.cancel_requested = FALSE;
                RefreshUI(g_hwnd);
                
                CreateThread(NULL, 0, download_thread_proc, NULL, 0, NULL);
                return 0;
            }
            return 0;
        }
        
        case WM_SETSTATUS:
            if (lParam) {
                wcsncpy(g_state.status_text, (wchar_t*)lParam, 255);
                g_state.status_text[255] = L'\0';
                RefreshUI(g_hwnd);
            }
            return 0;
        
        case WM_CLOSE:
            if (g_state.is_downloading) {
                if (MessageBoxW(hwnd, L"Загрузка в процессе. Отменить?", L"Подтверждение", 
                               MB_YESNO | MB_ICONQUESTION) == IDNO) {
                    return 0;
                }
                g_state.cancel_requested = TRUE;
            }
            DestroyWindow(hwnd);
            return 0;
        
        case WM_DESTROY:
            free_manifest(&g_manifest);
            free(g_state.selected);
            free(g_state.item_y);
            PostQuitMessage(0);
            return 0;
        
        default:
            return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Защита от отладки (опционально)
    // if (IsDebuggerPresent()) return 1;
    
    const wchar_t CLASS_NAME[] = L"SecureClientWindow";
    
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    if (!RegisterClassW(&wc)) return 1;
    
    g_hwnd = CreateWindowExW(
        0, CLASS_NAME, L"Secure Client", 
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInstance, NULL
    );
 
    if (!g_hwnd) return 1;

    // === КРИТИЧЕСКИ ВАЖНО: инициализируем размеры И МАССИВЫ ДО ПЕРВОЙ ОТРИСОВКИ ===
    GetClientRect(g_hwnd, &g_state.client_rect);
    g_state.items_count = 0;              // пока нет элементов
    g_state.selected = NULL;
    g_state.item_y = NULL;                // ← ИНИЦИАЛИЗИРУЕМ В NULL
    g_state.background_bmp = NULL;
    wcsncpy(g_state.status_text, L"Загрузка манифеста...", 255);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);  // ← первая отрисовка (без элементов, но без краша)
    
    // Загрузка манифеста (синхронно, но с отзывчивым интерфейсом благодаря PeekMessage в downloader.c)
    SetStatusText(L"Загрузка манифеста...");
    RefreshUI(g_hwnd);
    
    if (!load_manifest_secure(&g_manifest)) {
        MessageBoxW(g_hwnd, L"Не удалось загрузить манифест.\nПроверьте интернет-соединение.", 
                   L"Ошибка", MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }
    
    if (!load_manifest_secure(&g_manifest)) {
        MessageBoxW(g_hwnd, L"Не удалось загрузить манифест.\nПроверьте интернет-соединение.", 
                   L"Ошибка", MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }

// === ЗАГРУЗКА ФОНОВОГО ИЗОБРАЖЕНИЯ (если указано в манифесте) ===
if (g_manifest.ui_schema.background.url_b64[0] != '\0') {
    SetStatusText(L"Загрузка фона...");
    RefreshUI(g_hwnd);
    
    g_state.background_bmp = load_background_image(
        g_manifest.ui_schema.background.url_b64,
        g_manifest.ui_schema.background.sha256
    );
    
    if (!g_state.background_bmp) {
        SetStatusText(L"Цвет вместо фона");
    } else {
        SetStatusText(L"Готов к загрузке");
    }
    RefreshUI(g_hwnd);
} else {
    SetStatusText(L"Готов к загрузке");
    RefreshUI(g_hwnd);
}

    // === ИНИЦИАЛИЗИРУЕМ МАССИВЫ ТОЛЬКО ПОСЛЕ ЗАГРУЗКИ МАНИФЕСТА ===
    free(g_state.selected);   // освобождаем старые (если были)
    free(g_state.item_y);

    g_state.items_count = g_manifest.items_count;
    g_state.selected = (BOOL*)calloc(g_manifest.items_count, sizeof(BOOL));
    g_state.item_y = (int*)calloc(g_manifest.items_count, sizeof(int));
    
    for (size_t i = 0; i < g_manifest.items_count; i++) {
        if (g_manifest.items[i].required) {
            g_state.selected[i] = TRUE;
        }
    }
    wcsncpy(g_state.status_text, L"Готов к загрузке", 255);
    RefreshUI(g_hwnd);
    
    // Цикл сообщений
    MSG msg = {0};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    
    free(g_state.selected);
    free(g_state.item_y);
    return (int)msg.wParam;
}