#ifndef TABLE_H
#define TABLE_H

#include <stdint.h>

#define CHAIN_LEN 881689

typedef struct {
    uint64_t *data;        // [start0][end0][start1][end1]...
    uint64_t num_chains;
    size_t file_size;
} rt_table;

int table_load(rt_table *table, const char *filename);
void table_free(rt_table *table);
uint64_t table_search(rt_table *table, uint64_t end_index, int *found);

#endif