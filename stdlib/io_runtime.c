#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// stderr (need global FILE* stderr)
void sammine_io_eprint(const char* s)   { fputs(s, stderr); }
void sammine_io_eprintln(const char* s) { fprintf(stderr, "%s\n", s); }

// stdin — returns malloc'd buffer (caller owns), strips trailing \n
char* sammine_io_read_line(int64_t* out_len) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) { *out_len = 0; free(line); return NULL; }
    if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
    *out_len = (int64_t)len;
    return line;
}

// File handle operations (FILE* passed as opaque void*)
void* sammine_io_open(const char* path, const char* mode) {
    return (void*)fopen(path, mode);
}

void sammine_io_close(void* handle) {
    if (handle) fclose((FILE*)handle);
}

// Thread handle through: consume and return the same handle
void* sammine_io_file_read_line(void* handle, char** out_buf, int64_t* out_len) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, (FILE*)handle);
    if (len < 0) {
        *out_len = 0;
        *out_buf = NULL;
        free(line);
    } else {
        if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
        *out_len = (int64_t)len;
        *out_buf = line;
    }
    return handle;
}

void* sammine_io_file_write(void* handle, const char* content, int64_t len) {
    fwrite(content, 1, (size_t)len, (FILE*)handle);
    return handle;
}

// Convenience (open + operate + close in one call)
char* sammine_io_read_file(const char* path, int64_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) { *out_len = -1; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); *out_len = -1; return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    *out_len = (int64_t)rd;
    return buf;
}

int sammine_io_write_file(const char* path, const char* content, int64_t len) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(content, 1, (size_t)len, f);
    fclose(f);
    return 1;
}

int sammine_io_append_file(const char* path, const char* content, int64_t len) {
    FILE* f = fopen(path, "ab");
    if (!f) return 0;
    fwrite(content, 1, (size_t)len, f);
    fclose(f);
    return 1;
}