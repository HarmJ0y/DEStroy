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

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <ciphertext_hex> [work_dir] [gpu_index]\n", argv[0]);
        fprintf(stderr, "\nAvailable GPUs:\n");
        gpu_list();
        return 1;
    }

    const char *ct_hex = argv[1];
    const char *work_dir = argc > 2 ? argv[2] : "working";
    int gpu_index = argc > 3 ? atoi(argv[3]) : 0;

    uint8_t ciphertext[8];
    if (hex_to_bytes(ct_hex, ciphertext, 8) != 8) {
        fprintf(stderr, "ERROR Invalid ciphertext\n");
        write_error(work_dir, ct_hex, "Invalid ciphertext");
        return 1;
    }

    printf("STATUS Initializing GPU %d...\n", gpu_index);
    gpu_context gpu = {0};
    if (gpu_init_index(&gpu, gpu_index) != 0) {
        fprintf(stderr, "ERROR GPU init failed\n");
        write_error(work_dir, ct_hex, "GPU init failed");
        return 1;
    }

    printf("STATUS Loading kernel...\n");
    if (gpu_load_kernel(&gpu, "kernels/precompute.cl", "precompute") != 0) {
        fprintf(stderr, "ERROR Kernel load failed\n");
        write_error(work_dir, ct_hex, "Kernel load failed");
        gpu_cleanup(&gpu);
        return 1;
    }

    uint32_t num_indices = CHAIN_LEN - 1;
    uint64_t *end_indices = malloc(num_indices * sizeof(uint64_t));
    if (!end_indices) {
        fprintf(stderr, "ERROR malloc failed\n");
        write_error(work_dir, ct_hex, "malloc failed");
        gpu_cleanup(&gpu);
        return 1;
    }

    printf("STATUS Computing %u endpoints...\n", num_indices);
    uint64_t plaintext_space = get_plaintext_space();
    int result = gpu_precompute(&gpu, ciphertext, CHAIN_LEN, REDUCTION_OFFSET,
                                 plaintext_space, end_indices);

    if (result < 0) {
        fprintf(stderr, "ERROR Precompute failed\n");
        write_error(work_dir, ct_hex, "Precompute failed");
        free(end_indices);
        gpu_cleanup(&gpu);
        return 1;
    }

    printf("STATUS Saving endpoints...\n");
    if (save_endpoints_to(work_dir, ct_hex, end_indices, num_indices) != 0) {
        fprintf(stderr, "ERROR Save failed\n");
        write_error(work_dir, ct_hex, "Save failed");
        free(end_indices);
        gpu_cleanup(&gpu);
        return 1;
    }

    printf("DONE %u\n", num_indices);

    free(end_indices);
    gpu_cleanup(&gpu);
    return 0;
}