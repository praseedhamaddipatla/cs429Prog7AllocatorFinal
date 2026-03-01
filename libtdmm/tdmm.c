#include "tdmm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIN getpagesize()

sec *frH = NULL;    // free head
sec *allocH = NULL; // allocated head

void *mStart = NULL; // memory start
size_t mSize = 0;    // memory size

alloc_strat_e currPol;

size_t totMap = 0;
size_t totAlloc = 0;
size_t totOh = 0;
double utilSum = 0;
size_t utilCount = 0;

// get ptr to tag at end of block
size_t *tag(sec *s) { return (size_t *)((char *)(s + 1) + s->size); }

// call when size or free changes
void setTag(sec *s) { *tag(s) = s->size; }

// init structures
void t_init(alloc_strat_e pol) {

    // unmap old heap
    if (mStart && mStart != MAP_FAILED) {
        munmap(mStart, mSize);
    }

    // reset all globals
    mStart = NULL;
    mSize = 0;
    frH = NULL;
    allocH = NULL;

    currPol = pol;

    totMap = 0;
    totAlloc = 0;
    totOh = 0;
    utilSum = 0;
    utilCount = 0;

    // map new heap
    size_t pgSize = MIN;
    //allocate less initially to improve memory utilization
    size_t requested = pgSize;

    mSize = ((requested + pgSize - 1) / pgSize) * pgSize;

    mStart = mmap(NULL, mSize, PROT_READ | PROT_WRITE,
                  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (mStart == MAP_FAILED) {
        fprintf(stderr, "mmap failed");
        exit(1);
    }

    // init free list
    frH = (sec *)mStart;

    frH->size = mSize - sizeof(sec) - sizeof(size_t);
    frH->free = 1;
    frH->n = NULL;
    frH->p = NULL;

    setTag(frH);

    allocH = NULL;

    totMap = mSize;
}

// helper for removing block
void detach(sec **head, sec *s) {
    if (!s || !head)
        return;

    // check prev/next exist first
    if (s->p)
        s->p->n = s->n;
    else
        *head = s->n;

    if (s->n)
        s->n->p = s->p;

    s->n = NULL;
    s->p = NULL;
}

// helper for inserting block sorted by addr
void insert(sec **head, sec *s) {
    if (!s || !head)
        return;

    // first node with addr > s
    sec *curr = *head;
    sec *prev = NULL;
    while (curr && curr < s) {
        prev = curr;
        curr = curr->n;
    }

    // insert between prev and curr
    s->n = curr;
    s->p = prev;
    if (curr)
        curr->p = s;
    if (prev)
        prev->n = s;
    else
        *head = s;
}

size_t align(size_t size) {
    if (size % 4 == 0) {
        return size;
    }
    // round to next multiple of 4
    return ((size + 3) / 4) * 4;
}

sec *allocMore(size_t size) {
    size_t pgSize = getpagesize();
    size_t needed = size + sizeof(sec) + sizeof(size_t);
    size_t reqSize = ((needed + pgSize - 1) / pgSize) * pgSize;

    //prevent repeated calls
    if (reqSize < MIN)
        reqSize = MIN;

    void *newMem = mmap(NULL, reqSize, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (newMem == MAP_FAILED) {
        fprintf(stderr, "mmap failed");
        exit(1);
    }

    totMap += reqSize;

    // create free blcok
    sec *newBlock = (sec *)newMem;
    newBlock->size = reqSize - sizeof(sec) - sizeof(size_t);
    newBlock->free = 1;
    newBlock->n = NULL;
    newBlock->p = NULL;

    // update footer
    setTag(newBlock);

    insert(&frH, newBlock);
    return newBlock;
}

sec *findFirst(size_t size) {
    sec *search = frH;
    while (search != NULL) {
        if (search->size >= size) {
            return search;
        }
        search = search->n;
    }
    return allocMore(size);
}

sec *findBest(size_t size) {
    sec *search = frH;
    sec *best = NULL;
    size_t diff = SIZE_MAX;

    while (search != NULL) {
        if (search->size >= size) {

            size_t currDiff = search->size - size;

            if (best == NULL || currDiff < diff) {
                best = search;
                diff = currDiff;
            }
        }
        search = search->n;
    }

    if (best == NULL) {
        best = allocMore(size);
    }
    return best;
}

sec *findWorst(size_t size) {
    sec *search = frH;
    sec *worst = NULL;
    size_t diff = 0;
    while (search != NULL) {
        if (search->size >= size) {

            size_t currDiff = search->size - size;

            if (worst == NULL || currDiff > diff) {
                worst = search;
                diff = currDiff;
            }
        }

        search = search->n;
    }

    if (worst == NULL) {
        worst = allocMore(size);
    }
    return worst;
}

void split(sec *s, size_t aligned) {
    size_t total = sizeof(sec) + s->size + sizeof(size_t);

    size_t used = sizeof(sec) + aligned + sizeof(size_t);

    size_t remaining = total - used;

    if (remaining <= sizeof(sec) + sizeof(size_t) + 4)
        return;

    sec *new = (sec *)((char *)s + used);

    new->size = remaining - sizeof(sec) - sizeof(size_t);
    new->free = 1;
    new->n = NULL;
    new->p = NULL;

    s->size = aligned;

    setTag(s);
    setTag(new);

    insert(&frH, new);
}

void printStats() {
    printf("Mapped: %zu\n", totMap);
    printf("Allocated: %zu\n", totAlloc);
    printf("Utilization: %.2f%%\n", 100.0 * totAlloc / totMap);
    printf("Overhead: %zu\n", totOh);
    if (utilCount > 0)
        printf("Avg Utilization: %.4f%%\n", 100.0 * utilSum / utilCount);
}

size_t getAllocCount() {
    size_t count = 0;
    sec *curr = allocH;

    while (curr) {
        count++;
        curr = curr->n;
    }

    return count;
}

size_t getFrCount() {
    size_t count = 0;
    sec *curr = frH;
    while (curr) {
        count++;
        curr = curr->n;
    }
    return count;
}

void *t_malloc(size_t size) {
    if (size == 0)
        return NULL;
    size_t aligned = align(size);
    sec *found = NULL;

    // choose policy
    if (currPol == FIRST_FIT) {
        found = findFirst(aligned);
    } else if (currPol == BEST_FIT) {
        found = findBest(aligned);
    } else if (currPol == WORST_FIT) {
        found = findWorst(aligned);
    } else {
        fprintf(stderr, "policy undefined");
        exit(1);
    }
    if (found == NULL) {
        fprintf(stderr, "t_malloc failed");
        return NULL;
    }

    detach(&frH, found);
    split(found, aligned);

    found->free = 0;
    setTag(found);
    insert(&allocH, found);

    totAlloc += found->size;
    totOh += sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // return ptr
    return (void *)(found + 1);
}

// consolidate adj free blocks
sec *merge(sec *s) {
    sec *merged = s;

    // merge w next if free
    sec *next = frH;
    while (next) {
        if ((char *)next ==
            (char *)merged + sizeof(sec) + merged->size + sizeof(size_t)) {
            detach(&frH, next);

            merged->size += sizeof(sec) + sizeof(size_t) + next->size;
            setTag(merged);

            next = frH; // restart search
            continue;
        }
        next = next->n;
    }

    // merge w prev if free
    sec *prev = frH;
    while (prev) {
        if ((char *)merged ==
            (char *)prev + sizeof(sec) + prev->size + sizeof(size_t)) {
            detach(&frH, merged);

            prev->size += sizeof(sec) + sizeof(size_t) + merged->size;
            setTag(prev);
            merged = prev;
            prev = frH;
            continue;
        }
        prev = prev->n;
    }

    return merged;
}

void t_free(void *ptr) {
    if (!ptr)
        return;

    sec *block = (sec *)ptr - 1;

    // validate exists in alloc list
    sec *check = allocH;
    while (check && check != block)
        check = check->n;

    if (!check) {
        fprintf(stderr, "invalid or already-freed pointer\n");
        return;
    }

    // remove from alloc list
    detach(&allocH, block);
    block->free = 1;
    setTag(block);

    totAlloc -= block->size;
    totOh -= sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // find merged block
    insert(&frH, block);
    sec *merged = merge(block);
    // printStats();
}