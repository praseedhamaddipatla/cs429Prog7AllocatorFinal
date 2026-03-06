#include "tdmm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIN getpagesize()
#define MAX_ORDER 30
#define MIN_ORDER 12 // page size = min
#define MAX_REGIONS 65536

region regions[MAX_REGIONS];
int regionCount = 0;

// buddy free list, organize by block size
sec *buddyLists[MAX_ORDER + 1];

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

int mixCount = 0;

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
    mixCount = 0;
    regionCount = 0;

    if (currPol == BUDDY) {
        for (int i = 0; i <= MAX_ORDER; i++)
            buddyLists[i] = NULL;
        return;
    }

    // map new heap
    size_t pgSize = MIN;
    // allocate less initially to improve memory utilization
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

    // prevent repeated calls
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

// BUDDY HELPERS

// get smallest power-of-2 that's enough
int orderSize(size_t size) {
    size_t total = size + sizeof(sec);
    int order = MIN_ORDER;

    while (((size_t)1 << order) < total)
        order++;

    return order;
}

// insert into buddy list
void buddyInsert(sec *block, int order) {
    block->free = 1;
    block->size = ((size_t)1 << order) - sizeof(sec);
    block->n = buddyLists[order];
    block->p = NULL;

    if (buddyLists[order])
        buddyLists[order]->p = block;

    buddyLists[order] = block;
}

// remove from buddy list
void buddyRemove(sec *block, int order) {
    if (block->p)
        block->p->n = block->n;
    else
        buddyLists[order] = block->n;

    if (block->n)
        block->n->p = block->p;

    block->n = block->p = NULL;
}

region *findRegion(void *ptr) {
    for (int i = 0; i < regionCount; i++) {
        char *start = regions[i].start;
        char *end = start + regions[i].size;
        if ((char *)ptr >= start && (char *)ptr < end)
            return &regions[i];
    }
    return NULL;
}

// find buddy address using XOR
sec *getBuddy(sec *block, int order) {
    region *r = findRegion(block);
    if (!r)
        return NULL;

    size_t blockSize = (size_t)1 << order;
    size_t offset = (char *)block - (char *)r->start;
    size_t buddyOffset = offset ^ blockSize;

    if (buddyOffset >= r->size)
        return NULL;

    return (sec *)((char *)r->start + buddyOffset);
}

sec *allocMoreBuddy(int order) {
    size_t blockSize = (size_t)1 << order;

    void *newMem = mmap(NULL, blockSize, PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

    if (newMem == MAP_FAILED) {
        fprintf(stderr, "buddy mmap failed");
        exit(1);
    }

    // register region
    regions[regionCount].start = newMem;
    regions[regionCount].size = blockSize;
    regionCount++;
    totMap += blockSize;
    sec *block = (sec *)newMem;
    block->free = 1;
    block->n = block->p = NULL;

    buddyInsert(block, order);
    return block;
}

// END OF BUDDY HELPERS

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

sec *findBuddy(size_t size) {
    int order = orderSize(size);
    int current = order;

    while (current <= MAX_ORDER && buddyLists[current] == NULL)
        current++;

    if (current > MAX_ORDER) {
        allocMoreBuddy(order);
        return findBuddy(size);
    }

    sec *block = buddyLists[current];
    buddyRemove(block, current);

    // split until desired order
    while (current > order) {
        current--;
        size_t splitSize = (size_t)1 << current;
        sec *buddy = (sec *)((char *)block + splitSize);
        buddy->free = 1;
        buddy->size = ((size_t)1 << current) - sizeof(sec);
        buddy->n = buddy->p = NULL;
        buddyInsert(buddy, current);
    }

    block->free = 0;
    block->size = ((size_t)1 << order) - sizeof(sec);
    return block;
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

//STATS FOR REPORT

void printStats() {
    printf("Mapped: %zu\n", totMap);
    printf("Allocated: %zu\n", totAlloc);
    if (totMap > 0)
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
    //check separate buddy list
    if (currPol == BUDDY) {
        size_t count = 0;
        for (int i = 0; i <= MAX_ORDER; i++) {
            sec *curr = buddyLists[i];
            while (curr) {
                count++;
                curr = curr->n;
            }
        }
        return count;
    }
    size_t count = 0;
    sec *curr = frH;
    while (curr) {
        count++;
        curr = curr->n;
    }
    return count;
}

//END OF STATS

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
    } else if (currPol == BUDDY) {

        found = findBuddy(aligned);
        if (!found)
            return NULL;
        found->size = aligned;
        insert(&allocH, found);
        totAlloc += aligned + sizeof(sec);
        totOh += sizeof(sec);
        utilSum += (double)totAlloc / totMap;
        utilCount++;
        return (void *)(found + 1);

    } else if (currPol == MIXED) {
        if (mixCount == 0) {
            mixCount++;
            found = findFirst(aligned);
        } else if (mixCount == 1) {
            mixCount++;
            found = findBest(aligned);
        } else {
            mixCount = 0;
            found = findWorst(aligned);
        }
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

    totAlloc += found->size + sizeof(sec);
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

//helper to separate free for buddy
void buddyFree(void *ptr) {
    sec *block = (sec *)ptr - 1;
    detach(&allocH, block);
    block->free = 1;

    totAlloc -= block->size + sizeof(sec);
    totOh -= sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    int order = orderSize(block->size);
    block->size = ((size_t)1 << order) - sizeof(sec);
    if (order > MAX_ORDER) {
        fprintf(stderr, "t_free: could not determine block order\n");
        return;
    }

    //merge recursively
    while (order < MAX_ORDER) {
        sec *buddy = getBuddy(block, order);
        if (!buddy)
            break;
        if (!buddy->free)
            break;
        if (buddy->size != ((size_t)1 << order) - sizeof(sec))
            break;
        buddyRemove(buddy, order);
        buddy->free = 0;

        if (buddy < block)
            block = buddy;
        order++;
        block->size = ((size_t)1 << order) - sizeof(sec);
    }
    buddyInsert(block, order);
}

void t_free(void *ptr) {
    if (!ptr)
        return;

    if (currPol == BUDDY) {
        buddyFree(ptr);
        return;
    }
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
    size_t requested = block->size; // saved request size
    block->free = 1;

    totAlloc -= requested + sizeof(sec);
    totOh -= sizeof(sec);
    utilSum += (double)totAlloc / totMap;
    utilCount++;

    // find merged block
    insert(&frH, block);
    sec *merged = merge(block);
}