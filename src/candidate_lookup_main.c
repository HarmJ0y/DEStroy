#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "utils.h"
#include "table.h"

#define MAX_TABLES 8192

static void write_error(const char *work_dir, const char *ct_hex, const char *msg) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.error", work_dir, ct_hex);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static const char *extract_ct_hex(const char *filepath) {
    const char *filename = strrchr(filepath, '/');
    if (!filename) filename = strrchr(filepath, '\\');
    filename = filename ? filename + 1 : filepath;
    
    static char ct_hex[64];
    strncpy(ct_hex, filename, sizeof(ct_hex) - 1);
    ct_hex[sizeof(ct_hex) - 1] = '\0';
    
    char *dot = strrchr(ct_hex, '.');
    if (dot) *dot = '\0';
    
    return ct_hex;
}

static int collect_tables(const char *path, char **table_list, int *count, int max) {
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && *count < max) {
            size_t len = strlen(entry->d_name);
            if (len > 3 && strcmp(entry->d_name + len - 3, ".rt") == 0) {
                char *full_path = malloc(512);
                if (full_path) {
                    snprintf(full_path, 512, "%s/%s", path, entry->d_name);
                    table_list[(*count)++] = full_path;
                }
            }
        }
        closedir(dir);
        return 1;
    } else {
        size_t len = strlen(path);
        if (len > 3 && strcmp(path + len - 3, ".rt") == 0) {
            table_list[(*count)++] = strdup(path);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <endpoints_file> <work_dir> <table_dir_or_file> [...]\n", argv[0]);
        return 1;
    }

    const char *endpoints_file = argv[1];
    const char *work_dir = argv[2];
    const char *ct_hex = extract_ct_hex(endpoints_file);

    uint32_t num_indices = CHAIN_LEN - 1;
    uint64_t *end_indices = malloc(num_indices * sizeof(uint64_t));
    if (!end_indices) {
        fprintf(stderr, "ERROR malloc failed\n");
        write_error(work_dir, ct_hex, "malloc failed");
        return 1;
    }

    if (load_endpoints(endpoints_file, end_indices, &num_indices) != 0) {
        fprintf(stderr, "ERROR Failed to load endpoints: %s\n", endpoints_file);
        write_error(work_dir, ct_hex, "Failed to load endpoints");
        free(end_indices);
        return 1;
    }

    printf("STATUS Loaded %u endpoints for %s\n", num_indices, ct_hex);

    char *table_list[MAX_TABLES];
    int num_tables = 0;

    for (int i = 3; i < argc && num_tables < MAX_TABLES; i++) {
        collect_tables(argv[i], table_list, &num_tables, MAX_TABLES);
    }

    if (num_tables == 0) {
        fprintf(stderr, "ERROR No tables found\n");
        write_error(work_dir, ct_hex, "No tables found");
        free(end_indices);
        return 1;
    }

    printf("STATUS Found %d tables\n", num_tables);

    uint64_t *start_indices = malloc(100000 * sizeof(uint64_t));
    uint32_t *positions = malloc(100000 * sizeof(uint32_t));
    if (!start_indices || !positions) {
        fprintf(stderr, "ERROR malloc failed\n");
        write_error(work_dir, ct_hex, "malloc failed");
        free(end_indices);
        free(start_indices);
        free(positions);
        return 1;
    }

    uint32_t total_count = 0;

    for (int t = 0; t < num_tables; t++) {
        printf("STATUS Searching table %d/%d: %s\n", t + 1, num_tables, table_list[t]);
        
        rt_table table = {0};
        if (table_load(&table, table_list[t]) != 0) {
            fprintf(stderr, "WARNING Table load failed: %s\n", table_list[t]);
            free(table_list[t]);
            continue;
        }

        uint32_t count = 0;
        for (uint32_t pos = 0; pos < num_indices; pos++) {
            int found = 0;
            uint64_t start_index = table_search(&table, end_indices[pos], &found);
            if (found && count < 100000) {
                start_indices[count] = start_index;
                positions[count] = pos;
                count++;
            }
        }

        table_free(&table);

        if (count > 0) {
            printf("STATUS Found %u candidates in %s\n", count, table_list[t]);
            if (append_candidates_to(work_dir, ct_hex, start_indices, positions, count) != 0) {
                fprintf(stderr, "ERROR Failed to save candidates\n");
                write_error(work_dir, ct_hex, "Failed to save candidates");
                free(table_list[t]);
                break;
            }
            total_count += count;
        }

        free(table_list[t]);
    }
    char cand_path[512];
    snprintf(cand_path, sizeof(cand_path), "%s/%s.candidates", work_dir, ct_hex);
    FILE *f = fopen(cand_path, "ab");
    if (f) fclose(f);

    printf("DONE %u\n", total_count);

    free(end_indices);
    free(start_indices);
    free(positions);
    return 0;
}