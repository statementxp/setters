#ifndef MANIFEST_H
#define MANIFEST_H

#include <windows.h>

// Элемент дерева файлов
typedef struct {
    char path[512];          // Относительный путь от корня проекта
    char name[128];          // Отображаемое имя
    char description[256];   // Описание для пользователя
    char component[64];      // Идентификатор компонента
    size_t size_bytes;       // Размер в байтах
    BOOL required;           // Обязательный файл
    char url_b64[1024];      // Base64-кодированный URL
    char sha256[65];         // SHA256 хеш
    size_t index;            // Индекс в массиве
    BOOL needs_update;       // Требуется ли обновление
} DownloadItem;

// Фоновое изображение из манифеста
typedef struct {
    char url_b64[1024];      // Base64-кодированный URL изображения
    char sha256[65];         // Контрольная сумма для проверки
    int width;               // Ожидаемая ширина (для масштабирования)
    int height;              // Ожидаемая высота
    BOOL tiled;              // true = повторять как плитку, false = растягивать
} BackgroundImage;

// Схема интерфейса из манифеста
typedef struct {
    char title[128];         // Заголовок окна
    char header[256];        // Подзаголовок
    char project_version[32]; // Версия проекта
    char fallback_color[8];  // Цвет фона (#RRGGBB) как резервный вариант
    BackgroundImage background; // Фоновое изображение
} UISchema;

// Полный манифест
typedef struct {
    UISchema ui_schema;      // Данные для построения интерфейса
    DownloadItem *items;     // Массив файлов
    size_t items_count;      // Количество файлов
} Manifest;

// Состояние интерфейса и загрузки
typedef struct {
    RECT client_rect;        // Размеры клиентской области окна
    wchar_t status_text[256]; // Текст статуса
    BOOL *selected;          // Массив флагов выбора
    int *item_y;             // Y-координаты элементов
    size_t items_count;      // Количество элементов
    int progress;            // Прогресс загрузки (0-100)
    BOOL is_downloading;     // Идёт ли загрузка
    BOOL cancel_requested;   // Запрошена ли отмена
    HBITMAP background_bmp;  // Кэшированное фоновое изображение (для быстрой отрисовки)
} DownloadState;

// Функции работы с манифестом
BOOL load_manifest_secure(Manifest *manifest);
void free_manifest(Manifest *manifest);

#endif // MANIFEST_H