#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include "platform.h"
#include "utils.h"
#include "table.h"
#include "opencl_host.h"
#include "des.h"
#include "netntlmv1.h"

#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>
#endif

#define MAX_TABLES 8192
#define MAX_CANDIDATES_PER_TABLE 100000
#define MAX_TOTAL_CANDIDATES (4 * 1024 * 1024)
#define MAX_HASHES 2
#define MAX_GPUS 8

/* Streaming pipeline configuration */
#define CANDIDATE_BATCH_SIZE 10000    /* GPU verification batch size (smaller = GPUs get work sooner) */
#define CANDIDATE_QUEUE_BATCHES 32    /* Number of batches in queue (more batches since each is smaller) */

/* ---- Per-hash context ---- */

typedef struct {
    uint64_t endpoint;
    uint32_t original_pos;
} endpoint_with_pos_t;

typedef struct {
    uint8_t ciphertext[8];
    char ct_hex[17];
    endpoint_with_pos_t *endpoints_sorted;
    uint32_t num_indices;
    uint64_t *candidate_starts;
    uint32_t *candidate_positions;
    uint32_t num_candidates;
    uint8_t found_key[7];
    char found_key_hex[15];
    int key_found;
} hash_context_t;

/* ---- Timing helpers ---- */

static double get_time_sec(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
#endif
}

static void format_time(double seconds, char *buf, size_t buf_size) {
    if (seconds < 60)
        snprintf(buf, buf_size, "%.1fs", seconds);
    else if (seconds < 3600)
        snprintf(buf, buf_size, "%dm%ds", (int)(seconds/60), (int)seconds % 60);
    else
        snprintf(buf, buf_size, "%dh%dm", (int)(seconds/3600), ((int)seconds%3600)/60);
}

static const char *timestamp(void) {
    static char ts_buf[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(ts_buf, sizeof(ts_buf), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
    return ts_buf;
}

/* ---- JSON log helpers ---- */

static FILE *json_log_fp = NULL;
static pthread_mutex_t json_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static int json_log_open(const char *path) {
    json_log_fp = fopen(path, "w");
    if (!json_log_fp) {
        fprintf(stderr, "Error: Cannot open JSON log file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    setbuf(json_log_fp, NULL);
    return 0;
}

static void json_timestamp(char *buf, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = gmtime(&tv.tv_sec);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             (int)(tv.tv_usec / 1000));
}

static void jlog(const char *fmt, ...) {
    if (!json_log_fp) return;
    pthread_mutex_lock(&json_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(json_log_fp, fmt, ap);
    va_end(ap);
    fputc('\n', json_log_fp);
    pthread_mutex_unlock(&json_log_mutex);
}

/* ---- Work-balanced GPU split ---- */
/* Position p requires (num_indices - 1 - p) DES chain ops.
   Position 0 is hardest, position num_indices-1 is trivial.
   Split so each GPU does ~equal total DES work. */
static void compute_balanced_splits(uint32_t num_indices, int num_gpus,
                                     uint32_t *offsets, uint32_t *counts) {
    double total_work = (double)num_indices * (num_indices - 1) / 2.0;
    double work_per_gpu = total_work / num_gpus;
    uint32_t start = 0;

    for (int g = 0; g < num_gpus; g++) {
        offsets[g] = start;
        if (g == num_gpus - 1) {
            counts[g] = num_indices - start;
        } else {
            /* Solve: count*(N - 0.5 - start) - count^2/2 = work_per_gpu */
            double a = (double)num_indices - 0.5 - start;
            double disc = a * a - 2.0 * work_per_gpu;
            uint32_t cnt;
            if (disc <= 0) {
                cnt = num_indices - start;
            } else {
                cnt = (uint32_t)(a - sqrt(disc));
                if (cnt == 0) cnt = 1;
                if (start + cnt > num_indices) cnt = num_indices - start;
            }
            counts[g] = cnt;
            start += cnt;
        }
    }
}

/* ---- Table helpers ---- */

static int is_rainbow_table(const char *filename) {
    size_t len = strlen(filename);
    return (len > 3 && strcmp(filename + len - 3, ".rt") == 0);
}

static int find_tables_recursive(const char *dir_path, char **table_paths, int *count, int max_tables) {
    dir_iter_t *dir = dir_open(dir_path);
    if (!dir) return -1;

    dir_entry_t entry;
    while (dir_next(dir, &entry) && *count < max_tables) {
        size_t path_len = strlen(dir_path) + strlen(entry.name) + 2;
        char *full_path = malloc(path_len);
        if (!full_path) continue;

        snprintf(full_path, path_len, "%s" PATH_SEP_STR "%s", dir_path, entry.name);

        if (entry.is_dir) {
            find_tables_recursive(full_path, table_paths, count, max_tables);
            free(full_path);
        } else if (is_rainbow_table(entry.name)) {
            table_paths[*count] = full_path;
            (*count)++;
        } else {
            free(full_path);
        }
    }
    dir_close(dir);
    return 0;
}

/* ---- Endpoint sorting ---- */

static int compare_endpoints(const void *a, const void *b) {
    const endpoint_with_pos_t *ea = (const endpoint_with_pos_t *)a;
    const endpoint_with_pos_t *eb = (const endpoint_with_pos_t *)b;
    if (ea->endpoint < eb->endpoint) return -1;
    if (ea->endpoint > eb->endpoint) return 1;
    return 0;
}

/* Merge search: find matches between sorted endpoints and sorted table chains.
   Returns count of matches found. */
static uint32_t search_single_table(rt_table *table,
                                     endpoint_with_pos_t *endpoints, uint32_t num_indices,
                                     uint64_t *start_indices, uint32_t *positions) {
    uint32_t count = 0;
    uint64_t table_idx = 0;

    for (uint32_t endpoint_idx = 0; endpoint_idx < num_indices; endpoint_idx++) {
        uint64_t target = endpoints[endpoint_idx].endpoint;

        while (table_idx < table->num_chains) {
            uint64_t current_end = table->data[table_idx * 2 + 1];

            if (current_end == target) {
                start_indices[count] = table->data[table_idx * 2];
                positions[count] = endpoints[endpoint_idx].original_pos;
                count++;
                table_idx++;
                break;
            } else if (current_end < target) {
                table_idx++;
            } else {
                break;
            }
        }

        if (table_idx >= table->num_chains) break;
    }

    return count;
}

/* Combined merge search: search ONE table for TWO endpoint arrays in a single pass.
   Both endpoint arrays and table chains must be sorted by endpoint/end_index.
   Writes results into per-hash output arrays. Returns total matches across both. */
static uint32_t search_single_table_multi(rt_table *table,
                                           endpoint_with_pos_t *endpoints0, uint32_t num0,
                                           endpoint_with_pos_t *endpoints1, uint32_t num1,
                                           uint64_t *starts0, uint32_t *pos0, uint32_t *count0,
                                           uint64_t *starts1, uint32_t *pos1, uint32_t *count1) {
    *count0 = 0;
    *count1 = 0;
    uint32_t ei0 = 0, ei1 = 0;
    uint64_t ti = 0;
    uint64_t num_chains = table->num_chains;

    while (ti < num_chains && (ei0 < num0 || ei1 < num1)) {
        uint64_t current_end = table->data[ti * 2 + 1];

        /* Advance endpoint pointers past values smaller than current table entry */
        while (ei0 < num0 && endpoints0[ei0].endpoint < current_end) ei0++;
        while (ei1 < num1 && endpoints1[ei1].endpoint < current_end) ei1++;

        if (ei0 >= num0 && ei1 >= num1) break;

        /* Determine smallest target across both endpoint arrays */
        uint64_t target;
        if (ei0 < num0 && ei1 < num1)
            target = endpoints0[ei0].endpoint < endpoints1[ei1].endpoint ?
                     endpoints0[ei0].endpoint : endpoints1[ei1].endpoint;
        else if (ei0 < num0)
            target = endpoints0[ei0].endpoint;
        else
            target = endpoints1[ei1].endpoint;

        /* Advance table to target */
        while (ti < num_chains && table->data[ti * 2 + 1] < target) ti++;
        if (ti >= num_chains) break;

        current_end = table->data[ti * 2 + 1];

        /* Check matches for both arrays at this table position */
        if (ei0 < num0 && endpoints0[ei0].endpoint == current_end) {
            starts0[*count0] = table->data[ti * 2];
            pos0[*count0] = endpoints0[ei0].original_pos;
            (*count0)++;
            ei0++;
        }
        if (ei1 < num1 && endpoints1[ei1].endpoint == current_end) {
            starts1[*count1] = table->data[ti * 2];
            pos1[*count1] = endpoints1[ei1].original_pos;
            (*count1)++;
            ei1++;
        }

        ti++;
    }

    return *count0 + *count1;
}

/* ---- Multi-threaded lookup (supports multiple hashes per table load) ---- */

#ifndef _WIN32

typedef struct {
    uint64_t *start_indices;
    uint32_t *positions;
    uint32_t count;
    uint32_t capacity;
} candidate_list_t;

typedef struct {
    char **table_paths;
    int table_start;
    int table_end;
    int num_hashes;
    endpoint_with_pos_t *endpoints[MAX_HASHES];
    uint32_t num_indices[MAX_HASHES];
    candidate_list_t candidates[MAX_HASHES];
    int thread_id;
    volatile int *tables_done;
    int total_tables;
} lookup_thread_arg_t;

static void *lookup_worker(void *arg) {
    lookup_thread_arg_t *ta = (lookup_thread_arg_t *)arg;

    for (int h = 0; h < ta->num_hashes; h++)
        ta->candidates[h].count = 0;

    uint64_t *local_starts = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint64_t));
    uint32_t *local_pos = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint32_t));

    for (int t = ta->table_start; t < ta->table_end; t++) {
        rt_table table = {0};
        if (table_load(&table, ta->table_paths[t]) != 0) {
            fprintf(stderr, "  [T%d] WARNING: failed to load %s\n", ta->thread_id, ta->table_paths[t]);
            __sync_add_and_fetch(ta->tables_done, 1);
            continue;
        }

        /* Search this table against each hash's endpoints */
        for (int h = 0; h < ta->num_hashes; h++) {
            uint32_t found = search_single_table(&table, ta->endpoints[h], ta->num_indices[h],
                                                  local_starts, local_pos);
            if (found > 0) {
                uint32_t space = ta->candidates[h].capacity - ta->candidates[h].count;
                if (found > space) found = space;
                memcpy(ta->candidates[h].start_indices + ta->candidates[h].count,
                       local_starts, found * sizeof(uint64_t));
                memcpy(ta->candidates[h].positions + ta->candidates[h].count,
                       local_pos, found * sizeof(uint32_t));
                ta->candidates[h].count += found;
            }
        }

        table_free(&table);

        int done = __sync_add_and_fetch(ta->tables_done, 1);
        if (done % 100 == 0 || done == ta->total_tables) {
            uint32_t total_cands = 0;
            for (int h = 0; h < ta->num_hashes; h++)
                total_cands += ta->candidates[h].count;
            fprintf(stderr, "  Progress: %d/%d tables (%.1f%%) - %u candidates so far (thread %d)\n",
                    done, ta->total_tables, 100.0 * done / ta->total_tables, total_cands, ta->thread_id);
        }
    }

    free(local_starts);
    free(local_pos);
    return NULL;
}

/* ---- Streaming Pipeline Structures ---- */

typedef struct {
    uint64_t *start_indices;
    uint32_t *positions;
    uint32_t count;
    int hash_idx;  /* Which hash this batch belongs to */
} candidate_batch_t;

typedef struct {
    candidate_batch_t *batches;
    int capacity;           /* Number of batch slots */
    int head;               /* Producer inserts here */
    int tail;               /* Consumer reads from here */
    int count;              /* Current batches in queue */
    int done;               /* Producer finished flag */
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} candidate_queue_t;

static void queue_init(candidate_queue_t *q, int capacity) {
    q->batches = calloc(capacity, sizeof(candidate_batch_t));
    for (int i = 0; i < capacity; i++) {
        q->batches[i].start_indices = malloc(CANDIDATE_BATCH_SIZE * sizeof(uint64_t));
        q->batches[i].positions = malloc(CANDIDATE_BATCH_SIZE * sizeof(uint32_t));
        q->batches[i].count = 0;
        q->batches[i].hash_idx = -1;
    }
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->done = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_destroy(candidate_queue_t *q) {
    for (int i = 0; i < q->capacity; i++) {
        free(q->batches[i].start_indices);
        free(q->batches[i].positions);
    }
    free(q->batches);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* Producer: push candidates to queue. Returns 0 on success, -1 if queue is shutting down. */
static int queue_push(candidate_queue_t *q, uint64_t *starts, uint32_t *positions,
                      uint32_t count, int hash_idx) {
    pthread_mutex_lock(&q->mutex);

    /* Wait until there's space or queue is done (early exit) */
    while (q->count >= q->capacity && !q->done) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->done) {
        pthread_mutex_unlock(&q->mutex);
        return -1;  /* Queue shutting down, drop this batch */
    }

    /* Copy data to batch slot */
    candidate_batch_t *batch = &q->batches[q->head];
    memcpy(batch->start_indices, starts, count * sizeof(uint64_t));
    memcpy(batch->positions, positions, count * sizeof(uint32_t));
    batch->count = count;
    batch->hash_idx = hash_idx;

    q->head = (q->head + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/* Consumer: pop batch from queue (returns NULL when done and queue empty) */
static candidate_batch_t *queue_pop(candidate_queue_t *q) {
    pthread_mutex_lock(&q->mutex);

    /* Wait until there's data or producer is done */
    while (q->count == 0 && !q->done) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;  /* Done */
    }

    candidate_batch_t *batch = &q->batches[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    return batch;
}

/* Signal that producer is done (or early exit requested) */
static void queue_finish(candidate_queue_t *q) {
    pthread_mutex_lock(&q->mutex);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);  /* Wake producers blocked on full queue */
    pthread_mutex_unlock(&q->mutex);
}

/* ---- Streaming: I/O Worker with Queue ---- */

typedef struct {
    char **table_paths;
    int table_start;
    int table_end;
    int num_hashes;
    endpoint_with_pos_t *endpoints[MAX_HASHES];
    uint32_t num_indices[MAX_HASHES];
    candidate_queue_t *queue;
    int thread_id;
    volatile int *tables_done;
    int total_tables;
    /* Local accumulator before pushing batch */
    uint64_t *accum_starts[MAX_HASHES];
    uint32_t *accum_positions[MAX_HASHES];
    uint32_t accum_count[MAX_HASHES];
    /* Overlap support: hash 2 might not be ready at start */
    volatile int *hash2_ready;  /* NULL = all hashes ready, non-NULL = poll this flag */
    /* Early exit: stop scanning when all keys found */
    volatile int *all_keys_found;
} streaming_lookup_arg_t;

static void flush_accumulator(streaming_lookup_arg_t *ta, int hash_idx) {
    if (ta->accum_count[hash_idx] == 0) return;

    queue_push(ta->queue,
               ta->accum_starts[hash_idx],
               ta->accum_positions[hash_idx],
               ta->accum_count[hash_idx],
               hash_idx);
    ta->accum_count[hash_idx] = 0;
}

static void add_to_accumulator(streaming_lookup_arg_t *ta, int hash_idx,
                               uint64_t *starts, uint32_t *pos, uint32_t found) {
    for (uint32_t i = 0; i < found; i++) {
        ta->accum_starts[hash_idx][ta->accum_count[hash_idx]] = starts[i];
        ta->accum_positions[hash_idx][ta->accum_count[hash_idx]] = pos[i];
        ta->accum_count[hash_idx]++;

        if (ta->accum_count[hash_idx] >= CANDIDATE_BATCH_SIZE) {
            flush_accumulator(ta, hash_idx);
        }
    }
}

static void *streaming_lookup_worker(void *arg) {
    streaming_lookup_arg_t *ta = (streaming_lookup_arg_t *)arg;

    /* Initialize per-hash accumulators */
    for (int h = 0; h < ta->num_hashes; h++) {
        ta->accum_starts[h] = malloc(CANDIDATE_BATCH_SIZE * sizeof(uint64_t));
        ta->accum_positions[h] = malloc(CANDIDATE_BATCH_SIZE * sizeof(uint32_t));
        ta->accum_count[h] = 0;
    }

    uint64_t *local_starts0 = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint64_t));
    uint32_t *local_pos0 = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint32_t));
    uint64_t *local_starts1 = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint64_t));
    uint32_t *local_pos1 = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint32_t));

    for (int t = ta->table_start; t < ta->table_end; t++) {
        rt_table table = {0};
        if (table_load(&table, ta->table_paths[t]) != 0) {
            __sync_add_and_fetch(ta->tables_done, 1);
            continue;
        }

        int use_multi = (ta->num_hashes == 2 && ta->endpoints[0] != ta->endpoints[1]);

        if (use_multi) {
            /* Combined single-pass search for both hashes */
            uint32_t c0 = 0, c1 = 0;
            search_single_table_multi(&table,
                                       ta->endpoints[0], ta->num_indices[0],
                                       ta->endpoints[1], ta->num_indices[1],
                                       local_starts0, local_pos0, &c0,
                                       local_starts1, local_pos1, &c1);
            add_to_accumulator(ta, 0, local_starts0, local_pos0, c0);
            add_to_accumulator(ta, 1, local_starts1, local_pos1, c1);
        } else {
            /* Single hash, identical hashes, or hash2 not ready yet */
            int search_count = ta->num_hashes;
            /* If endpoints are shared (identical hashes), search only once */
            if (ta->num_hashes == 2 && ta->endpoints[0] == ta->endpoints[1])
                search_count = 1;

            for (int h = 0; h < search_count; h++) {
                uint32_t found = search_single_table(&table, ta->endpoints[h], ta->num_indices[h],
                                                      local_starts0, local_pos0);
                add_to_accumulator(ta, h, local_starts0, local_pos0, found);

                /* If identical hashes, duplicate results for hash 1 */
                if (search_count == 1 && ta->num_hashes == 2) {
                    add_to_accumulator(ta, 1, local_starts0, local_pos0, found);
                }
            }
        }

        table_free(&table);

        int done = __sync_add_and_fetch(ta->tables_done, 1);
        if (done % 100 == 0 || done == ta->total_tables) {
            fprintf(stderr, "  I/O Progress: %d/%d tables (%.1f%%)\n",
                    done, ta->total_tables, 100.0 * done / ta->total_tables);
        }
        {
            int pct = (int)(100.0 * done / ta->total_tables);
            int prev_pct = (int)(100.0 * (done - 1) / ta->total_tables);
            if (pct > prev_pct || done == ta->total_tables) {
                char ts[64];
                json_timestamp(ts, sizeof(ts));
                jlog("{\"event\":\"scan_progress\",\"timestamp\":\"%s\","
                     "\"tables_done\":%d,\"total_tables\":%d,\"pct\":%.1f}",
                     ts, done, ta->total_tables, 100.0 * done / ta->total_tables);
            }
        }

        /* Early exit if all keys found */
        if (ta->all_keys_found && *ta->all_keys_found) break;
    }

    /* Flush remaining candidates */
    for (int h = 0; h < ta->num_hashes; h++) {
        flush_accumulator(ta, h);
        free(ta->accum_starts[h]);
        free(ta->accum_positions[h]);
    }

    free(local_starts0);
    free(local_pos0);
    free(local_starts1);
    free(local_pos1);
    return NULL;
}

/* ---- Streaming: GPU Verification Worker ---- */

typedef struct {
    candidate_queue_t *queue;
    gpu_context *gpu;
    hash_context_t *hashes;
    int num_hashes;
    uint64_t plaintext_space;
    volatile uint32_t *verified_count;
    volatile int *all_keys_found;  /* Set when all hashes are cracked */
} gpu_verify_arg_t;

static void *gpu_verify_worker(void *arg) {
    gpu_verify_arg_t *va = (gpu_verify_arg_t *)arg;

    while (1) {
        candidate_batch_t *batch = queue_pop(va->queue);
        if (!batch) break;  /* Queue empty and done */

        int h = batch->hash_idx;
        if (h < 0 || h >= va->num_hashes) continue;

        /* Skip if already found key for this hash */
        if (va->hashes[h].key_found) {
            __sync_add_and_fetch(va->verified_count, batch->count);
            continue;
        }

        /* Run GPU verification using pre-allocated buffers */
        uint8_t key_bytes[7] = {0};
        int result = gpu_check_false_alarms_fast(va->gpu, va->hashes[h].ciphertext,
                                                  batch->start_indices, batch->positions,
                                                  batch->count, REDUCTION_OFFSET,
                                                  va->plaintext_space, key_bytes);

        __sync_add_and_fetch(va->verified_count, batch->count);

        if (result == 1) {
            va->hashes[h].key_found = 1;
            memcpy(va->hashes[h].found_key, key_bytes, 7);
            bytes_to_hex(key_bytes, 7, va->hashes[h].found_key_hex, sizeof(va->hashes[h].found_key_hex));
            fprintf(stderr, "  GPU: KEY FOUND for hash %d -> %s\n", h + 1, va->hashes[h].found_key_hex);
            {
                char ts[64];
                json_timestamp(ts, sizeof(ts));
                jlog("{\"event\":\"key_found\",\"timestamp\":\"%s\","
                     "\"hash_idx\":%d,\"ct_hex\":\"%s\",\"key_hex\":\"%s\"}",
                     ts, h, va->hashes[h].ct_hex, va->hashes[h].found_key_hex);
            }

            /* Check if ALL hashes are now found */
            int all_done = 1;
            for (int i = 0; i < va->num_hashes; i++) {
                if (!va->hashes[i].key_found) { all_done = 0; break; }
            }
            if (all_done && va->all_keys_found) {
                __sync_val_compare_and_swap((int *)va->all_keys_found, 0, 1);
                fprintf(stderr, "  GPU: All keys found! Signaling early exit.\n");
                /* Signal queue so I/O threads see done flag after they flush */
                queue_finish(va->queue);
            }
        }
    }

    return NULL;
}

/* ---- Multi-GPU Precompute Worker ---- */

typedef struct {
    gpu_context *gpu;
    const uint8_t *ciphertext;
    uint32_t chain_len;
    uint32_t reduction_offset;
    uint64_t plaintext_space;
    uint64_t *output;
    uint32_t pos_offset;
    uint32_t count;
    int gpu_id;
    int result;
    precompute_progress_fn cb;
    void *cb_data;
} precompute_thread_arg_t;

/* ---- Precompute Progress Reporting ---- */

typedef struct {
    int hash_idx;
    const char *ct_hex;
    int last_reported_pct;
} precompute_progress_ctx_t;

static void precompute_progress_single(uint32_t rounds_done, uint32_t total_rounds, void *user_data) {
    precompute_progress_ctx_t *ctx = (precompute_progress_ctx_t *)user_data;
    double pct = 100.0 * rounds_done / total_rounds;
    int ipct = (int)pct;

    if (rounds_done >= total_rounds) {
        fprintf(stderr, "\r  Precompute: 100.0%% (%u/%u rounds)                    \n", total_rounds, total_rounds);
    } else {
        fprintf(stderr, "\r  Precompute: %.1f%% (%u/%u rounds)    ", pct, rounds_done, total_rounds);
    }

    if (ipct > ctx->last_reported_pct || rounds_done >= total_rounds) {
        ctx->last_reported_pct = ipct;
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        jlog("{\"event\":\"precompute_progress\",\"timestamp\":\"%s\","
             "\"hash_idx\":%d,\"ct_hex\":\"%s\",\"rounds_done\":%u,"
             "\"total_rounds\":%u,\"pct\":%.1f}",
             ts, ctx->hash_idx, ctx->ct_hex, rounds_done, total_rounds, pct);
    }
}

typedef struct {
    int hash_idx;
    const char *ct_hex;
    int num_gpus;
    uint32_t gpu_rounds[MAX_GPUS];
    uint32_t gpu_total[MAX_GPUS];
    int last_reported_pct;
    pthread_mutex_t mutex;
} precompute_multi_progress_t;

typedef struct {
    precompute_multi_progress_t *shared;
    int gpu_id;
} precompute_gpu_cb_ctx_t;

static void precompute_progress_multi(uint32_t rounds_done, uint32_t total_rounds, void *user_data) {
    precompute_gpu_cb_ctx_t *gctx = (precompute_gpu_cb_ctx_t *)user_data;
    precompute_multi_progress_t *shared = gctx->shared;

    pthread_mutex_lock(&shared->mutex);
    shared->gpu_rounds[gctx->gpu_id] = rounds_done;
    shared->gpu_total[gctx->gpu_id] = total_rounds;

    uint64_t sum_done = 0, sum_total = 0;
    for (int g = 0; g < shared->num_gpus; g++) {
        sum_done += shared->gpu_rounds[g];
        sum_total += shared->gpu_total[g];
    }

    double pct = sum_total > 0 ? 100.0 * sum_done / sum_total : 0.0;
    int ipct = (int)pct;
    int all_done = (sum_done >= sum_total && sum_total > 0);

    if (all_done) {
        fprintf(stderr, "\r  Precompute: 100.0%% (%d GPUs done)                    \n", shared->num_gpus);
    } else {
        fprintf(stderr, "\r  Precompute: %.1f%% (%d GPUs)    ", pct, shared->num_gpus);
    }

    if (ipct > shared->last_reported_pct || all_done) {
        shared->last_reported_pct = ipct;
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        jlog("{\"event\":\"precompute_progress\",\"timestamp\":\"%s\","
             "\"hash_idx\":%d,\"ct_hex\":\"%s\",\"rounds_done\":%lu,"
             "\"total_rounds\":%lu,\"pct\":%.1f}",
             ts, shared->hash_idx, shared->ct_hex,
             (unsigned long)sum_done, (unsigned long)sum_total, pct);
    }

    pthread_mutex_unlock(&shared->mutex);
}

static void *precompute_gpu_worker(void *arg) {
    precompute_thread_arg_t *a = (precompute_thread_arg_t *)arg;
    a->result = gpu_precompute_chunked_range(a->gpu, a->ciphertext, a->chain_len,
                                              a->reduction_offset, a->plaintext_space,
                                              a->output, a->pos_offset, a->count,
                                              a->cb, a->cb_data);
    return NULL;
}

/* ---- Multi-GPU Verification Worker ---- */

typedef struct {
    gpu_context *gpu;
    const uint8_t *ciphertext;
    uint64_t *start_indices;
    uint32_t *positions;
    uint32_t num_candidates;
    uint32_t reduction_offset;
    uint64_t plaintext_space;
    uint8_t found_key[7];
    int gpu_id;
    int result;
} verify_thread_arg_t;

static void *verify_gpu_worker(void *arg) {
    verify_thread_arg_t *a = (verify_thread_arg_t *)arg;
    a->result = gpu_check_false_alarms(a->gpu, a->ciphertext,
                                        a->start_indices, a->positions,
                                        a->num_candidates, a->reduction_offset,
                                        a->plaintext_space, a->found_key);
    return NULL;
}

#endif /* _WIN32 */

/* ---- CT3 brute-force (2-byte key space, CPU) ---- */

static int bruteforce_ct3(const uint8_t *ct3, uint8_t *found_key2) {
    for (uint32_t hi = 0; hi < 256; hi++) {
        for (uint32_t lo = 0; lo < 256; lo++) {
            uint8_t key7[7] = { (uint8_t)hi, (uint8_t)lo, 0, 0, 0, 0, 0 };
            uint8_t output[8];
            des_encrypt_ntlmv1(key7, output);
            if (memcmp(output, ct3, 8) == 0) {
                found_key2[0] = (uint8_t)hi;
                found_key2[1] = (uint8_t)lo;
                return 1;
            }
        }
    }
    return 0;
}

/* ---- NetNTLMv1 hash string parser ---- */
/* Format: user::domain:LM_response:NT_response:challenge
   Returns number of fields parsed, fills ct_hex[0..2] with 16-char hex CTs from NT response */
static int parse_ntlmv1(const char *hash_str, char ct_hex[3][17], char *challenge_hex) {
    /* Count colons to validate format */
    int colons = 0;
    for (const char *p = hash_str; *p; p++)
        if (*p == ':') colons++;

    if (colons < 4) return -1;

    /* Find fields by scanning for colons */
    const char *fields[7];
    int field_lens[7];
    int nfields = 0;
    const char *p = hash_str;
    for (int f = 0; f < 6 && *p; f++) {
        fields[f] = p;
        const char *colon = strchr(p, ':');
        if (colon) {
            field_lens[f] = (int)(colon - p);
            p = colon + 1;
        } else {
            field_lens[f] = (int)strlen(p);
            p += field_lens[f];
        }
        nfields++;
    }

    if (nfields < 5) return -1;

    /* NT response is field index 4 (0-based), should be 48 hex chars */
    int nt_idx = 4;
    /* But if field 1 is empty (user::domain format), fields shift.
       Actually fields[0]=user, fields[1]="", fields[2]=domain, fields[3]=LM, fields[4]=NT, fields[5]=challenge
       With a single colon between user and domain: fields[0]=user, fields[1]=domain, fields[2]=LM, fields[3]=NT, fields[4]=challenge
       Detect by checking field lengths and content */

    /* Find the 48-char hex field that looks like a response */
    nt_idx = -1;
    int challenge_idx = -1;
    for (int f = 0; f < nfields; f++) {
        if (field_lens[f] == 48) {
            /* Could be LM or NT response; take the last 48-char field as NT */
            nt_idx = f;
        }
        if (field_lens[f] == 16 && f == nfields - 1) {
            challenge_idx = f;
        }
    }

    if (nt_idx < 0) {
        fprintf(stderr, "Error: Could not find 48-char NT response in hash string\n");
        return -1;
    }

    if (challenge_idx >= 0) {
        memcpy(challenge_hex, fields[challenge_idx], 16);
        challenge_hex[16] = '\0';
    } else {
        strcpy(challenge_hex, "1122334455667788");
    }

    /* Extract 3 CTs from NT response (each 16 hex chars) */
    const char *nt = fields[nt_idx];
    for (int i = 0; i < 3; i++) {
        memcpy(ct_hex[i], nt + i * 16, 16);
        ct_hex[i][16] = '\0';
    }

    return 3;
}

/* ---- Main ---- */

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <table_dir> <ciphertext>                  Crack 1 DES CT\n", prog);
    fprintf(stderr, "  %s <table_dir> <ct1> <ct2>                   Crack 2 DES CTs\n", prog);
    fprintf(stderr, "  %s <table_dir> <NetNTLMv1_hash>              Full end-to-end crack\n", prog);
    fprintf(stderr, "\nNetNTLMv1 hash format: user::domain:LM_resp:NT_resp:challenge\n");
    fprintf(stderr, "  Extracts CT1+CT2 from NT response, cracks both, brute-forces CT3,\n");
    fprintf(stderr, "  and reconstructs the full NTLM hash.\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --gpus N        Number of GPUs (default: 4)\n");
    fprintf(stderr, "  --io-threads N  Number of I/O threads (default: 32)\n");
    fprintf(stderr, "  --streaming     Enable streaming pipeline (default: on)\n");
    fprintf(stderr, "  --no-streaming  Disable streaming, use batch mode\n");
    fprintf(stderr, "  --json-log F    Write JSONL progress events to file F\n");
    fprintf(stderr, "\nAvailable GPUs:\n");
    gpu_list();
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    const char *table_dir = argv[1];
    int num_gpus = 4;
    int io_threads = 32;
    int streaming_mode = 1;
    int ntlmv1_mode = 0;

    /* NetNTLMv1 end-to-end state */
    uint8_t ct3_bytes[8] = {0};
    char ct3_hex[17] = {0};
    char challenge_hex[17] = {0};
    char ntlmv1_input[512] = {0};

    /* Parse hash arguments (1 or 2 before any --flags) */
    int num_hashes = 0;
    hash_context_t hashes[MAX_HASHES];
    memset(hashes, 0, sizeof(hashes));

    /* Check if argv[2] is a NetNTLMv1 hash string (contains ':') */
    if (argc >= 3 && strchr(argv[2], ':') != NULL) {
        ntlmv1_mode = 1;
        strncpy(ntlmv1_input, argv[2], sizeof(ntlmv1_input) - 1);

        char parsed_cts[3][17];
        int n = parse_ntlmv1(argv[2], parsed_cts, challenge_hex);
        if (n != 3) {
            fprintf(stderr, "Error: Failed to parse NetNTLMv1 hash string\n");
            return 1;
        }

        /* Validate challenge */
        if (strcasecmp(challenge_hex, "1122334455667788") != 0) {
            fprintf(stderr, "Error: Server challenge must be 1122334455667788 (got %s)\n", challenge_hex);
            fprintf(stderr, "  Use crack.sh SSP trick to convert the hash first.\n");
            return 1;
        }

        /* Set up CT1 and CT2 for rainbow table cracking */
        for (int i = 0; i < 2; i++) {
            strncpy(hashes[i].ct_hex, parsed_cts[i], 16);
            hashes[i].ct_hex[16] = '\0';
            if (hex_to_bytes(parsed_cts[i], hashes[i].ciphertext, 8) != 8) {
                fprintf(stderr, "Error: Invalid ciphertext in NT response: '%s'\n", parsed_cts[i]);
                return 1;
            }
        }
        num_hashes = 2;

        /* Save CT3 for brute-force later */
        strncpy(ct3_hex, parsed_cts[2], 16);
        ct3_hex[16] = '\0';
        hex_to_bytes(parsed_cts[2], ct3_bytes, 8);

        /* flags start at argv[3] */
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--gpus") == 0 && i + 1 < argc)
                num_gpus = atoi(argv[++i]);
            else if (strcmp(argv[i], "--io-threads") == 0 && i + 1 < argc)
                io_threads = atoi(argv[++i]);
            else if (strcmp(argv[i], "--streaming") == 0)
                streaming_mode = 1;
            else if (strcmp(argv[i], "--no-streaming") == 0)
                streaming_mode = 0;
            else if (strcmp(argv[i], "--json-log") == 0 && i + 1 < argc) {
                if (json_log_open(argv[++i]) != 0) return 1;
            }
        }
    } else {
        /* Original mode: 1 or 2 raw ciphertext arguments */
        for (int i = 2; i < argc && num_hashes < MAX_HASHES; i++) {
            if (argv[i][0] == '-') break;
            if (strlen(argv[i]) != 16) {
                fprintf(stderr, "Error: '%s' is not a valid 16-char hex ciphertext\n", argv[i]);
                return 1;
            }
            strncpy(hashes[num_hashes].ct_hex, argv[i], 16);
            hashes[num_hashes].ct_hex[16] = '\0';
            if (hex_to_bytes(argv[i], hashes[num_hashes].ciphertext, 8) != 8) {
                fprintf(stderr, "Error: Invalid ciphertext '%s' (need 16 hex chars)\n", argv[i]);
                return 1;
            }
            num_hashes++;
        }

        if (num_hashes == 0) {
            fprintf(stderr, "Error: No ciphertext provided\n");
            print_usage(argv[0]);
            return 1;
        }

        /* Parse flags */
        for (int i = 2 + num_hashes; i < argc; i++) {
            if (strcmp(argv[i], "--gpus") == 0 && i + 1 < argc)
                num_gpus = atoi(argv[++i]);
            else if (strcmp(argv[i], "--io-threads") == 0 && i + 1 < argc)
                io_threads = atoi(argv[++i]);
            else if (strcmp(argv[i], "--streaming") == 0)
                streaming_mode = 1;
            else if (strcmp(argv[i], "--no-streaming") == 0)
                streaming_mode = 0;
            else if (strcmp(argv[i], "--json-log") == 0 && i + 1 < argc) {
                if (json_log_open(argv[++i]) != 0) return 1;
            }
        }
    }

    double total_start = get_time_sec();
    char time_buf[64];

    printf("\n");
    printf("=== DEStroy Optimized - NetNTLMv1 DES Key Recovery ===\n");
    if (ntlmv1_mode) {
        printf("Mode: END-TO-END NetNTLMv1\n");
        printf("  CT1: %s (rainbow table)\n", hashes[0].ct_hex);
        printf("  CT2: %s (rainbow table)\n", hashes[1].ct_hex);
        printf("  CT3: %s (brute-force)\n", ct3_hex);
    } else {
        printf("Targets: %d hash(es)\n", num_hashes);
        for (int h = 0; h < num_hashes; h++)
            printf("  [%d] %s\n", h + 1, hashes[h].ct_hex);
    }
    printf("Pipeline: %s\n", streaming_mode ? "STREAMING (I/O + GPU overlap)" : "BATCH");
    printf("\n");

    /* JSON log: start event */
    {
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        jlog("{\"event\":\"start\",\"timestamp\":\"%s\",\"mode\":\"%s\",\"num_hashes\":%d,"
             "\"ct1\":\"%s\",\"ct2\":\"%s\",\"ct3\":\"%s\","
             "\"pipeline\":\"%s\",\"num_gpus\":%d,\"io_threads\":%d}",
             ts, ntlmv1_mode ? "ntlmv1" : "raw", num_hashes,
             hashes[0].ct_hex,
             num_hashes > 1 ? hashes[1].ct_hex : "",
             ntlmv1_mode ? ct3_hex : "",
             streaming_mode ? "streaming" : "batch",
             num_gpus, io_threads);
    }

    /* ---- Phase 1: GPU Init ---- */
    printf("[%s] [1/4] Initializing GPU(s)...\n", timestamp());
    double step_start = get_time_sec();

    gpu_context gpus[MAX_GPUS];
    int actual_gpus = 0;

    for (int i = 0; i < num_gpus && i < MAX_GPUS; i++) {
        memset(&gpus[i], 0, sizeof(gpu_context));
        if (gpu_init_index(&gpus[i], i) != 0) {
            fprintf(stderr, "  Warning: Failed to init GPU %d\n", i);
            continue;
        }
        if (gpu_load_kernel(&gpus[i], "kernels" PATH_SEP_STR "precompute.cl", "precompute") != 0) {
            fprintf(stderr, "  Warning: Failed to load precompute kernel on GPU %d\n", i);
            gpu_cleanup(&gpus[i]);
            continue;
        }
        if (gpu_load_false_alarm_kernel(&gpus[i], "kernels" PATH_SEP_STR "false_alarm.cl") != 0) {
            fprintf(stderr, "  Warning: Failed to load false alarm kernel on GPU %d\n", i);
            gpu_cleanup(&gpus[i]);
            continue;
        }
        printf("  GPU %d: %s (%u CUs)\n", i, gpus[i].device_name, gpus[i].compute_units);
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"gpu_init\",\"timestamp\":\"%s\",\"gpu_id\":%d,"
                 "\"name\":\"%s\",\"compute_units\":%u}",
                 ts, i, gpus[i].device_name, gpus[i].compute_units);
        }
        actual_gpus++;
    }

    if (actual_gpus == 0) {
        fprintf(stderr, "Error: No GPUs initialized successfully\n");
        return 1;
    }

    format_time(get_time_sec() - step_start, time_buf, sizeof(time_buf));
    printf("  %d GPU(s) ready [%s]\n\n", actual_gpus, time_buf);
    {
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        jlog("{\"event\":\"gpu_init_done\",\"timestamp\":\"%s\",\"num_gpus\":%d,\"elapsed_sec\":%.1f}",
             ts, actual_gpus, get_time_sec() - step_start);
    }

    /* ---- Phase 2: Precompute endpoints + Phase 3: Table Lookup ---- */
    printf("[%s] [2/4] Precomputing endpoints...\n", timestamp());
    step_start = get_time_sec();

    uint64_t plaintext_space = get_plaintext_space();
    uint32_t num_indices = CHAIN_LEN - 1;

    /* Check for identical hashes */
    int identical_hashes = 0;
    if (num_hashes == 2 && strcmp(hashes[0].ct_hex, hashes[1].ct_hex) == 0) {
        identical_hashes = 1;
        printf("  NOTE: Both hashes are identical — computing endpoints once\n");
    }

    /* Find all tables early (needed for overlap) */
    char **table_paths = calloc(MAX_TABLES, sizeof(char *));
    int num_tables = 0;

    struct stat st;
    if (stat(table_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        find_tables_recursive(table_dir, table_paths, &num_tables, MAX_TABLES);
    } else if (is_rainbow_table(table_dir)) {
        table_paths[0] = strdup(table_dir);
        num_tables = 1;
    }

    if (num_tables == 0) {
        fprintf(stderr, "Error: No rainbow tables found in %s\n", table_dir);
        free(table_paths);
        for (int g = 0; g < actual_gpus; g++) gpu_cleanup(&gpus[g]);
        return 1;
    }

    /* Helper macro: precompute endpoints for hash h using given GPUs */
    #define DO_PRECOMPUTE(h_idx, gpu_arr, gpu_count) do { \
        uint64_t *end_indices = malloc(num_indices * sizeof(uint64_t)); \
        if (!end_indices) { fprintf(stderr, "Error: Out of memory\n"); return 1; } \
        if ((gpu_count) > 1) { \
            pthread_t _pthreads[MAX_GPUS]; \
            precompute_thread_arg_t _pargs[MAX_GPUS]; \
            uint32_t _offsets[MAX_GPUS], _counts[MAX_GPUS]; \
            precompute_multi_progress_t _mprog; \
            precompute_gpu_cb_ctx_t _gpu_cb_ctx[MAX_GPUS]; \
            memset(&_mprog, 0, sizeof(_mprog)); \
            _mprog.hash_idx = (h_idx); \
            _mprog.ct_hex = hashes[(h_idx)].ct_hex; \
            _mprog.num_gpus = (gpu_count); \
            _mprog.last_reported_pct = -1; \
            pthread_mutex_init(&_mprog.mutex, NULL); \
            compute_balanced_splits(num_indices, (gpu_count), _offsets, _counts); \
            for (int _g = 0; _g < (gpu_count); _g++) { \
                _gpu_cb_ctx[_g].shared = &_mprog; \
                _gpu_cb_ctx[_g].gpu_id = _g; \
                _pargs[_g].gpu = &(gpu_arr)[_g]; \
                _pargs[_g].ciphertext = hashes[(h_idx)].ciphertext; \
                _pargs[_g].chain_len = CHAIN_LEN; \
                _pargs[_g].reduction_offset = REDUCTION_OFFSET; \
                _pargs[_g].plaintext_space = plaintext_space; \
                _pargs[_g].output = end_indices + _offsets[_g]; \
                _pargs[_g].pos_offset = _offsets[_g]; \
                _pargs[_g].count = _counts[_g]; \
                _pargs[_g].gpu_id = _g; \
                _pargs[_g].cb = precompute_progress_multi; \
                _pargs[_g].cb_data = &_gpu_cb_ctx[_g]; \
                pthread_create(&_pthreads[_g], NULL, precompute_gpu_worker, &_pargs[_g]); \
            } \
            for (int _g = 0; _g < (gpu_count); _g++) { \
                pthread_join(_pthreads[_g], NULL); \
                if (_pargs[_g].result < 0) { fprintf(stderr, "Error: precompute failed\n"); free(end_indices); return 1; } \
            } \
            pthread_mutex_destroy(&_mprog.mutex); \
        } else { \
            precompute_progress_ctx_t _sprog = { (h_idx), hashes[(h_idx)].ct_hex, -1 }; \
            if (gpu_precompute_chunked(&(gpu_arr)[0], hashes[(h_idx)].ciphertext, CHAIN_LEN, REDUCTION_OFFSET, \
                                        plaintext_space, end_indices, \
                                        precompute_progress_single, &_sprog) < 0) { \
                fprintf(stderr, "Error: precompute failed\n"); free(end_indices); return 1; \
            } \
        } \
        hashes[(h_idx)].num_indices = num_indices; \
        hashes[(h_idx)].endpoints_sorted = malloc(num_indices * sizeof(endpoint_with_pos_t)); \
        for (uint32_t _i = 0; _i < num_indices; _i++) { \
            hashes[(h_idx)].endpoints_sorted[_i].endpoint = end_indices[_i]; \
            hashes[(h_idx)].endpoints_sorted[_i].original_pos = _i; \
        } \
        qsort(hashes[(h_idx)].endpoints_sorted, num_indices, sizeof(endpoint_with_pos_t), compare_endpoints); \
        free(end_indices); \
    } while(0)

    /* Precompute hash 1 (always done first, using all GPUs) */
    {
        double hash_start = get_time_sec();
        printf("  Hash 1/%d: %s (%d GPU%s)\n", identical_hashes ? 1 : num_hashes, hashes[0].ct_hex,
               actual_gpus, actual_gpus > 1 ? "s" : "");
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"precompute_start\",\"timestamp\":\"%s\",\"hash_idx\":0,"
                 "\"ct_hex\":\"%s\",\"num_indices\":%u}",
                 ts, hashes[0].ct_hex, num_indices);
        }
#ifndef _WIN32
        DO_PRECOMPUTE(0, gpus, actual_gpus);
#else
        DO_PRECOMPUTE(0, gpus, 1);
#endif
        format_time(get_time_sec() - hash_start, time_buf, sizeof(time_buf));
        printf("  -> %u endpoints computed + sorted [%s]\n", num_indices, time_buf);
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"precompute_done\",\"timestamp\":\"%s\",\"hash_idx\":0,"
                 "\"elapsed_sec\":%.1f}",
                 ts, get_time_sec() - hash_start);
        }
    }

    /* For identical hashes, share endpoint data and skip hash 2 precompute */
    if (identical_hashes) {
        hashes[1].num_indices = hashes[0].num_indices;
        hashes[1].endpoints_sorted = hashes[0].endpoints_sorted;
        printf("  Hash 2 shares endpoints with hash 1\n");
    } else if (num_hashes == 2) {
        /* Precompute hash 2 */
        double hash2_start = get_time_sec();
        printf("  [%s] Hash 2/2: %s (%d GPU%s)\n", timestamp(), hashes[1].ct_hex,
               actual_gpus, actual_gpus > 1 ? "s" : "");
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"precompute_start\",\"timestamp\":\"%s\",\"hash_idx\":1,"
                 "\"ct_hex\":\"%s\",\"num_indices\":%u}",
                 ts, hashes[1].ct_hex, num_indices);
        }
#ifndef _WIN32
        DO_PRECOMPUTE(1, gpus, actual_gpus);
#else
        DO_PRECOMPUTE(1, gpus, 1);
#endif
        format_time(get_time_sec() - hash2_start, time_buf, sizeof(time_buf));
        printf("  -> %u endpoints computed + sorted [%s]\n", num_indices, time_buf);
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"precompute_done\",\"timestamp\":\"%s\",\"hash_idx\":1,"
                 "\"elapsed_sec\":%.1f}",
                 ts, get_time_sec() - hash2_start);
        }
    }

    format_time(get_time_sec() - step_start, time_buf, sizeof(time_buf));
    printf("  Total precompute [%s]\n\n", time_buf);

    /* ---- Phase 3: Table Lookup ---- */
    printf("[%s] [3/4] Searching tables...\n", timestamp());
    step_start = get_time_sec();
    printf("  Found %d table(s), searching for %d hash(es) per table\n", num_tables, num_hashes);
    {
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        jlog("{\"event\":\"scan_start\",\"timestamp\":\"%s\",\"num_tables\":%d,\"num_hashes\":%d}",
             ts, num_tables, num_hashes);
    }

#ifndef _WIN32
    if (streaming_mode && io_threads > 1) {
        /* ---- STREAMING PIPELINE MODE ---- */
        printf("  Starting streaming pipeline (%d I/O threads + %d GPU verifiers)\n",
               io_threads, actual_gpus);

        candidate_queue_t queue;
        queue_init(&queue, CANDIDATE_QUEUE_BATCHES * actual_gpus);

        volatile int tables_done = 0;
        volatile uint32_t verified_count = 0;
        volatile int all_keys_found = 0;

        /* Pre-allocate GPU verification buffers */
        for (int g = 0; g < actual_gpus; g++) {
            if (gpu_alloc_verify_buffers(&gpus[g], CANDIDATE_BATCH_SIZE) != 0)
                fprintf(stderr, "  Warning: Failed to pre-alloc verify buffers on GPU %d\n", g);
        }

        /* Start GPU verification threads — one per GPU */
        gpu_verify_arg_t verify_args[MAX_GPUS];
        pthread_t gpu_threads[MAX_GPUS];
        for (int g = 0; g < actual_gpus; g++) {
            verify_args[g].queue = &queue;
            verify_args[g].gpu = &gpus[g];
            verify_args[g].hashes = hashes;
            verify_args[g].num_hashes = num_hashes;
            verify_args[g].plaintext_space = plaintext_space;
            verify_args[g].verified_count = &verified_count;
            verify_args[g].all_keys_found = &all_keys_found;
            pthread_create(&gpu_threads[g], NULL, gpu_verify_worker, &verify_args[g]);
        }

        /* Start I/O threads */
        pthread_t *io_threads_arr = malloc(io_threads * sizeof(pthread_t));
        streaming_lookup_arg_t *io_args = malloc(io_threads * sizeof(streaming_lookup_arg_t));

        int tables_per_thread = num_tables / io_threads;
        int remainder = num_tables % io_threads;
        int offset = 0;

        for (int i = 0; i < io_threads; i++) {
            io_args[i].table_paths = table_paths;
            io_args[i].table_start = offset;
            int count = tables_per_thread + (i < remainder ? 1 : 0);
            io_args[i].table_end = offset + count;
            offset += count;
            io_args[i].num_hashes = num_hashes;
            io_args[i].queue = &queue;
            io_args[i].thread_id = i;
            io_args[i].tables_done = &tables_done;
            io_args[i].total_tables = num_tables;
            io_args[i].hash2_ready = NULL;
            io_args[i].all_keys_found = &all_keys_found;

            for (int h = 0; h < num_hashes; h++) {
                io_args[i].endpoints[h] = hashes[h].endpoints_sorted;
                io_args[i].num_indices[h] = hashes[h].num_indices;
            }

            pthread_create(&io_threads_arr[i], NULL, streaming_lookup_worker, &io_args[i]);
        }

        /* Wait for I/O threads to finish */
        for (int i = 0; i < io_threads; i++) {
            pthread_join(io_threads_arr[i], NULL);
        }

        /* Signal GPU threads that no more data is coming */
        queue_finish(&queue);

        /* Wait for all GPU threads to finish */
        for (int g = 0; g < actual_gpus; g++) {
            pthread_join(gpu_threads[g], NULL);
        }

        free(io_threads_arr);
        free(io_args);
        queue_destroy(&queue);

        /* Count total candidates found (approximate from verified) */
        for (int h = 0; h < num_hashes; h++) {
            hashes[h].num_candidates = verified_count / num_hashes;  /* Approximate */
        }

        format_time(get_time_sec() - step_start, time_buf, sizeof(time_buf));
        printf("  Streaming lookup + verification complete [%s]\n", time_buf);
        printf("  Verified: %u candidates\n\n", verified_count);
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"scan_done\",\"timestamp\":\"%s\",\"tables_scanned\":%d,"
                 "\"verified_candidates\":%u,\"elapsed_sec\":%.1f,\"early_exit\":%s}",
                 ts, num_tables, verified_count, get_time_sec() - step_start,
                 all_keys_found ? "true" : "false");
        }

        /* Skip separate verification phase - already done */
        goto summary;
    } else
#endif
    if (io_threads > 1 && num_tables > 1) {
        /* ---- BATCH MODE (original multi-threaded) ---- */
        if (io_threads > num_tables) io_threads = num_tables;

        /* Allocate candidate storage for each hash */
        for (int h = 0; h < num_hashes; h++) {
            hashes[h].candidate_starts = malloc(MAX_TOTAL_CANDIDATES * sizeof(uint64_t));
            hashes[h].candidate_positions = malloc(MAX_TOTAL_CANDIDATES * sizeof(uint32_t));
            hashes[h].num_candidates = 0;
        }

#ifndef _WIN32
        pthread_t *threads = malloc(io_threads * sizeof(pthread_t));
        lookup_thread_arg_t *args = malloc(io_threads * sizeof(lookup_thread_arg_t));
        volatile int tables_done = 0;

        int tables_per_thread = num_tables / io_threads;
        int remainder = num_tables % io_threads;
        int offset = 0;
        uint32_t candidates_per_thread = MAX_TOTAL_CANDIDATES / io_threads;

        for (int i = 0; i < io_threads; i++) {
            args[i].table_paths = table_paths;
            args[i].table_start = offset;
            int count = tables_per_thread + (i < remainder ? 1 : 0);
            args[i].table_end = offset + count;
            offset += count;
            args[i].num_hashes = num_hashes;
            args[i].thread_id = i;
            args[i].tables_done = &tables_done;
            args[i].total_tables = num_tables;

            for (int h = 0; h < num_hashes; h++) {
                args[i].endpoints[h] = hashes[h].endpoints_sorted;
                args[i].num_indices[h] = hashes[h].num_indices;
                args[i].candidates[h].capacity = candidates_per_thread;
                args[i].candidates[h].count = 0;
                args[i].candidates[h].start_indices = malloc(candidates_per_thread * sizeof(uint64_t));
                args[i].candidates[h].positions = malloc(candidates_per_thread * sizeof(uint32_t));
            }

            pthread_create(&threads[i], NULL, lookup_worker, &args[i]);
        }

        /* Collect results from all threads */
        for (int i = 0; i < io_threads; i++) {
            pthread_join(threads[i], NULL);

            for (int h = 0; h < num_hashes; h++) {
                uint32_t n = args[i].candidates[h].count;
                if (hashes[h].num_candidates + n > MAX_TOTAL_CANDIDATES)
                    n = MAX_TOTAL_CANDIDATES - hashes[h].num_candidates;

                if (n > 0) {
                    memcpy(hashes[h].candidate_starts + hashes[h].num_candidates,
                           args[i].candidates[h].start_indices, n * sizeof(uint64_t));
                    memcpy(hashes[h].candidate_positions + hashes[h].num_candidates,
                           args[i].candidates[h].positions, n * sizeof(uint32_t));
                    hashes[h].num_candidates += n;
                }

                free(args[i].candidates[h].start_indices);
                free(args[i].candidates[h].positions);
            }
        }

        free(threads);
        free(args);
#endif
    } else {
        /* ---- SINGLE-THREADED FALLBACK ---- */
        for (int h = 0; h < num_hashes; h++) {
            hashes[h].candidate_starts = malloc(MAX_TOTAL_CANDIDATES * sizeof(uint64_t));
            hashes[h].candidate_positions = malloc(MAX_TOTAL_CANDIDATES * sizeof(uint32_t));
            hashes[h].num_candidates = 0;
        }

        uint64_t *local_starts = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint64_t));
        uint32_t *local_pos = malloc(MAX_CANDIDATES_PER_TABLE * sizeof(uint32_t));

        for (int t = 0; t < num_tables; t++) {
            rt_table table = {0};
            if (table_load(&table, table_paths[t]) != 0) continue;

            for (int h = 0; h < num_hashes; h++) {
                if (hashes[h].num_candidates >= MAX_TOTAL_CANDIDATES) continue;

                uint32_t found = search_single_table(&table, hashes[h].endpoints_sorted,
                                                      hashes[h].num_indices, local_starts, local_pos);
                if (found > 0) {
                    uint32_t space = MAX_TOTAL_CANDIDATES - hashes[h].num_candidates;
                    if (found > space) found = space;
                    memcpy(hashes[h].candidate_starts + hashes[h].num_candidates,
                           local_starts, found * sizeof(uint64_t));
                    memcpy(hashes[h].candidate_positions + hashes[h].num_candidates,
                           local_pos, found * sizeof(uint32_t));
                    hashes[h].num_candidates += found;
                }
            }

            table_free(&table);

            if ((t + 1) % 100 == 0 || t + 1 == num_tables) {
                uint32_t total_cands = 0;
                for (int h = 0; h < num_hashes; h++) total_cands += hashes[h].num_candidates;
                fprintf(stderr, "  Progress: %d/%d tables (%.1f%%) - %u total candidates\n",
                        t + 1, num_tables, 100.0 * (t + 1) / num_tables, total_cands);
            }
        }

        free(local_starts);
        free(local_pos);
    }

    /* Free sorted endpoints (careful with shared pointers for identical hashes) */
    if (identical_hashes) {
        free(hashes[0].endpoints_sorted);
    } else {
        for (int h = 0; h < num_hashes; h++)
            free(hashes[h].endpoints_sorted);
    }

    format_time(get_time_sec() - step_start, time_buf, sizeof(time_buf));
    printf("  Table lookup complete [%s]\n", time_buf);
    for (int h = 0; h < num_hashes; h++)
        printf("  Hash %d: %u candidates\n", h + 1, hashes[h].num_candidates);
    printf("\n");

    /* ---- Phase 4: Verification for each hash (batch mode only) ---- */
    printf("[%s] [4/4] Verifying candidates on GPU (%d GPU%s)...\n", timestamp(), actual_gpus, actual_gpus > 1 ? "s" : "");
    step_start = get_time_sec();

    int any_found = 0;
    for (int h = 0; h < num_hashes; h++) {
        if (hashes[h].num_candidates == 0) {
            printf("  Hash %d (%s): no candidates to verify\n", h + 1, hashes[h].ct_hex);
            continue;
        }

        double verify_start = get_time_sec();

#ifndef _WIN32
        if (actual_gpus > 1) {
            /* Multi-GPU verification: split candidates across GPUs */
            pthread_t vthreads[MAX_GPUS];
            verify_thread_arg_t vargs[MAX_GPUS];
            uint32_t per_gpu = hashes[h].num_candidates / actual_gpus;
            uint32_t voffset = 0;

            for (int g = 0; g < actual_gpus; g++) {
                uint32_t cnt = (g == actual_gpus - 1) ? hashes[h].num_candidates - voffset : per_gpu;
                vargs[g].gpu = &gpus[g];
                vargs[g].ciphertext = hashes[h].ciphertext;
                vargs[g].start_indices = hashes[h].candidate_starts + voffset;
                vargs[g].positions = hashes[h].candidate_positions + voffset;
                vargs[g].num_candidates = cnt;
                vargs[g].reduction_offset = REDUCTION_OFFSET;
                vargs[g].plaintext_space = plaintext_space;
                vargs[g].gpu_id = g;
                memset(vargs[g].found_key, 0, 7);
                pthread_create(&vthreads[g], NULL, verify_gpu_worker, &vargs[g]);
                voffset += cnt;
            }

            int found = 0;
            for (int g = 0; g < actual_gpus; g++) {
                pthread_join(vthreads[g], NULL);
                if (vargs[g].result == 1 && !found) {
                    found = 1;
                    memcpy(hashes[h].found_key, vargs[g].found_key, 7);
                }
            }

            format_time(get_time_sec() - verify_start, time_buf, sizeof(time_buf));

            if (found) {
                hashes[h].key_found = 1;
                any_found = 1;
                bytes_to_hex(hashes[h].found_key, 7, hashes[h].found_key_hex, sizeof(hashes[h].found_key_hex));
                printf("  Hash %d (%s): KEY FOUND -> %s [%s]\n",
                       h + 1, hashes[h].ct_hex, hashes[h].found_key_hex, time_buf);
            } else {
                printf("  Hash %d (%s): no match [%s]\n", h + 1, hashes[h].ct_hex, time_buf);
            }
        } else
#endif
        {
            int result = gpu_check_false_alarms(&gpus[0], hashes[h].ciphertext,
                                                 hashes[h].candidate_starts, hashes[h].candidate_positions,
                                                 hashes[h].num_candidates, REDUCTION_OFFSET,
                                                 plaintext_space, hashes[h].found_key);

            format_time(get_time_sec() - verify_start, time_buf, sizeof(time_buf));

            if (result == 1) {
                hashes[h].key_found = 1;
                any_found = 1;
                bytes_to_hex(hashes[h].found_key, 7, hashes[h].found_key_hex, sizeof(hashes[h].found_key_hex));
                printf("  Hash %d (%s): KEY FOUND -> %s [%s]\n",
                       h + 1, hashes[h].ct_hex, hashes[h].found_key_hex, time_buf);
            } else {
                printf("  Hash %d (%s): no match [%s]\n", h + 1, hashes[h].ct_hex, time_buf);
            }
        }
    }

    format_time(get_time_sec() - step_start, time_buf, sizeof(time_buf));
    printf("  Total verification [%s]\n\n", time_buf);

summary:
    /* ---- Summary ---- */
    ;
    double total_time = get_time_sec() - total_start;
    format_time(total_time, time_buf, sizeof(time_buf));

    any_found = 0;
    for (int h = 0; h < num_hashes; h++)
        if (hashes[h].key_found) any_found = 1;

    /* ---- NetNTLMv1 end-to-end: brute-force CT3 and reconstruct NTLM hash ---- */
    uint8_t ct3_key[2] = {0};
    int ct3_found = 0;
    char ntlm_hash_hex[33] = {0};

    if (ntlmv1_mode && any_found) {
        printf("[%s] [CT3] Brute-forcing CT3 (%s) — 65536 DES keys...\n", timestamp(), ct3_hex);
        double ct3_start = get_time_sec();
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"ct3_start\",\"timestamp\":\"%s\",\"ct3_hex\":\"%s\"}",
                 ts, ct3_hex);
        }

        ct3_found = bruteforce_ct3(ct3_bytes, ct3_key);

        format_time(get_time_sec() - ct3_start, time_buf, sizeof(time_buf));
        if (ct3_found) {
            printf("  CT3 key found: %02x%02x [%s]\n\n", ct3_key[0], ct3_key[1], time_buf);
        } else {
            printf("  CT3 key NOT found [%s]\n\n", time_buf);
        }
        {
            char ts[64];
            json_timestamp(ts, sizeof(ts));
            jlog("{\"event\":\"ct3_done\",\"timestamp\":\"%s\",\"found\":%s,"
                 "\"key_hex\":\"%02x%02x\",\"elapsed_sec\":%.2f}",
                 ts, ct3_found ? "true" : "false",
                 ct3_key[0], ct3_key[1], get_time_sec() - ct3_start);
        }

        /* Reconstruct NTLM hash: K1[7] || K2[7] || CT3_key[2] = 16 bytes */
        if (hashes[0].key_found && hashes[1].key_found && ct3_found) {
            uint8_t ntlm_hash[16];
            memcpy(ntlm_hash, hashes[0].found_key, 7);       /* bytes 0-6 */
            memcpy(ntlm_hash + 7, hashes[1].found_key, 7);   /* bytes 7-13 */
            ntlm_hash[14] = ct3_key[0];                       /* byte 14 */
            ntlm_hash[15] = ct3_key[1];                       /* byte 15 */
            bytes_to_hex(ntlm_hash, 16, ntlm_hash_hex, sizeof(ntlm_hash_hex));
        }
    }

    /* ---- Final output ---- */
    total_time = get_time_sec() - total_start;
    format_time(total_time, time_buf, sizeof(time_buf));

    printf("============================================\n");
    if (ntlmv1_mode) {
        printf("  NetNTLMv1 End-to-End Results\n");
        printf("--------------------------------------------\n");
        for (int h = 0; h < num_hashes; h++) {
            if (hashes[h].key_found) {
                printf("  CT%d: %s -> K%d = %s\n", h + 1, hashes[h].ct_hex, h + 1, hashes[h].found_key_hex);
            } else {
                printf("  CT%d: %s -> NOT FOUND\n", h + 1, hashes[h].ct_hex);
            }
        }
        if (ct3_found) {
            printf("  CT3: %s -> K3 = %02x%02x000000000000\n", ct3_hex, ct3_key[0], ct3_key[1]);
        } else {
            printf("  CT3: %s -> NOT FOUND\n", ct3_hex);
        }
        printf("--------------------------------------------\n");
        if (ntlm_hash_hex[0]) {
            printf("  NTLM Hash: %s\n", ntlm_hash_hex);
        } else if (hashes[0].key_found && hashes[1].key_found && !ct3_found) {
            printf("  NTLM Hash: INCOMPLETE (CT3 not found)\n");
        } else {
            printf("  NTLM Hash: INCOMPLETE (missing DES keys)\n");
        }
    } else {
        for (int h = 0; h < num_hashes; h++) {
            if (hashes[h].key_found) {
                printf("  HASH %d: KEY FOUND\n", h + 1);
                printf("    Ciphertext: %s\n", hashes[h].ct_hex);
                printf("    DES Key:    %s\n", hashes[h].found_key_hex);
            } else {
                printf("  HASH %d: KEY NOT FOUND\n", h + 1);
                printf("    Ciphertext: %s\n", hashes[h].ct_hex);
            }
            printf("    Candidates: %u\n", hashes[h].num_candidates);
        }
    }
    printf("  Tables:     %d\n", num_tables);
    printf("  Total time: %s\n", time_buf);
    printf("============================================\n");

    /* JSON log: result event */
    {
        char ts[64];
        json_timestamp(ts, sizeof(ts));
        int success = ntlmv1_mode ? (ntlm_hash_hex[0] != '\0') : any_found;
        /* Build keys JSON array */
        char keys_buf[256];
        int kpos = 0;
        kpos += snprintf(keys_buf + kpos, sizeof(keys_buf) - kpos, "[");
        for (int h = 0; h < num_hashes; h++) {
            if (h > 0) kpos += snprintf(keys_buf + kpos, sizeof(keys_buf) - kpos, ",");
            kpos += snprintf(keys_buf + kpos, sizeof(keys_buf) - kpos, "\"%s\"",
                             hashes[h].key_found ? hashes[h].found_key_hex : "");
        }
        if (ntlmv1_mode && ct3_found) {
            kpos += snprintf(keys_buf + kpos, sizeof(keys_buf) - kpos, ",\"%02x%02x\"",
                             ct3_key[0], ct3_key[1]);
        }
        snprintf(keys_buf + kpos, sizeof(keys_buf) - kpos, "]");

        jlog("{\"event\":\"result\",\"timestamp\":\"%s\",\"success\":%s,"
             "\"ntlm_hash\":\"%s\",\"keys\":%s,\"total_sec\":%.1f}",
             ts, success ? "true" : "false",
             ntlm_hash_hex, keys_buf, total_time);
    }

    /* Cleanup */
    if (!streaming_mode) {
        for (int h = 0; h < num_hashes; h++) {
            free(hashes[h].candidate_starts);
            free(hashes[h].candidate_positions);
        }
    }
    for (int i = 0; i < num_tables; i++) free(table_paths[i]);
    free(table_paths);
    for (int g = 0; g < actual_gpus; g++) gpu_cleanup(&gpus[g]);
    if (json_log_fp) fclose(json_log_fp);

    if (ntlmv1_mode)
        return ntlm_hash_hex[0] ? 0 : 1;
    return any_found ? 0 : 1;
}
