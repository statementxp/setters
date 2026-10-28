#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../third_party/cJSON.h"
#include "manifest.h"
#include "downloader.h"
#include "../config.h"

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

BOOL load_manifest_secure(Manifest *manifest) {
    char manifest_url[2048] = {0};
    base64_decode_simple(MANIFEST_URL_BASE64, manifest_url, sizeof(manifest_url));
    
    char *json_data = download_to_memory(manifest_url);
    if (!json_data) {
        // Демо-режим при ошибке загрузки
        manifest->items_count = 3;
        manifest->items = (DownloadItem*)calloc(manifest->items_count, sizeof(DownloadItem));
        
        strncpy(manifest->ui_schema.title, "Тестовый загрузчик", 127);
        strncpy(manifest->ui_schema.header, "Демо-режим", 255);
        strncpy(manifest->ui_schema.project_version, "1.0", 31);
        strncpy(manifest->ui_schema.fallback_color, "#1a1a2e", 7);
        
        // Элемент 1
        strncpy(manifest->items[0].path, "engine/core.dll", 511);
        strncpy(manifest->items[0].name, "Ядро системы", 127);
        strncpy(manifest->items[0].description, "Обязательный компонент", 255);
        manifest->items[0].size_bytes = 45678901;
        manifest->items[0].required = TRUE;
        strncpy(manifest->items[0].url_b64, "aHR0cHM6Ly9leGFtcGxlLmNvbS9jb3JlLnppcA==", 1023);
        strncpy(manifest->items[0].sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 64);
        strncpy(manifest->items[0].component, "core", 63);
        manifest->items[0].index = 0;
        manifest->items[0].needs_update = TRUE;
        
        // Элемент 2
        strncpy(manifest->items[1].path, "assets/textures/base.png", 511);
        strncpy(manifest->items[1].name, "Базовые ассеты", 127);
        strncpy(manifest->items[1].description, "Модели и текстуры", 255);
        manifest->items[1].size_bytes = 215000000;
        manifest->items[1].required = TRUE;
        strncpy(manifest->items[1].url_b64, "aHR0cHM6Ly9leGFtcGxlLmNvbS9hc3NldHMuemlw", 1023);
        strncpy(manifest->items[1].sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 64);
        strncpy(manifest->items[1].component, "base_assets", 63);
        manifest->items[1].index = 1;
        manifest->items[1].needs_update = TRUE;
        
        // Элемент 3
        strncpy(manifest->items[2].path, "assets/textures/hd.png", 511);
        strncpy(manifest->items[2].name, "Текстуры 4K", 127);
        strncpy(manifest->items[2].description, "Дополнительные текстуры высокого разрешения", 255);
        manifest->items[2].size_bytes = 1200000000;
        manifest->items[2].required = FALSE;
        strncpy(manifest->items[2].url_b64, "aHR0cHM6Ly9leGFtcGxlLmNvbS9oZC56aXA=", 1023);
        strncpy(manifest->items[2].sha256, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 64);
        strncpy(manifest->items[2].component, "hd_textures", 63);
        manifest->items[2].index = 2;
        manifest->items[2].needs_update = TRUE;
        
        return TRUE;
    }
    
    // Парсим JSON
    cJSON *root = cJSON_Parse(json_data);
    if (!root) {
        free(json_data);
        return FALSE;
    }
    
    // UI
    cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (ui) {
        cJSON *window = cJSON_GetObjectItemCaseSensitive(ui, "window");
        if (window) {
            cJSON *title = cJSON_GetObjectItemCaseSensitive(window, "title");
            if (title && cJSON_IsString(title)) {
                strncpy(manifest->ui_schema.title, title->valuestring, sizeof(manifest->ui_schema.title) - 1);
            }
        }
        
        cJSON *header = cJSON_GetObjectItemCaseSensitive(ui, "header");
        if (header) {
            cJSON *hdr_title = cJSON_GetObjectItemCaseSensitive(header, "title");
            if (hdr_title && cJSON_IsString(hdr_title)) {
                strncpy(manifest->ui_schema.header, hdr_title->valuestring, sizeof(manifest->ui_schema.header) - 1);
            }
        }
        
        // Резервный цвет фона
        cJSON *fallback = cJSON_GetObjectItemCaseSensitive(ui, "fallback_color");
        if (fallback && cJSON_IsString(fallback)) {
            strncpy(manifest->ui_schema.fallback_color, fallback->valuestring, sizeof(manifest->ui_schema.fallback_color) - 1);
        } else {
            strncpy(manifest->ui_schema.fallback_color, "#1a1a2e", 7);
        }
        
        // Версия проекта
        cJSON *proj_ver = cJSON_GetObjectItemCaseSensitive(ui, "project_version");
        if (proj_ver && cJSON_IsString(proj_ver)) {
            strncpy(manifest->ui_schema.project_version, proj_ver->valuestring, sizeof(manifest->ui_schema.project_version) - 1);
        } else {
            strncpy(manifest->ui_schema.project_version, "1.0", 31);
        }
    }
    
    // Items (совместимость со старым форматом)
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!items) {
        // Новый формат: поле "tree"
        items = cJSON_GetObjectItemCaseSensitive(root, "tree");
    }
    
    if (items && cJSON_IsArray(items)) {
        manifest->items_count = cJSON_GetArraySize(items);
        manifest->items = (DownloadItem*)calloc(manifest->items_count, sizeof(DownloadItem));
        
        for (size_t i = 0; i < manifest->items_count; i++) {
            cJSON *item = cJSON_GetArrayItem(items, i);
            DownloadItem *di = &manifest->items[i];
            di->index = i;
            di->needs_update = TRUE; // по умолчанию требует обновления
            
            // Вспомогательная функция для извлечения строк
            #define SAFE_GET_STR(field, dst, max_len) \
                do { \
                    cJSON *f = cJSON_GetObjectItemCaseSensitive(item, field); \
                    if (f && cJSON_IsString(f) && f->valuestring) { \
                        strncpy(dst, f->valuestring, max_len - 1); \
                        (dst)[max_len - 1] = '\0'; \
                    } \
                } while(0)
            
            SAFE_GET_STR("path", di->path, sizeof(di->path));
            SAFE_GET_STR("name", di->name, sizeof(di->name));
            SAFE_GET_STR("description", di->description, sizeof(di->description));
            SAFE_GET_STR("component", di->component, sizeof(di->component));
            SAFE_GET_STR("url_b64", di->url_b64, sizeof(di->url_b64));
            SAFE_GET_STR("sha256", di->sha256, sizeof(di->sha256));
            
            #undef SAFE_GET_STR
            
            // Если имя не задано — извлекаем из пути
            if (di->name[0] == '\0' && di->path[0] != '\0') {
                const char *last_slash = strrchr(di->path, '/');
                if (!last_slash) last_slash = strrchr(di->path, '\\');
                if (last_slash) {
                    strncpy(di->name, last_slash + 1, sizeof(di->name) - 1);
                } else {
                    strncpy(di->name, di->path, sizeof(di->name) - 1);
                }
            }
            
            // Размер и обязательность
            cJSON *f;
            if ((f = cJSON_GetObjectItemCaseSensitive(item, "size_bytes")) && cJSON_IsNumber(f))
                di->size_bytes = (size_t)f->valueint;
            
            if ((f = cJSON_GetObjectItemCaseSensitive(item, "required")) && cJSON_IsBool(f))
                di->required = cJSON_IsTrue(f);
        }
    }
    
    cJSON_Delete(root);
    free(json_data);
    
    if (manifest->items_count == 0) {
        free_manifest(manifest);
        return FALSE;
    }
    
    return TRUE;
}

void free_manifest(Manifest *manifest) {
    if (manifest->items) {
        free(manifest->items);
        manifest->items = NULL;
        manifest->items_count = 0;
    }
}