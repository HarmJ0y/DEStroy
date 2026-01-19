#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"
#include "opencl_host.h"

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

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <candidates_file> <work_dir> [gpu_index]\n", argv[0]);
        fprintf(stderr, "\nAvailable GPUs:\n");
        gpu_list();
        return 1;
    }

    const char *candidates_file = argv[1];
    const char *work_dir = argv[2];
    int gpu_index = argc > 3 ? atoi(argv[3]) : 0;
    const char *ct_hex = extract_ct_hex(candidates_file);

    uint8_t ciphertext[8];
    if (hex_to_bytes(ct_hex, ciphertext, 8) != 8) {
        fprintf(stderr, "ERROR Invalid ciphertext from filename\n");
        write_error(work_dir, ct_hex, "Invalid ciphertext from filename");
        return 1;
    }

    uint64_t *start_indices = NULL;
    uint32_t *positions = NULL;
    uint32_t total = 0;

    if (load_candidates(candidates_file, &start_indices, &positions, &total) != 0) {
        fprintf(stderr, "ERROR Failed to load candidates: %s\n", candidates_file);
        write_error(work_dir, ct_hex, "Failed to load candidates");
        return 1;
    }

    printf("STATUS Loaded %u candidates for %s\n", total, ct_hex);

    printf("STATUS Initializing GPU %d...\n", gpu_index);
    gpu_context gpu = {0};
    if (gpu_init_index(&gpu, gpu_index) != 0) {
        fprintf(stderr, "ERROR GPU init failed\n");
        write_error(work_dir, ct_hex, "GPU init failed");
        free(start_indices);
        free(positions);
        return 1;
    }

    printf("STATUS Loading kernel...\n");
    if (gpu_load_false_alarm_kernel(&gpu, "kernels/false_alarm.cl") != 0) {
        fprintf(stderr, "ERROR Kernel load failed\n");
        write_error(work_dir, ct_hex, "Kernel load failed");
        gpu_cleanup(&gpu);
        free(start_indices);
        free(positions);
        return 1;
    }

    printf("STATUS Checking %u candidates...\n", total);
    uint64_t plaintext_space = get_plaintext_space();
    uint8_t key_bytes[7] = {0};

    int result = gpu_check_false_alarms(&gpu, ciphertext, start_indices, positions,
                                         total, REDUCTION_OFFSET, plaintext_space, key_bytes);

    gpu_cleanup(&gpu);
    free(start_indices);
    free(positions);

    if (result < 0) {
        fprintf(stderr, "ERROR False alarm check failed\n");
        write_error(work_dir, ct_hex, "False alarm check failed");
        return 1;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.result", work_dir, ct_hex);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "ERROR Failed to write result file\n");
        write_error(work_dir, ct_hex, "Failed to write result file");
        return 1;
    }

    if (result == 1) {
        char key_hex[15];
        bytes_to_hex(key_bytes, 7, key_hex, sizeof(key_hex));
        fprintf(f, "%s\n", key_hex);
        fclose(f);
        printf("FOUND %s\n", key_hex);
        return 0;
    } else {
        fprintf(f, "NOTFOUND\n");
        fclose(f);
        printf("NOTFOUND\n");
        return 0;
    }
}