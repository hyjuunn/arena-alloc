#include "jmalloc.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

// platform abstraction
#if defined(_WIN32)
    #include <windows.h>
    // Os internally manages memory in page units (page is the minimum unit of allocation). usually 4KB for x86/x64. 
    static size_t os_pagesize() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        size_t ps = (size_t)si.dwPageSize;
        if (ps == 0) ps = 4096u;
        return ps;
    }
    static void* os_alloc(size_t n) {
        // round up to nearest multiple of ps
        size_t ps = os_pagesize();
        // need = ceil(n / ps) * ps
        // n & ~(ps - 1) -> floor
        // (n + ps - 1) & ~(ps - 1) -> ceil
        size_t need = (n + ps - 1) & ~(ps - 1);
        // VirtualAlloc(memory_address, size (has to be multiple of page size), allocation_type, protection_flags)
        // NULL if it fails
        void* p = VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return p;
    }
    // free memory allocated with VirtualAlloc
    static int os_free(void* p, size_t n) {
        // n is not needed but kept for interface consistency
        (void)n;
        // VirtualFree(memory_address, size (0 means free the entire region), free_type)
        return VirtualFree(p, 0, MEM_RELEASE) ? 0 : -1;
    }
    // if not windows (linux, macos, etc)
    #else
    #include <unistd.h>
    #include <sys/mman.h>
    static size_t os_pagesize() {
        long ps = sysconf(_SC_PAGESIZE);
        // linux returns -1 on error
        if (ps <= 0) ps = 4096u;
        return (size_t)ps;
    }
    static void* os_alloc(size_t n) {
        size_t ps = os_pagesize();
        size_t need = (n + ps - 1) & ~(ps - 1);
        // mmap(addr, length, protection flag, flags, fd, offset)
        void* p = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return p == MAP_FAILED ? NULL : p;
    }
    static int os_free(void* p, size_t n) {
        size_t ps = os_pagesize();
        size_t need = (n + ps - 1) & ~(ps - 1);
        return munmap(p, need);
    }
#endif

// allocator core11
// every block to 8 bytes
#define ALIGNMENT 8UL
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

// To reduce OS calls, request memory by arenas (>= 1 MiB)
#define ARENA_MIN_SIZE (1u << 20) // 1 MiB

static arena_header_t *g_arenas = NULL;
static block_header_t *g_head = NULL; // global block list (across arenas)
static block_header_t *g_tail = NULL;
static size_t g_total_bytes = 0;
static size_t g_free_bytes  = 0;

// return header sizes aligned to ALIGNMENT
static inline size_t header_size(void) { 
    return ALIGN_UP(sizeof(block_header_t), ALIGNMENT); 
}
static inline size_t arena_header_size(void) { 
    return ALIGN_UP(sizeof(arena_header_t), ALIGNMENT); 
}

// helpers
static void split_block(block_header_t *blk, size_t want);
static block_header_t* coalesce(block_header_t *blk);
static block_header_t* find_first_fit(size_t size);
static block_header_t* request_space(size_t size);

// api
// main malloc function
void *j_malloc(size_t size) {
    if (size == 0) return NULL;
    size = ALIGN_UP(size, ALIGNMENT);

    block_header_t *blk = find_first_fit(size);
    // first fit not found
    if (!blk) {
        blk = request_space(size);
        if (!blk) return NULL;
    } 
    // found
    else {
        size_t old_size = blk->size;
        if (old_size >= size + header_size() + ALIGNMENT) {
            split_block(blk, size);
        }
        blk->free = 0;
        // reduce free bytes count
        g_free_bytes -= old_size;
    }

    // return pointer to payload (after header)
    return (void*)((uint8_t*)blk + header_size());
}

// free function
void j_free(void *ptr) {
    // if null pointer, do nothing
    if (!ptr) return;
    // payload pointer ptr -> block header pointer blk
    block_header_t *blk = (block_header_t*)((uint8_t*)ptr - header_size());
    //if already free, do nothing
    if (blk->free) return;
    blk->free = 1;
    // increase free bytes count
    g_free_bytes += blk->size;
    // try to merge with adjacent free blocks
    coalesce(blk);
}

// realloc function
// realloc is to resize an allocated memory block
void *j_realloc(void *ptr, size_t new_size) {
    // if ptr is NULL, behave like malloc
    if (!ptr) return j_malloc(new_size);
    // if new_size is 0, behave like free and return NULL
    if (new_size == 0) { 
        j_free(ptr);
        return NULL; 
    }

    // align new_size
    new_size = ALIGN_UP(new_size, ALIGNMENT);
    // get block header from payload pointer
    block_header_t *blk = (block_header_t*)((uint8_t*)ptr - header_size());
    size_t old_size = blk->size;

    // if the current block is large enough
    // split if there is enough space left
    if (old_size >= new_size) {
        if (old_size >= new_size + header_size() + ALIGNMENT) {
            split_block(blk, new_size);
        }
        return ptr;
    }

    // if the next block exists and is free, and if merging with it can satisfy new_size
    if (blk->next && blk->next->free && (old_size + header_size() + blk->next->size) >= new_size) {
        block_header_t *n = blk->next;
        // merge sizes
        blk->size += header_size() + n->size;
        blk->next = n->next;
        if (blk->next) blk->next->prev = blk; 
        else g_tail = blk;
        g_free_bytes -= n->size;
        // still have extra space, split
        if (blk->size >= new_size + header_size() + ALIGNMENT) {
            split_block(blk, new_size);
        }
        return (void*)((uint8_t*)blk + header_size());
    }

    // otherwise, need to allocate a new block
    void *new_ptr = j_malloc(new_size);
    if (!new_ptr) return NULL;
    // data copy
    // memcpy(dest, src, n)
    size_t keep = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, keep);
    // free old block
    j_free(ptr);
    return new_ptr;
}

size_t j_heap_bytes() { 
    return g_total_bytes; 
}
size_t j_free_bytes() {
    size_t sum = 0;
    for (block_header_t *cur = g_head; cur; cur = cur->next) {
        if (cur->free) sum += cur->size;
    }
    return sum;
}

// helpers implementation
static block_header_t* find_first_fit(size_t size) {
    // find first fit block in global list
    for (block_header_t *cur = g_head; cur; cur = cur->next) {
        // if there is a free block (free = 1) and if its size is larger than or equal to the required size, return the pointer
        if (cur->free && cur->size >= size) return cur;
    }
    // if no fit -> NULL
    return NULL;
}

static block_header_t* request_space(size_t size) {
    // allocate at least ARENA_MIN_SIZE to reduce OS calls
    size_t need = header_size() + size;
    // if need is larger than ARENA_MIN_SIZE, allocate need; otherwise allocate ARENA_MIN_SIZE (+ arena header size)
    size_t arena_total = arena_header_size() + (need > ARENA_MIN_SIZE ? need : ARENA_MIN_SIZE);

    // ask OS for memory
    void* mem = os_alloc(arena_total);
    if (!mem) return NULL;

    // set up arena header
    // [mem start] -> [arena_header][block_header_t first block][...]
    arena_header_t* a = (arena_header_t*)mem;
    a->size = arena_total;
    a->prev = NULL;
    // first arena in the global arena list assigned to a->next
    // then, the first arena in the g_arenas will get a as prev arena
    // then, set current g_arenas to a (now first)
    a->next = g_arenas;
    if (g_arenas) g_arenas->prev = a;
    g_arenas = a;

    // first block placed right after arena header; if we reserved a big arena, split to leave a trailing free block
    uint8_t* base = (uint8_t*)mem + arena_header_size();
    block_header_t* blk = (block_header_t*)base;
    blk->prev = g_tail;
    blk->next = NULL;
    blk->free = 0;
    blk->size = size;

    if (!g_head) g_head = blk;
    if (g_tail) g_tail->next = blk;
    g_tail = blk;

    a->first_block = blk;

    g_total_bytes += arena_total;

    // if there is extra space, create a trailing free block
    // used memory = arena + block header + payload
    size_t used = arena_header_size() + header_size() + size;
    // check if one block header + 8 bytes payload can fit in the remaining space
    if (arena_total >= used + header_size() + ALIGNMENT) {
        // create a free block
        uint8_t* faddr = (uint8_t*)blk + header_size() + size;
        block_header_t* f = (block_header_t*)faddr;
        f->size = (arena_total - used) - header_size();
        f->free = 1;
        f->prev = blk;
        f->next = blk->next;
        // if there is a next block, update its prev pointer
        if (f->next) f->next->prev = f;
        else g_tail = f;
        blk->next = f;
        g_free_bytes += f->size;
    }

    return blk;
}

static void split_block(block_header_t *blk, size_t req_size) {
    // split blk into two blocks
    // first should follow req_size, second follows the remaining
    size_t remain = blk->size - req_size;
    blk->size = req_size;

    // new block address = current block address + header size + req_size
    uint8_t *new_addr = (uint8_t*)blk + header_size() + req_size;
    // new block setup
    block_header_t *n = (block_header_t*)new_addr;
    // payload size = remaining - header size
    n->size = remain - header_size();
    n->free = 1;
    n->prev = blk;
    n->next = blk->next;
    // update next block's prev pointer if exists
    if (n->next) n->next->prev = n; 
    else g_tail = n;
    blk->next = n;

    g_free_bytes += n->size;
}

static block_header_t* coalesce(block_header_t *blk) {
    // merge with next if free
    // if there is a next block and if it is free
    if (blk->next && blk->next->free) {
        block_header_t *n = blk->next;
        // merge sizes
        blk->size += header_size() + n->size;
        // update links
        blk->next = n->next;
        // if there is a next block, update its prev pointer
        if (blk->next) blk->next->prev = blk; 
        else g_tail = blk;

        g_free_bytes += header_size();
    }
    // merge with prev if free
    // merge to the previous block, return the previous block pointer
    if (blk->prev && blk->prev->free) {
        block_header_t *p = blk->prev;
        p->size += header_size() + blk->size;
        p->next = blk->next;
        if (p->next) p->next->prev = p; 
        else g_tail = p;
        g_free_bytes += header_size();
        blk = p;
    }
    return blk;
}