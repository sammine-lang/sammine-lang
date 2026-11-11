#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// stderr (need global FILE* stderr)
void sammine_io_eprint(const char* s)   { fputs(s, stderr); }
void sammine_io_eprintln(const char* s) { fprintf(stderr, "%s\n", s); }

// Reads hand back what they produce instead of writing through pointers
// into sammine memory. A read is a malloc'd buffer the caller owns and its
// length, or NULL with a length of -1 when there is nothing to read.
typedef struct { char* data; int64_t len; } Read;

// The next line of `f` without its trailing \n.
static Read read_line_from(FILE* f) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, f);
    if (len < 0) { free(line); return (Read){NULL, -1}; }
    if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
    return (Read){line, (int64_t)len};
}

Read sammine_io_read_line(void) { return read_line_from(stdin); }

// The next number on stdin, or 0 if there is none.
int32_t sammine_io_read_i32(void) { int32_t v; return scanf("%d", &v) == 1 ? v : 0; }
int64_t sammine_io_read_i64(void) { int64_t v; return scanf("%ld", &v) == 1 ? v : 0; }
double sammine_io_read_f64(void) { double v; return scanf("%lf", &v) == 1 ? v : 0; }

// File handle operations (FILE* passed as opaque void*)
void* sammine_io_open(const char* path, const char* mode) {
    return (void*)fopen(path, mode);
}

void sammine_io_close(void* handle) {
    if (handle) fclose((FILE*)handle);
}

// Takes the handle, reads its next line, and hands the handle back with it.
typedef struct { void* handle; char* data; int64_t len; } FileRead;

FileRead sammine_io_file_read_line(void* handle) {
    Read r = read_line_from((FILE*)handle);
    return (FileRead){handle, r.data, r.len};
}

void* sammine_io_file_write(void* handle, const char* content, int64_t len) {
    fwrite(content, 1, (size_t)len, (FILE*)handle);
    return handle;
}

// Convenience (open + operate + close in one call). Nothing to read if the
// file cannot be read.
Read sammine_io_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return (Read){NULL, -1};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(f); return (Read){NULL, -1}; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = '\0';
    fclose(f);
    return (Read){buf, (int64_t)rd};
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