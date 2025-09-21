#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "jmalloc.h"

typedef struct { int id; char name[16]; } Item;

static void stats(const char* tag){
    printf("[%s] heap=%zuB free=%zuB\n", tag, j_heap_bytes(), j_free_bytes());
}

int main(void) {
    stats("start");

    // 1) save a string
    char* s = (char*)j_malloc(16);
    strcpy(s, "hello");
    printf("s=%p -> %s\n", (void*)s, s);

    // realloc to expand
    s = (char*)j_realloc(s, 64);
    strcat(s, " allocator!");
    printf("after realloc: s=%p -> %s\n", (void*)s, s);
    stats("after string");

    // 2) emulate dynamic vector (push back)
    size_t cap = 4, len = 0;
    Item* arr = (Item*)j_malloc(cap * sizeof(Item));
    for (int i = 0; i < 10; ++i) {
        if (len == cap) {
            cap *= 2;
            arr = (Item*)j_realloc(arr, cap * sizeof(Item));
        }
        arr[len].id = i;
        snprintf(arr[len].name, sizeof(arr[len].name), "I%02d", i);
        len++;
    }

    printf("vector size=%zu cap=%zu first={%d,%s} last={%d,%s}\n",
           len, cap, arr[0].id, arr[0].name, arr[len-1].id, arr[len-1].name);
    stats("after vector");

    // 3) coalesce
    void* a = j_malloc(128);
    void* b = j_malloc(128);
    void* c = j_malloc(128);
    j_free(b);
    j_free(a); 
    j_free(c);             
    stats("after coalesce trio");

    // 4) cleanup
    j_free(arr);
    j_free(s);
    stats("end");
    return 0;
}
