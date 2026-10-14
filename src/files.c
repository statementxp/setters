/* files.c — POSIX + Windows реализация */
#include "files.h"

#if LHOST_WIN
#  include <direct.h>
#  include <io.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
#include <errno.h>
#endif

static char g_err[256] = "";

const char *host_last_fs_error(void) { return g_err; }

static void set_err(const char *op, const char *path) {
    snprintf(g_err, sizeof(g_err), "%s '%s' failed: %s", op, path, strerror(errno));
}

int host_file_read(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { set_err("open", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); set_err("ftell", path); return -1; }
    /* ограничение 64 МБ на чтение через ассистента */
    if ((size_t)sz > 64u*1024*1024) { fclose(f); snprintf(g_err, sizeof(g_err), "file too large: %s", path); return -1; }
    char *buf = xmalloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    *out = buf;
    if (out_len) *out_len = rd;
    return 0;
}

int host_file_write(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { set_err("create", path); return -1; }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) { set_err("write", path); return -1; }
    return 0;
}

int host_file_append(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "ab");
    if (!f) { set_err("append-open", path); return -1; }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) { set_err("append", path); return -1; }
    return 0;
}

int host_list_dir(const char *dir, FileEntry **out) {
    if (!dir || !*dir) dir = ".";
    *out = NULL;
    int count = 0, cap = 64;
    FileEntry *arr = xmalloc(sizeof(FileEntry) * cap);

#if LHOST_WIN
    char pattern[1024];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    struct _finddatai64_t fd;
    intptr_t h = _findfirsti64(pattern, &fd);
    if (h == -1L) { set_err("findfirst", dir); free(arr); return -1; }
    do {
        if (!strcmp(fd.name, ".") || !strcmp(fd.name, "..")) continue;
        if (count == cap) { cap *= 2; arr = xrealloc(arr, sizeof(FileEntry)*cap); }
        Str p; str_init(&p);
        str_appendf(&p, "%s\\%s", dir, fd.name);
        arr[count].path = str_take(&p);
        arr[count].is_dir = (fd.attrib & _A_SUBDIR) != 0;
        arr[count].size = fd.size;
        count++;
    } while (_findnexti64(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { set_err("opendir", dir); free(arr); return -1; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (count == cap) { cap *= 2; arr = xrealloc(arr, sizeof(FileEntry)*cap); }
        Str p; str_init(&p);
        str_appendf(&p, "%s/%s", dir, de->d_name);
        arr[count].path = str_take(&p);
        struct stat st;
        if (stat(arr[count].path, &st) == 0) {
            arr[count].is_dir = S_ISDIR(st.st_mode);
            arr[count].size = (long long)st.st_size;
        } else { arr[count].is_dir = 0; arr[count].size = -1; }
        count++;
    }
    closedir(d);
#endif
    *out = arr;
    return count;
}
