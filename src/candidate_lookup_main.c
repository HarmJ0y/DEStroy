#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "utils.h"
#include "table.h"

#define MAX_TABLES 8192

// Structure to track original positions through sorting
typedef struct {
    uint64_t endpoint;
    uint32_t original_pos;
} endpoint_with_pos_t;

// Comparison function for sorting endpoints
static int compare_endpoints(const void *a, const void *b) {
    const endpoint_with_pos_t *ea = (const endpoint_with_pos_t *)a;
    const endpoint_with_pos_t *eb = (const endpoint_with_pos_t *)b;
    if (ea->endpoint < eb->endpoint) return -1;
    if (ea->endpoint > eb->endpoint) return 1;
    return 0;
}

static double get_time_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER count;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void fail(const char *output_file, const char *msg) {
    fprintf(stderr, "ERROR %s\n", msg);
    char path[512];
    snprintf(path, sizeof(path), "%s.error", output_file);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static int collect_tables(const char *path, char **table_list, int *count, int max) {
    dir_iter_t *dir = dir_open(path);
    if (dir) {
        dir_entry_t entry;
        while (dir_next(dir, &entry) && *count < max) {
            size_t len = strlen(entry.name);
            if (len > 3 && strcmp(entry.name + len - 3, ".rt") == 0) {
                char *full_path = malloc(512);
                if (full_path) {
                    snprintf(full_path, 512, "%s" PATH_SEP_STR "%s", path, entry.name);
                    table_list[(*count)++] = full_path;
                }
            }
        }
        dir_close(dir);
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

static int append_candidates(const char *output_file, 
                             uint64_t *start_indices, uint32_t *positions, uint32_t count) {
    FILE *f = fopen(output_file, "ab");
    if (!f) return -1;
    
    for (uint32_t i = 0; i < count; i++) {
        fwrite(&start_indices[i], sizeof(uint64_t), 1, f);
        fwrite(&positions[i], sizeof(uint32_t), 1, f);
    }
    
    fclose(f);
    return 0;
}

static const char *extract_basename(const char *filepath) {
    const char *filename = strrchr(filepath, '/');
    if (!filename) filename = strrchr(filepath, '\\');
    filename = filename ? filename + 1 : filepath;
    
    static char basename[256];
    strncpy(basename, filename, sizeof(basename) - 1);
    basename[sizeof(basename) - 1] = '\0';
    
    char *dot = strrchr(basename, '.');
    if (dot) *dot = '\0';
    
    return basename;
}

/* Sequential merge search for a single table */
static uint32_t search_single_table(rt_table *table,
                                     endpoint_with_pos_t *endpoints, uint32_t num_indices,
                                     uint64_t *start_indices, uint32_t *positions) {
    uint32_t count = 0;
    uint64_t table_idx = 0;
    
    for (uint32_t endpoint_idx = 0; endpoint_idx < num_indices; endpoint_idx++) {
        uint64_t target = endpoints[endpoint_idx].endpoint;
        
        /* Advance table cursor forward until we reach or pass target */
        while (table_idx < table->num_chains) {
            uint64_t current_end = table->data[table_idx * 2 + 1];
            
            if (current_end == target) {
                /* Match found! Save original position, not sorted position */
                start_indices[count] = table->data[table_idx * 2];
                positions[count] = endpoints[endpoint_idx].original_pos;
                count++;
                table_idx++;
                break;
            } else if (current_end < target) {
                /* Table is behind, keep advancing */
                table_idx++;
            } else {
                /* Table is ahead, stop and wait for next endpoint */
                break;
            }
        }
        
        /* Early exit if we've exhausted the table */
        if (table_idx >= table->num_chains) {
            break;
        }
    }
    
    return count;
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <endpoints_file> <table_dir_or_file> [...] [-o output_file]\n", argv[0]);
        return 1;
    }

    const char *endpoints_file = argv[1];
    
    /* Parse arguments: find -o flag or use default */
    char default_output[256];
    snprintf(default_output, sizeof(default_output), "%s.candidates", extract_basename(endpoints_file));
    const char *output_file = default_output;
    
    int table_arg_end = argc;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[i + 1];
            table_arg_end = i;
            break;
        }
    }

    /* Load endpoints */
    uint32_t num_indices = CHAIN_LEN - 1;
    uint64_t *end_indices = malloc(num_indices * sizeof(uint64_t));
    if (!end_indices) {
        fail(output_file, "malloc failed");
        return 1;
    }

    if (load_endpoints(endpoints_file, end_indices, &num_indices) != 0) {
        fail(output_file, "Failed to load endpoints");
        free(end_indices);
        return 1;
    }

    /* Create array with original positions for sorting */
    endpoint_with_pos_t *endpoints_sorted = malloc(num_indices * sizeof(endpoint_with_pos_t));
    if (!endpoints_sorted) {
        fail(output_file, "malloc failed");
        free(end_indices);
        return 1;
    }
    
    for (uint32_t i = 0; i < num_indices; i++) {
        endpoints_sorted[i].endpoint = end_indices[i];
        endpoints_sorted[i].original_pos = i;
    }
    
    /* Sort endpoints while preserving original positions */
    printf("STATUS Sorting %u endpoints...\n", num_indices);
    double sort_start = get_time_sec();
    qsort(endpoints_sorted, num_indices, sizeof(endpoint_with_pos_t), compare_endpoints);
    double sort_time = get_time_sec() - sort_start;
    printf("STATUS Endpoints sorted in %.1fs\n", sort_time);
    
    /* Debug: Show endpoint range */
    printf("STATUS Endpoint range: %016llx to %016llx\n", 
           (unsigned long long)endpoints_sorted[0].endpoint, 
           (unsigned long long)endpoints_sorted[num_indices - 1].endpoint);

    /* Can free original endpoint array now */
    free(end_indices);

    /* Collect table paths */
    char *table_list[MAX_TABLES];
    int num_tables = 0;

    for (int i = 2; i < table_arg_end && num_tables < MAX_TABLES; i++) {
        collect_tables(argv[i], table_list, &num_tables, MAX_TABLES);
    }

    if (num_tables == 0) {
        fail(output_file, "No tables found");
        free(endpoints_sorted);
        return 1;
    }

    printf("STATUS %u endpoints, %d tables\n", num_indices, num_tables);

    /* Allocate result buffers (reused for each table) */
    uint64_t *start_indices = malloc(100000 * sizeof(uint64_t));
    uint32_t *positions = malloc(100000 * sizeof(uint32_t));
    if (!start_indices || !positions) {
        fail(output_file, "malloc failed");
        free(endpoints_sorted);
        free(start_indices);
        free(positions);
        return 1;
    }

    uint32_t total_count = 0;
    double total_time = 0.0;

    /* Process one table at a time */
    for (int t = 0; t < num_tables; t++) {
        double load_start = get_time_sec();
        rt_table table = {0};
        
        if (table_load(&table, table_list[t]) != 0) {
            fprintf(stderr, "WARNING Table load failed: %s\n", table_list[t]);
            free(table_list[t]);
            continue;
        }
        double load_time = get_time_sec() - load_start;

        /* Debug first table */
        if (t == 0) {
            printf("STATUS First table range: %016llx to %016llx\n",
                   (unsigned long long)table.data[1],
                   (unsigned long long)table.data[(table.num_chains - 1) * 2 + 1]);
        }

        /* Sequential merge search through this table */
        double search_start = get_time_sec();
        uint32_t count = search_single_table(&table, endpoints_sorted, num_indices,
                                              start_indices, positions);
        double search_time = get_time_sec() - search_start;
        double table_time = load_time + search_time;
        total_time += table_time;

        table_free(&table);

        printf("STATUS [%d/%d] %s: %.1fs (load: %.1fs, search: %.1fs), %u found\n",
               t + 1, num_tables, table_list[t], table_time, load_time, search_time, count);

        if (count > 0) {
            if (append_candidates(output_file, start_indices, positions, count) != 0) {
                fail(output_file, "Failed to save candidates");
                free(table_list[t]);
                break;
            }
            total_count += count;
        }

        free(table_list[t]);
    }
    
    /* Touch output file even if no candidates */
    FILE *f = fopen(output_file, "ab");
    if (f) fclose(f);

    printf("DONE %u candidates in %.1fs\n", total_count, total_time);

    free(endpoints_sorted);
    free(start_indices);
    free(positions);
    return 0;
}