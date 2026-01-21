#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "utils.h"
#include "table.h"

#ifdef _WIN32
#include <windows.h>
static double get_time_sec(void) {
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER count;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

#define MAX_TABLES 8192

static void get_output_path(char *path, size_t size, const char *work_dir, const char *ct_hex) {
    const char *index = getenv("DESTROY_TMP_INDEX");
    if (index) {
        snprintf(path, size, "%s/%s.candidates.tmp.%s", work_dir, ct_hex, index);
    } else {
        snprintf(path, size, "%s/%s.candidates", work_dir, ct_hex);
    }
}

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

static int append_candidates(const char *work_dir, const char *ct_hex, 
                             uint64_t *start_indices, uint32_t *positions, uint32_t count) {
    char path[512];
    get_output_path(path, sizeof(path), work_dir, ct_hex);
    
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    
    for (uint32_t i = 0; i < count; i++) {
        fwrite(&start_indices[i], sizeof(uint64_t), 1, f);
        fwrite(&positions[i], sizeof(uint32_t), 1, f);
    }
    
    fclose(f);
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
    const char *tmp_index = getenv("DESTROY_TMP_INDEX");

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

    if (tmp_index) {
        printf("STATUS [%s] Loaded %u endpoints for %s\n", tmp_index, num_indices, ct_hex);
    } else {
        printf("STATUS Loaded %u endpoints for %s\n", num_indices, ct_hex);
    }

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

    if (tmp_index) {
        printf("STATUS [%s] Found %d tables\n", tmp_index, num_tables);
    } else {
        printf("STATUS Found %d tables\n", num_tables);
    }

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
    double total_time = 0.0;

    for (int t = 0; t < num_tables; t++) {
        if (tmp_index) {
            printf("STATUS [%s] Searching table %d/%d: %s\n", tmp_index, t + 1, num_tables, table_list[t]);
        } else {
            printf("STATUS Searching table %d/%d: %s\n", t + 1, num_tables, table_list[t]);
        }
        
        double load_start = get_time_sec();
        rt_table table = {0};
        if (table_load(&table, table_list[t]) != 0) {
            fprintf(stderr, "WARNING Table load failed: %s\n", table_list[t]);
            free(table_list[t]);
            continue;
        }
        double load_time = get_time_sec() - load_start;

        double search_start = get_time_sec();
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
        double search_time = get_time_sec() - search_start;
        double table_time = load_time + search_time;
        total_time += table_time;

        table_free(&table);

        if (tmp_index) {
            printf("STATUS [%s] Table %d: load=%.3fs search=%.3fs total=%.3fs (%u lookups, %.0f lookups/sec)\n",
                   tmp_index, t + 1, load_time, search_time, table_time, num_indices,
                   search_time > 0 ? num_indices / search_time : 0);
        } else {
            printf("STATUS Table %d: load=%.3fs search=%.3fs total=%.3fs (%u lookups, %.0f lookups/sec)\n",
                   t + 1, load_time, search_time, table_time, num_indices,
                   search_time > 0 ? num_indices / search_time : 0);
        }

        if (count > 0) {
            if (tmp_index) {
                printf("STATUS [%s] Found %u candidates in %s\n", tmp_index, count, table_list[t]);
            } else {
                printf("STATUS Found %u candidates in %s\n", count, table_list[t]);
            }
            if (append_candidates(work_dir, ct_hex, start_indices, positions, count) != 0) {
                fprintf(stderr, "ERROR Failed to save candidates\n");
                write_error(work_dir, ct_hex, "Failed to save candidates");
                free(table_list[t]);
                break;
            }
            total_count += count;
        }

        free(table_list[t]);
    }
    
    // Touch output file even if no candidates
    char cand_path[512];
    get_output_path(cand_path, sizeof(cand_path), work_dir, ct_hex);
    FILE *f = fopen(cand_path, "ab");
    if (f) fclose(f);

    if (tmp_index) {
        printf("STATUS [%s] Total time: %.3fs across %d tables\n", tmp_index, total_time, num_tables);
    } else {
        printf("STATUS Total time: %.3fs across %d tables\n", total_time, num_tables);
    }
    printf("DONE %u\n", total_count);

    free(end_indices);
    free(start_indices);
    free(positions);
    return 0;
}