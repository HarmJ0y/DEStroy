#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

#define CHAIN_LEN 881689
#define REDUCTION_OFFSET 0
#define CHARSET_LEN 256
#define PLAINTEXT_LEN_MAX 7

// Hex conversion
int hex_to_bytes(const char *hex, uint8_t *bytes, size_t max_bytes);
void bytes_to_hex(const uint8_t *bytes, size_t len, char *hex, size_t hex_size);

// Utility
uint64_t get_plaintext_space(void);

// Endpoints
int save_endpoints_to(const char *dir, const char *ct_hex, uint64_t *end_indices, uint32_t count);
int load_endpoints(const char *filepath, uint64_t *end_indices, uint32_t *count);

// Candidates
int append_candidates_to(const char *dir, const char *ct_hex,
                         uint64_t *start_indices, uint32_t *positions, uint32_t count);
int load_candidates(const char *filepath, uint64_t **start_indices, uint32_t **positions, uint32_t *count);

#endif