/* files.h — доступ к файлам в окружении хоста */
#ifndef LHOST_FILES_H
#define LHOST_FILES_H

#include "common.h"

typedef struct { char *path; int is_dir; long long size; } FileEntry;

/* Чтение/запись текста. Возвращают 0/-1. read: out — malloc'ный буфер. */
int host_file_read(const char *path, char **out, size_t *out_len);
int host_file_write(const char *path, const char *data, size_t len);
int host_file_append(const char *path, const char *data, size_t len);

/* Список файлов директории ("." по умолчанию). Возвращает count, массив через out. */
int host_list_dir(const char *dir, FileEntry **out);

const char *host_last_fs_error(void);

#endif
