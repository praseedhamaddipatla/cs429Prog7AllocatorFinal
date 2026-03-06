#!/bin/bash

set -e

CC=gcc
CFLAGS="-std=c11 -O2 -Wall"

echo "Compiling allocator..."
$CC $CFLAGS -c libtdmm/tdmm.c

# Create temporary test runner
cat << 'EOF' > __test_runner.c
#include "libtdmm/tdmm.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void correctness(int policy){

    t_init(policy);

    void *a = t_malloc(100);
    assert(a);
    memset(a,0xAA,100);

    void *b = t_malloc(200);
    void *c = t_malloc(50);
    assert(b && c);

    memset(b,0xBB,200);
    memset(c,0xCC,50);

    t_free(a);

    void *d = t_malloc(100);
    assert(d);
    memset(d,0xDD,100);

    unsigned char *buf = t_malloc(64);
    assert(buf);

    for(int i=0;i<64;i++) buf[i]=i;
    for(int i=0;i<64;i++) assert(buf[i]==i);

    t_free(buf);
    t_free(b);
    t_free(c);
    t_free(d);
}

void binaryStress(int policy){

    t_init(policy);

    const int N=1024;
    void *ptrs[N];

    for(int i=0;i<N;i++){
        ptrs[i]=t_malloc(32);
        assert(ptrs[i]);
    }

    for(int i=0;i<N;i+=2){
        t_free(ptrs[i]);
    }

    for(int i=1;i<N;i+=2){
        t_free(ptrs[i]);
    }
}

void mergeCascade(int policy){

    t_init(policy);

    void *ptrs[512];

    for(int i=0;i<512;i++){
        ptrs[i]=t_malloc(128);
        assert(ptrs[i]);
    }

    for(int i=511;i>=0;i--){
        t_free(ptrs[i]);
    }
}

void mmffPattern(int policy){

    t_init(policy);

    for(int i=0;i<5000;i++){

        void *a=t_malloc(64);
        void *b=t_malloc(128);
        assert(a && b);

        t_free(a);
        t_free(b);
    }
}

void randomStress(int policy){

    t_init(policy);

    const int N=2000;
    void *ptrs[N];

    for(int i=0;i<N;i++) ptrs[i]=NULL;

    for(int i=0;i<20000;i++){

        int idx=rand()%N;

        if(ptrs[idx]){
            t_free(ptrs[idx]);
            ptrs[idx]=NULL;
        }
        else{
            size_t size=(rand()%10+1)*64;
            ptrs[idx]=t_malloc(size);
        }
    }

    for(int i=0;i<N;i++)
        if(ptrs[i])
            t_free(ptrs[i]);
}

int main(int argc,char**argv){

    int test=atoi(argv[1]);
    int policy=atoi(argv[2]);

    if(test==0) correctness(policy);
    if(test==1) binaryStress(policy);
    if(test==2) mergeCascade(policy);
    if(test==3) mmffPattern(policy);
    if(test==4) randomStress(policy);

    return 0;
}
EOF

$CC $CFLAGS __test_runner.c tdmm.o -o __test_runner

POLICIES=("FIRST_FIT" "BEST_FIT" "WORST_FIT" "MIXED" "BUDDY")

run_test(){
    name=$1
    testid=$2

    for i in {0..4}; do

        printf "%-20s %-12s" "$name" "${POLICIES[$i]}"

        if ./__test_runner $testid $i >/dev/null 2>&1; then
            echo "PASS"
        else
            echo "FAIL"
        fi

    done

    echo
}

echo
echo "===== TEST RESULTS ====="

run_test "Correctness" 0
run_test "Binary Stress" 1
run_test "Merge Cascade" 2
run_test "MMFF Pattern" 3
run_test "Random Stress" 4

rm -f __test_runner __test_runner.c tdmm.o

echo "All tests complete."