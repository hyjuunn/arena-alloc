#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "jmalloc.h"

#ifndef ALIGNMENT
#define ALIGNMENT 8
#endif

#define N_ALLOC      50000
#define MAX_SZ       1024
#define REALLOC_RATE 30    // % of live blocks to realloc
#define FREE_RATE    50    // % chance to free in partial free phase
#define CHURN_ITERS  20000 // mixed alloc/free/realloc ops

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "ASSERT FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } \
} while (0)

static int is_aligned(void* p, size_t a) { return ((uintptr_t)p % a) == 0; }

typedef struct {
    void*    p;
    size_t   sz;      // payload size we asked
    uint32_t stamp;   // pattern id
    int      live;
} Slot;

static void fill_pattern(void* p, size_t sz, uint32_t stamp) {
    // 전체를 stamp 바이트로 채우고, 맨앞/맨뒤에 마커를 둠
    unsigned char b = (unsigned char)(stamp & 0xFF);
    memset(p, b, sz);
    if (sz >= 1) ((unsigned char*)p)[0] = 0xAB;
    if (sz >= 2) ((unsigned char*)p)[sz-1] = 0xCD;
}

static int check_pattern(const void* p, size_t sz, uint32_t stamp) {
    if (!p) return 0;
    if (sz >= 1 && ((const unsigned char*)p)[0] != 0xAB) return 0;
    if (sz >= 2 && ((const unsigned char*)p)[sz-1] != 0xCD) return 0;
    unsigned char b = (unsigned char)(stamp & 0xFF);
    for (size_t i = 1; i + 1 < sz; ++i) {
        if (((const unsigned char*)p)[i] != b) return 0;
    }
    return 1;
}

static void print_stats(const char* tag) {
    printf("[%s] heap=%zuB free=%zuB\n", tag, j_heap_bytes(), j_free_bytes());
}

int main(void) {
    srand(42);

    Slot* slots = (Slot*)malloc(sizeof(Slot) * N_ALLOC);
    ASSERT(slots, "host malloc for slots failed");
    memset(slots, 0, sizeof(Slot) * N_ALLOC);

    clock_t t0, t1;
    double ms;

    // PHASE 1: 대량 할당
    t0 = clock();
    size_t live_count = 0, live_bytes = 0;
    for (int i = 0; i < N_ALLOC; ++i) {
        size_t sz = (rand() % MAX_SZ) + 1;
        void* p = j_malloc(sz);
        ASSERT(p, "j_malloc returned NULL");
        ASSERT(is_aligned(p, ALIGNMENT), "pointer not aligned");

        slots[i].p = p;
        slots[i].sz = sz;
        slots[i].stamp = (uint32_t)(i * 2654435761u);
        slots[i].live = 1;

        fill_pattern(p, sz, slots[i].stamp);
        ASSERT(check_pattern(p, sz, slots[i].stamp), "pattern write check failed");

        live_count++;
        live_bytes += sz;
    }
    t1 = clock();
    ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("Phase1 alloc: items=%zu live_bytes=%zu time=%.2fms\n", live_count, live_bytes, ms);
    print_stats("after alloc");

    // PHASE 2: 일부 realloc (grow/shrink 랜덤) + 보존 규칙에 맞춘 검증
    t0 = clock();
    size_t realloc_ok = 0;
    for (int i = 0; i < N_ALLOC; ++i) {
        if (!slots[i].live) continue;
        if ((rand() % 100) < REALLOC_RATE) {
            size_t old_sz = slots[i].sz;
            uint32_t old_stamp = slots[i].stamp;
            ASSERT(check_pattern(slots[i].p, old_sz, old_stamp), "pre-realloc pattern corrupted");

            size_t new_sz = (rand() % 2)
                ? (size_t)(rand() % (MAX_SZ * 4) + 1) // grow
                : (size_t)(rand() % MAX_SZ + 1);      // shrink

            void* np = j_realloc(slots[i].p, new_sz);
            ASSERT(np, "j_realloc returned NULL");
            ASSERT(is_aligned(np, ALIGNMENT), "realloc pointer not aligned");

            // 표준 보존 규칙: 앞에서 min(old,new) 바이트 보존
            size_t keep = old_sz < new_sz ? old_sz : new_sz;
            if (new_sz >= old_sz) {
                // GROW: 옛 전체 영역이 그대로여야 함
                ASSERT(((unsigned char*)np)[0] == 0xAB, "grow: first marker lost");
                if (old_sz >= 2)
                    ASSERT(((unsigned char*)np)[old_sz-1] == 0xCD, "grow: old end marker lost");
            } else {
                // SHRINK: 새 끝은 원래 내부였으므로 0xCD 기대 X
                ASSERT(((unsigned char*)np)[0] == 0xAB, "shrink: first marker lost");
                if (keep >= 3) {
                    unsigned char b = (unsigned char)(old_stamp & 0xFF);
                    for (size_t k = 1; k + 1 < keep; ++k)
                        ASSERT(((unsigned char*)np)[k] == b, "shrink: interior byte changed");
                }
            }

            // 새 패턴으로 덮어써 다음 단계에서도 검증 가능하게 유지
            slots[i].p = np;
            slots[i].sz = new_sz;
            slots[i].stamp ^= 0xA5A5A5A5;
            fill_pattern(np, new_sz, slots[i].stamp);
            ASSERT(check_pattern(np, new_sz, slots[i].stamp), "post-realloc pattern check failed");
            realloc_ok++;
        }
    }
    t1 = clock();
    ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("Phase2 realloc: applied=%zu time=%.2fms\n", realloc_ok, ms);
    print_stats("after realloc batch");

    // PHASE 3: 부분 free
    t0 = clock();
    size_t freed_cnt = 0, freed_bytes = 0;
    for (int i = 0; i < N_ALLOC; ++i) {
        if (!slots[i].live) continue;
        if ((rand() % 100) < FREE_RATE) {
            ASSERT(check_pattern(slots[i].p, slots[i].sz, slots[i].stamp), "pre-free pattern corrupted");
            j_free(slots[i].p);
            slots[i].live = 0;
            freed_cnt++;
            freed_bytes += slots[i].sz;
        }
    }
    t1 = clock();
    ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("Phase3 partial free: freed=%zu bytes=%zu time=%.2fms\n", freed_cnt, freed_bytes, ms);
    print_stats("after partial free");

    // PHASE 4: 혼합 churn(alloc/free/realloc 랜덤)
    t0 = clock();
    size_t churn_ops = 0;
    for (int it = 0; it < CHURN_ITERS; ++it) {
        int i = rand() % N_ALLOC;
        int op = rand() % 3; // 0=alloc,1=free,2=realloc
        if (op == 0) {
            if (!slots[i].live) {
                size_t sz = (rand() % MAX_SZ) + 1;
                void* p = j_malloc(sz);
                if (!p) continue;
                ASSERT(is_aligned(p, ALIGNMENT), "churn alloc not aligned");
                slots[i].p = p;
                slots[i].sz = sz;
                slots[i].stamp = (uint32_t)(i * 1103515245u + it);
                slots[i].live = 1;
                fill_pattern(p, sz, slots[i].stamp);
                ASSERT(check_pattern(p, sz, slots[i].stamp), "churn alloc pattern");
                churn_ops++;
            }
        } else if (op == 1) {
            if (slots[i].live) {
                ASSERT(check_pattern(slots[i].p, slots[i].sz, slots[i].stamp), "churn pre-free pattern");
                j_free(slots[i].p);
                slots[i].live = 0;
                churn_ops++;
            }
        } else {
            if (slots[i].live) {
                size_t new_sz = (rand() % (MAX_SZ * 2)) + 1;
                ASSERT(check_pattern(slots[i].p, slots[i].sz, slots[i].stamp), "churn pre-realloc pattern");
                void* np = j_realloc(slots[i].p, new_sz);
                if (!np) continue;
                ASSERT(is_aligned(np, ALIGNMENT), "churn realloc not aligned");
                slots[i].p = np;
                slots[i].sz = new_sz;
                slots[i].stamp ^= 0x5A5A5A5A;
                fill_pattern(np, new_sz, slots[i].stamp);
                ASSERT(check_pattern(np, new_sz, slots[i].stamp), "churn post-realloc pattern");
                churn_ops++;
            }
        }
    }
    t1 = clock();
    ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("Phase4 churn: ops=%zu time=%.2fms\n", churn_ops, ms);
    print_stats("after churn");

    // PHASE 5: 전부 해제 + 최종 확인
    t0 = clock();
    size_t live_left = 0;
    for (int i = 0; i < N_ALLOC; ++i) {
        if (slots[i].live) {
            ASSERT(check_pattern(slots[i].p, slots[i].sz, slots[i].stamp), "final pre-free pattern");
            j_free(slots[i].p);
            slots[i].live = 0;
            live_left++;
        }
    }
    t1 = clock();
    ms = 1000.0 * (t1 - t0) / CLOCKS_PER_SEC;
    printf("Phase5 cleanup: freed_left=%zu time=%.2fms\n", live_left, ms);
    print_stats("end");

    free(slots);
    return 0;
}
