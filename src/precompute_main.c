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
        fprintf(stderr, "Usage: %s <ciphertext_hex> <output_file> [-g gpu_index]\n", argv[0]);
        fprintf(stderr, "\nAvailable GPUs:\n");
        gpu_list();
        return 1;
    }

    const char *ct_hex = argv[1];
    const char *output_file = argv[2];
    int gpu_index = 0;
    
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gpu_index = atoi(argv[++i]);
        }
    }

    uint8_t ciphertext[8];
    if (hex_to_bytes(ct_hex, ciphertext, 8) != 8) {
        fail(output_file, "Invalid ciphertext: must be exactly 16 hex characters (8 bytes)");
        return 1;
    }

    gpu_context gpu = {0};
    if (gpu_init_index(&gpu, gpu_index) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Failed to initialize GPU %d", gpu_index);
        fail(output_file, msg);
        return 1;
    }
    printf("STATUS GPU %d: %s (%u CUs)\n", gpu_index, gpu.device_name, gpu.compute_units);

    if (gpu_load_kernel(&gpu, "kernels" PATH_SEP_STR "precompute.cl", "precompute") != 0) {
        fail(output_file, "Failed to compile precompute kernel");
        gpu_cleanup(&gpu);
        return 1;
    }

    uint32_t num_indices = CHAIN_LEN - 1;
    uint64_t *end_indices = malloc(num_indices * sizeof(uint64_t));
    if (!end_indices) {
        fail(output_file, "Out of memory");
        gpu_cleanup(&gpu);
        return 1;
    }

    uint64_t plaintext_space = get_plaintext_space();
    printf("STATUS Computing %u endpoints for %s...\n", num_indices, ct_hex);
    
    int result = gpu_precompute_chunked(&gpu, ciphertext, CHAIN_LEN, REDUCTION_OFFSET,
                                         plaintext_space, end_indices);

    if (result < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "GPU precomputation failed (error %d)", result);
        fail(output_file, msg);
        free(end_indices);
        gpu_cleanup(&gpu);
        return 1;
    }

    if (save_endpoints(output_file, end_indices, num_indices) != 0) {
        fail(output_file, "Failed to save endpoints");
        free(end_indices);
        gpu_cleanup(&gpu);
        return 1;
    }

    printf("DONE %u endpoints saved to %s\n", num_indices, output_file);

    free(end_indices);
    gpu_cleanup(&gpu);
    return 0;
}