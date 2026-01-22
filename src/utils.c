#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "platform.h"
#include "utils.h"

#ifdef _WIN32
#define mkdir_p(path) _mkdir(path)
#else
#include <sys/file.h>
#define mkdir_p(path) mkdir(path, 0755)
#endif

int hex_to_bytes(const char *hex, uint8_t *bytes, size_t max_bytes) {
    size_t len = strlen(hex);
    if (len % 2 != 0) return -1;
    
    size_t num_bytes = len / 2;
    if (num_bytes > max_bytes) num_bytes = max_bytes;
    
    for (size_t i = 0; i < num_bytes; i++) {
        unsigned int byte;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) return -1;
        bytes[i] = (uint8_t)byte;
    }
    
    return (int)num_bytes;
}

void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex, size_t hex_size) {
    if (hex_size < len * 2 + 1) return;
    
    for (size_t i = 0; i < len; i++) {
        sprintf(hex + 2 * i, "%02X", bytes[i]);
    }
    hex[len * 2] = '\0';
}

uint64_t get_plaintext_space(void) {
    uint64_t space = 1;
    for (int i = 0; i < PLAINTEXT_LEN_MAX; i++) {
        space *= CHARSET_LEN;
    }
    return space;
}

int save_endpoints(const char *path, uint64_t *end_indices, uint32_t count) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    
    size_t written = fwrite(end_indices, sizeof(uint64_t), count, f);
    fclose(f);
    
    return (written == count) ? 0 : -1;
}

int save_endpoints_to(const char *dir, const char *ct_hex, uint64_t *end_indices, uint32_t count) {
    mkdir_p(dir);
    
    char path[512];
    snprintf(path, sizeof(path), "%s" PATH_SEP_STR "%s.endpoints", dir, ct_hex);
    
    return save_endpoints(path, end_indices, count);
}

int load_endpoints(const char *filepath, uint64_t *end_indices, uint32_t *count) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint32_t num_endpoints = file_size / sizeof(uint64_t);
    if (num_endpoints > *count) num_endpoints = *count;
    
    size_t read = fread(end_indices, sizeof(uint64_t), num_endpoints, f);
    fclose(f);
    
    *count = (uint32_t)read;
    return (read > 0) ? 0 : -1;
}

static void lock_file(FILE *f) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    OVERLAPPED ov = {0};
    LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &ov);
#else
    flock(fileno(f), LOCK_EX);
#endif
}

static void unlock_file(FILE *f) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    OVERLAPPED ov = {0};
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
#else
    flock(fileno(f), LOCK_UN);
#endif
}

int append_candidates_to(const char *dir, const char *ct_hex,
                         uint64_t *start_indices, uint32_t *positions, uint32_t count) {
    mkdir_p(dir);
    
    char path[512];
    snprintf(path, sizeof(path), "%s" PATH_SEP_STR "%s.candidates", dir, ct_hex);
    
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    
    lock_file(f);
    
    for (uint32_t i = 0; i < count; i++) {
        fwrite(&start_indices[i], sizeof(uint64_t), 1, f);
        fwrite(&positions[i], sizeof(uint32_t), 1, f);
    }
    
    unlock_file(f);
    fclose(f);
    
    return 0;
}

int load_candidates(const char *filepath, uint64_t **start_indices, uint32_t **positions, uint32_t *count) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return -1;
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    *count = file_size / 12;  /* 8 bytes start_index + 4 bytes position */
    
    if (*count == 0) {
        fclose(f);
        *start_indices = NULL;
        *positions = NULL;
        return -1;
    }
    
    *start_indices = malloc(*count * sizeof(uint64_t));
    *positions = malloc(*count * sizeof(uint32_t));
    
    if (!*start_indices || !*positions) {
        free(*start_indices);
        free(*positions);
        *start_indices = NULL;
        *positions = NULL;
        fclose(f);
        return -1;
    }
    
    for (uint32_t i = 0; i < *count; i++) {
        if (fread(&(*start_indices)[i], sizeof(uint64_t), 1, f) != 1 ||
            fread(&(*positions)[i], sizeof(uint32_t), 1, f) != 1) {
            free(*start_indices);
            free(*positions);
            *start_indices = NULL;
            *positions = NULL;
            fclose(f);
            return -1;
        }
    }
    
    fclose(f);
    return 0;
}