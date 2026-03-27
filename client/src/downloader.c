#include "../third_party/stb_image.h"
#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "downloader.h"
#include "../globals.h"
#include "../config.h"
#include "ui.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")

// Простое декодирование Base64 (статическая функция)
static void base64_decode_simple(const char *in, char *out, size_t out_len) {
    static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in);
    size_t i, j;
    
    for (i = 0, j = 0; i < len && j + 3 < out_len; i += 4) {
        const char *p1 = strchr(b64, in[i]);
        const char *p2 = (i+1 < len) ? strchr(b64, in[i+1]) : NULL;
        const char *p3 = (i+2 < len && in[i+2] != '=') ? strchr(b64, in[i+2]) : NULL;
        const char *p4 = (i+3 < len && in[i+3] != '=') ? strchr(b64, in[i+3]) : NULL;
        
        if (!p1 || !p2) break;
        int a = (int)(p1 - b64);
        int b = (int)(p2 - b64);
        int c = p3 ? (int)(p3 - b64) : 0;
        int d = p4 ? (int)(p4 - b64) : 0;
        
        out[j++] = (a << 2) | (b >> 4);
        if (!p3) break;
        out[j++] = (b << 4) | (c >> 2);
        if (!p4) break;
        out[j++] = (c << 6) | d;
    }
    if (j < out_len) out[j] = '\0';
}

// Скачивание данных в память с обработкой сообщений для отзывчивости UI
char* download_to_memory(const char *url) {
    HINTERNET hSession = InternetOpenA("SecureClient/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hSession) return NULL;
    
    HINTERNET hUrl = InternetOpenUrlA(hSession, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        InternetCloseHandle(hSession);
        return NULL;
    }
    
    char *buffer = NULL;
    DWORD total_read = 0;
    DWORD buffer_size = 8192;
    buffer = (char*)malloc(buffer_size);
    if (!buffer) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hSession);
        return NULL;
    }
    
    DWORD bytes_read;
    while (InternetReadFile(hUrl, buffer + total_read, buffer_size - total_read - 1, &bytes_read) && bytes_read > 0) {
        total_read += bytes_read;
        if (total_read + 1 >= buffer_size) {
            buffer_size *= 2;
            char *new_buffer = (char*)realloc(buffer, buffer_size);
            if (!new_buffer) {
                free(buffer);
                InternetCloseHandle(hUrl);
                InternetCloseHandle(hSession);
                return NULL;
            }
            buffer = new_buffer;
        }
        
        // Обработка сообщений для отзывчивости интерфейса во время загрузки
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    
    buffer[total_read] = '\0';
    
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hSession);
    return buffer;
}

// Вычисление SHA256 файла через BCrypt API
static BOOL compute_file_sha256(const char *path, char *out_sha256) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;
    
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    BYTE hash[32] = {0};
    
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != ERROR_SUCCESS) {
        CloseHandle(hFile);
        return FALSE;
    }
    
    if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) != ERROR_SUCCESS) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return FALSE;
    }
    
    BYTE buffer[8192];
    DWORD bytes_read;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytes_read, NULL) && bytes_read > 0) {
        BCryptHashData(hHash, buffer, bytes_read, 0);
    }
    
    BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    
    // Форматирование в hex
    for (int i = 0; i < 32; i++) {
        sprintf(out_sha256 + i*2, "%02x", hash[i]);
    }
    out_sha256[64] = '\0';
    return TRUE;
}

// Проверка необходимости обновления файла по хешу
BOOL file_needs_update(const char *base_dir, const char *rel_path, const char *expected_sha256) {
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s\\%s", base_dir, rel_path);
    
    // Файл отсутствует → нуждается в загрузке
    if (GetFileAttributesA(full_path) == INVALID_FILE_ATTRIBUTES) {
        return TRUE;
    }
    
    // Вычисляем хеш локального файла
    char actual_sha256[65] = {0};
    if (!compute_file_sha256(full_path, actual_sha256)) {
        return TRUE; // Ошибка хеширования → перезагрузить
    }
    
    // Сравниваем с ожидаемым хешем из манифеста
    return (strcmp(actual_sha256, expected_sha256) != 0);
}

// Рекурсивное создание папок для пути вида "assets/textures/character"
static BOOL ensure_directory_exists(const char *base_dir, const char *rel_path) {
    char full_path[MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s\\%s", base_dir, rel_path);
    
    // Нормализуем слеши (заменяем / на \ для Windows)
    for (char *p = full_path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    
    // Находим последний слеш (отделяем имя файла от пути)
    char *last_slash = strrchr(full_path, '\\');
    if (!last_slash) return TRUE; // нет слешей — папка не нужна
    *last_slash = '\0'; // обрезаем имя файла
    
    // Создаём все промежуточные папки по цепочке
    char *p = full_path;
    while (*p) {
        if (*p == '\\') {
            *p = '\0'; // временно обрезаем строку до текущей папки
            if (strlen(full_path) > 0) {
                // Пропускаем корневой диск (например, "C:")
                if (!(strlen(full_path) == 2 && full_path[1] == ':')) {
                    CreateDirectoryA(full_path, NULL);
                }
            }
            *p = '\\'; // восстанавливаем слеш
        }
        p++;
    }
    
    // Создаём последнюю папку в цепочке
    if (strlen(full_path) > 0) {
        CreateDirectoryA(full_path, NULL);
    }
    
    return TRUE;
}
// Внутренняя функция загрузки файла (без конфликта с заголовком)
static BOOL download_file_internal(const char *url_b64, const char *output_path) {
    char url[2048] = {0};
    base64_decode_simple(url_b64, url, sizeof(url));
    
    HANDLE hFile = CreateFileA(output_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    
    HINTERNET hSession = InternetOpenA("SecureClient/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hSession) {
        CloseHandle(hFile);
        return FALSE;
    }
    
    HINTERNET hUrl = InternetOpenUrlA(hSession, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hUrl) {
        CloseHandle(hFile);
        InternetCloseHandle(hSession);
        return FALSE;
    }
    
    BYTE buffer[8192];
    DWORD bytes_read, bytes_written;
    DWORD total_read = 0;
    
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytes_read) && bytes_read > 0) {
        WriteFile(hFile, buffer, bytes_read, &bytes_written, NULL);
        total_read += bytes_read;
        
        // Обновление статуса каждые ~1 МБ
        if (total_read % (1024*1024) == 0) {
            wchar_t status[256];
            swprintf(status, 256, L"Загружено: %lu MB", total_read / (1024*1024));
            SetStatusText(status);
        }
        
        // Проверка отмены
        if (g_state.cancel_requested) break;
        
        // Обработка сообщений для отзывчивости
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    
    CloseHandle(hFile);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hSession);
    
    if (g_state.cancel_requested) {
        DeleteFileA(output_path);
        return FALSE;
    }
    
    return TRUE;
}

// Поток загрузки файлов проекта с инкрементальной проверкой
DWORD WINAPI download_thread_proc(LPVOID lpParam) {
    char base_dir[MAX_PATH];
    snprintf(base_dir, sizeof(base_dir), "%s\\project", DOWNLOAD_DIR);
    CreateDirectoryA(base_dir, NULL);
    
    size_t total_files = 0;
    size_t updated_files = 0;
    
    for (size_t i = 0; i < g_manifest.items_count; i++) {
        DownloadItem *item = &g_manifest.items[i];
        
        // Пропускаем неотмеченные необязательные файлы
        if (!g_state.selected[i] && !item->required) continue;
        
        total_files++;
        
        // Проверяем, нуждается ли файл в обновлении
        if (!file_needs_update(base_dir, item->path, item->sha256)) {
            continue; // Файл актуален — пропускаем
        }
        
        item->needs_update = TRUE;
        updated_files++;
        
        // Создаём папки для файла
        if (!ensure_directory_exists(base_dir, item->path)) {
            wchar_t msg[256];
            MultiByteToWideChar(CP_UTF8, 0, item->path, -1, msg, 256);
            MessageBoxW(g_hwnd, msg, L"Ошибка создания папки", MB_ICONERROR);
            continue;
        }
        
        // Формируем полный путь
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s\\%s", base_dir, item->path);
        
        // Скачиваем файл
        if (!download_file_internal(item->url_b64, full_path)) {
            if (!g_state.cancel_requested) {
                wchar_t msg[256];
                MultiByteToWideChar(CP_UTF8, 0, item->path, -1, msg, 256);
                MessageBoxW(g_hwnd, msg, L"Ошибка загрузки", MB_ICONERROR);
            }
            break;
        }
        
        // Обновляем прогресс
        wchar_t status[256];
        swprintf(status, 256, L"Обновлено %zu/%zu файлов", updated_files, total_files);
        SetStatusText(status);
    }
    
    // Финальное сообщение
    if (!g_state.cancel_requested) {
        wchar_t status[256];
        if (updated_files == 0) {
            swprintf(status, 256, L"✓ Все файлы актуальны. Версия: %S", g_manifest.ui_schema.project_version);
        } else {
            swprintf(status, 256, L"✓ Обновлено %zu файлов. Версия: %S", updated_files, g_manifest.ui_schema.project_version);
        }
        SetStatusText(status);
    } else {
        SetStatusText(L"Обновление отменено");
        g_state.cancel_requested = FALSE;
    }
    
    g_state.is_downloading = FALSE;
    RefreshUI(g_hwnd);
    return 0;
}


// Загрузка картинки в фон
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"

HBITMAP load_background_image(const char *url_b64, const char *expected_sha256) {
    (void)expected_sha256;
    
    char url[2048] = {0};
    base64_decode_simple(url_b64, url, sizeof(url));
    
    char *image_data = download_to_memory(url);
    if (!image_data) return NULL;
    
    size_t data_size = 0;
    while (data_size < 10485760 && image_data[data_size] != 0) data_size++;
    if (data_size < 100) {
        free(image_data);
        return NULL;
    }
    
    int width, height, channels;
    unsigned char *pixels = stbi_load_from_memory((unsigned char*)image_data, (int)data_size, &width, &height, &channels, 4);
    free(image_data);
    
    if (!pixels) return NULL;
    
    HDC hdc = GetDC(NULL);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void *bits = NULL;
    HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    
    if (hBmp && bits) {
        for (int i = 0; i < width * height; i++) {
            unsigned char r = pixels[i*4 + 0];
            unsigned char g = pixels[i*4 + 1];
            unsigned char b = pixels[i*4 + 2];
            unsigned char a = pixels[i*4 + 3];
            ((unsigned char*)bits)[i*4 + 0] = b;
            ((unsigned char*)bits)[i*4 + 1] = g;
            ((unsigned char*)bits)[i*4 + 2] = r;
            ((unsigned char*)bits)[i*4 + 3] = a;
        }
    }
    
    stbi_image_free(pixels);
    return hBmp;
}