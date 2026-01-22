#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "utils.h"
#include "opencl_host.h"

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

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <ciphertext_hex> <candidates_file> [-o output_file] [-g gpu_index]\n", argv[0]);
        fprintf(stderr, "\nAvailable GPUs:\n");
        gpu_list();
        return 1;
    }

    const char *ct_hex = argv[1];
    const char *candidates_file = argv[2];
    
    /* Default output: {ct_hex}.result */
    char default_output[256];
    snprintf(default_output, sizeof(default_output), "%s.result", ct_hex);
    const char *output_file = default_output;
    int gpu_index = 0;
    
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gpu_index = atoi(argv[++i]);
        }
    }

    uint8_t ciphertext[8];
    if (hex_to_bytes(ct_hex, ciphertext, 8) != 8) {
        fail(output_file, "Invalid ciphertext (need 16 hex chars)");
        return 1;
    }

    uint64_t *start_indices = NULL;
    uint32_t *positions = NULL;
    uint32_t total = 0;

    if (load_candidates(candidates_file, &start_indices, &positions, &total) != 0) {
        fail(output_file, "Failed to load candidates");
        return 1;
    }

    gpu_context gpu = {0};
    if (gpu_init_index(&gpu, gpu_index) != 0) {
        fail(output_file, "GPU init failed");
        free(start_indices);
        free(positions);
        return 1;
    }
    printf("STATUS GPU %d: %s (%u CUs)\n", gpu_index, gpu.device_name, gpu.compute_units);

    if (gpu_load_false_alarm_kernel(&gpu, "kernels" PATH_SEP_STR "false_alarm.cl") != 0) {
        fail(output_file, "Kernel load failed");
        gpu_cleanup(&gpu);
        free(start_indices);
        free(positions);
        return 1;
    }

    printf("STATUS Checking %u candidates for %s...\n", total, ct_hex);
    uint64_t plaintext_space = get_plaintext_space();
    uint8_t key_bytes[7] = {0};

    int result = gpu_check_false_alarms(&gpu, ciphertext, start_indices, positions,
                                         total, REDUCTION_OFFSET, plaintext_space, key_bytes);

    gpu_cleanup(&gpu);
    free(start_indices);
    free(positions);

    if (result < 0) {
        fail(output_file, "False alarm check failed");
        return 1;
    }

    FILE *f = fopen(output_file, "w");
    if (!f) {
        fail(output_file, "Failed to write result file");
        return 1;
    }

    if (result == 1) {
        char key_hex[15];
        bytes_to_hex(key_bytes, 7, key_hex, sizeof(key_hex));
        fprintf(f, "%s\n", key_hex);
        fclose(f);
        printf("FOUND %s\n", key_hex);
    } else {
        fprintf(f, "NOTFOUND\n");
        fclose(f);
        printf("NOTFOUND\n");
    }
    
    return 0;
}