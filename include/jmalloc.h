#ifndef JMALLOC_H
#define JMALLOC_H

#include <stddef.h>

// Public API (changed name to j_~ avoid clashing with libc)
void *j_malloc(size_t size);
void  j_free(void *ptr);
void *j_realloc(void *ptr, size_t new_size);

// header for each block
typedef struct block_header {
    size_t size; // payload size
    int free; // 1 if free, 0 if used
    // doubly linked list pointers
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

// metadata for each arena
typedef struct arena_header {
    size_t size; // total bytes in this arena (including headers/blocks)
    struct arena_header *next;
    struct arena_header *prev;
    block_header_t *first_block; // pointer to first block in the arena
} arena_header_t;

// stats
size_t j_heap_bytes();
size_t j_free_bytes();

#endif