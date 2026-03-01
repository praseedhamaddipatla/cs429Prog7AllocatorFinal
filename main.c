#define _POSIX_C_SOURCE 199309L

#include "tdmm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

size_t getAllocCount();
size_t getFrCount();

// calc diff in nanoseconds between two timestamps
static long nsDiff(struct timespec t0, struct timespec t1) {
    return (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
}

// fragmentation patterns
static void createFragmentation(void **pins, int count) {

    size_t sizesA[] = {1024,  64, 2048, 128, 4096, 256, 512, 8192, 32, 1024,
                       16384, 64, 2048, 128, 4096, 256, 512, 8192, 32, 1024};

    size_t sizesB[] = {48, 96, 192, 384, 768, 1536, 3072, 6144, 12288, 24576};

    void *temp[count];

    // phase 1: alternating allocs
    for (int i = 0; i < count; i++) {

        temp[i] = t_malloc(sizesA[i % 20]);

        if (i % 2 == 0)
            pins[i] = t_malloc(64);
        else
            pins[i] = NULL;
    }

    // phase 2: free every 3rd to create holes
    for (int i = 0; i < count; i += 3) {
        t_free(temp[i]);
        temp[i] = NULL;
    }

    // phase 3: allocate different pattern
    for (int i = 0; i < count; i++) {

        if (temp[i] == NULL)
            temp[i] = t_malloc(sizesB[i % 10]);
    }

    // phase 4: free alternating
    for (int i = 1; i < count; i += 2) {
        t_free(temp[i]);
        temp[i] = NULL;
    }

    // phase 5: more mixed alloc/free
    for (int i = 0; i < count; i++) {

        if (temp[i] == NULL)
            temp[i] = t_malloc((i % 7 + 1) * 500);

        if (i % 4 == 0) {
            t_free(temp[i]);
            temp[i] = NULL;
        }
    }

    // cleanup temps but leave pins
    for (int i = 0; i < count; i++)
        if (temp[i])
            t_free(temp[i]);
}

// benchmark malloc and free
static void runBench(const char *name, alloc_strat_e pol) {

    printf("\n===== %s =====\n", name);

#define MAX_POW 23

    size_t sizes[MAX_POW + 1];

    for (int i = 0; i <= MAX_POW; i++)
        sizes[i] = (size_t)1 << i;

    int n = MAX_POW + 1;

    printf("%-12s %15s %15s\n", "Size (B)", "tmalloc (ns)", "tfree (ns)");

    int iters = 200;

    for (int i = 0; i < n; i++) {

        t_init(pol);

#define NFRAG 40
        void *pins[NFRAG];

        createFragmentation(pins, NFRAG);

        long totalMalloc = 0;
        long totalFree = 0;

        void *live[20] = {0};

        for (int r = 0; r < iters; r++) {

            struct timespec t0, t1;

            // malloc timing
            clock_gettime(CLOCK_MONOTONIC, &t0);
            void *p = t_malloc(sizes[i]);
            clock_gettime(CLOCK_MONOTONIC, &t1);

            totalMalloc += nsDiff(t0, t1);

            live[r % 20] = p;

            // periodically free something else
            if (r % 3 == 0 && live[(r + 7) % 20]) {

                clock_gettime(CLOCK_MONOTONIC, &t0);

                t_free(live[(r + 7) % 20]);

                clock_gettime(CLOCK_MONOTONIC, &t1);

                totalFree += nsDiff(t0, t1);

                live[(r + 7) % 20] = NULL;
            }
        }

        // cleanup
        for (int j = 0; j < 20; j++)
            if (live[j])
                t_free(live[j]);

        for (int j = 0; j < NFRAG; j++)
            if (pins[j])
                t_free(pins[j]);

        printf("%-12zu %15ld %15ld\n", sizes[i], totalMalloc / iters,
               totalFree / (iters / 3));
    }

    printf("\nFinal stats:\n");
    printStats();
}

// utilization over time
static void utilTest(const char *name, alloc_strat_e pol) {

    printf("\n===== %s =====\n", name);

    t_init(pol);

    printf("Step,Event,Size,");
    printStats();

    void *ptrs[20] = {0};

    size_t sizes[] = {4096, 512, 8192, 256, 16384, 1024, 2048, 512, 32768, 128};

    for (int i = 0; i < 10; i++) {

        ptrs[i] = t_malloc(sizes[i]);

        printf("%d,alloc,%zu,", i, sizes[i]);
        printStats();

        if (i % 2 == 1) {

            t_free(ptrs[i - 1]);
            ptrs[i - 1] = NULL;

            printf("%d,free,%zu,", i, sizes[i - 1]);
            printStats();
        }
    }

    // random realloc pattern
    for (int i = 0; i < 10; i++) {

        if (!ptrs[i]) {

            ptrs[i] = t_malloc(3000);

            printf("%d,realloc,3000,", i);
            printStats();
        }
    }

    for (int i = 0; i < 10; i++)
        if (ptrs[i])
            t_free(ptrs[i]);
}

// elper to print current state using available info
static void log_state(int step) {
    size_t aBlocks = getAllocCount();
    size_t fBlocks = getFrCount();
    
    // based on structs
    size_t overheadPerBlock = sizeof(sec) + sizeof(size_t);
    size_t totalOverhead = (aBlocks + fBlocks) * overheadPerBlock;

    printf("%d,%zu,%zu,%zu\n", step, aBlocks, fBlocks, totalOverhead);
}

void overheadTest(const char *name, alloc_strat_e policy) {
    printf("\n--- DATA_START_%s ---\n", name);
    printf("Step,AllocatedBlocks,FreeBlocks,TotalOverheadBytes\n");

    t_init(policy);
    
    // Pointer array
    void *ptrs[200];
    for(int i = 0; i < 200; i++) ptrs[i] = NULL;
    
    int step = 0;

    //phase 1: fragmentation holes
    for (int i = 0; i < 100; i++) {
        size_t size = (i % 5 + 1) * 128; // Sizes: 128, 256, 384, 512, 640
        ptrs[i] = t_malloc(size);
        
        // Free every 2nd block to create holes that cannot merge
        if (i % 2 == 0 && i > 0) {
            t_free(ptrs[i-1]);
            ptrs[i-1] = NULL;
        }
        log_state(step++);
    }

    //phase 2: stress zone
    for (int i = 100; i < 200; i++) {
        size_t request = (i % 10 + 1) * 60; // request small pieces
        
        int slot = i % 100;
        if (ptrs[slot] == NULL) {
            ptrs[slot] = t_malloc(request);
        } else {
            t_free(ptrs[slot]);
            ptrs[slot] = NULL;
        }
        log_state(step++);
    }

    //cleanup
    for (int i = 0; i < 200; i++) {
        if (ptrs[i]) {
            t_free(ptrs[i]);
            ptrs[i] = NULL;
            log_state(step++);
        }
    }

    printf("--- DATA_END_%s ---\n", name);
    printStats();
}

// correctness tests
static void correctnessTests() {

    t_init(FIRST_FIT);

    printf("\n===== BASIC CORRECTNESS =====\n");

    void *p = t_malloc(100);

    printf("ptr valid: %s\n", p ? "YES" : "NO");

    memset(p, 0xAA, 100);

    t_free(p);

    printStats();
}

int main() {

    correctnessTests();

    runBench("BENCH FIRST_FIT", FIRST_FIT);
    runBench("BENCH BEST_FIT", BEST_FIT);
    runBench("BENCH WORST_FIT", WORST_FIT);

    utilTest("UTIL FIRST_FIT", FIRST_FIT);
    utilTest("UTIL BEST_FIT", BEST_FIT);
    utilTest("UTIL WORST_FIT", WORST_FIT);

    overheadTest("OVERHEAD FIRST_FIT", FIRST_FIT);
    overheadTest("OVERHEAD BEST_FIT", BEST_FIT);
    overheadTest("OVERHEAD WORST_FIT", WORST_FIT);

    return 0;
}