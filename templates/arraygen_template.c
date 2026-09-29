/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the copyright holder nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#ifndef ARRAY_ELEMS
#pragma message "ARRAY_ELEMS not defined. Using the default value."
#define ARRAY_ELEMS {{ARRAY_ELEMS_VALUE}}
#endif

#ifndef ACTIVE_ARRAY_ELEMS
#define ACTIVE_ARRAY_ELEMS ARRAY_ELEMS
#endif

#ifndef CACHE_LINE
#define CACHE_LINE {{PTRCHASE_CACHE_LINE_VALUE}}
#endif

struct line{
    struct line *next;
    uint8_t pad[CACHE_LINE-8];
};

int *res;
struct line *array;

#if defined(_WIN32)
static uint64_t mess_rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t mess_next_u64(void) {
    uint64_t z = (mess_rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t mess_below(uint64_t bound) {
    uint64_t threshold = (~bound + 1u) % bound;   // 2^64 mod bound 
    for (;;) {
        uint64_t r = mess_next_u64();
        if (r >= threshold) return r % bound;
    }
}
#endif

void shuffle(int *array, size_t n) {
    /* Deterministic seed keeps pointer-chase walk reproducible across runs. */
    srand(0);
#if defined(_WIN32)
    mess_rng_state = 0x9E3779B97F4A7C15ULL;
#endif
    if (n > 1)
    {
        size_t i;
        for (i = 0; i < n - 1; i++)
        {
#if defined(_WIN32)
          size_t j = i + (size_t)mess_below((uint64_t)(n - i));
#else
          size_t j = i + rand() / (RAND_MAX / (n - i) + 1);
#endif
          int t = array[j];
          array[j] = array[i];
          array[i] = t;
        }
    }
}

void walk_generator(){
    int *indices = (int*)malloc(ACTIVE_ARRAY_ELEMS * sizeof(int));
    if (!indices) {
        printf("Memory allocation failed for indices\n");
        exit(1);
    }

    size_t i;
    for(i = 0; i < ACTIVE_ARRAY_ELEMS; i++){
        indices[i] = i;
    }

    shuffle(indices, ACTIVE_ARRAY_ELEMS);

    for (i = 0; i < ARRAY_ELEMS; i++) {
        array[i].next = &array[i];
    }

    for(i = 0; i < ACTIVE_ARRAY_ELEMS - 1; i++){
        res[indices[i]] = indices[i+1];
    }
    res[indices[ACTIVE_ARRAY_ELEMS - 1]] = indices[0];

    for(int j = 0; j < ACTIVE_ARRAY_ELEMS; j++){
        array[j].next = (struct line *) ((long)res[j] * CACHE_LINE);
    }
    
    free(indices);
}

int main() {
    res = (int*)malloc(ACTIVE_ARRAY_ELEMS * sizeof(int));
    array = (struct line*)malloc(ARRAY_ELEMS * sizeof(struct line));
    
    if (!res || !array) {
        printf("Memory allocation failed\n");
        return 1;
    }

    walk_generator();

    FILE *f = fopen("array.dat", "wb");
    if (!f) {
        printf("Failed to create array.dat\n");
        return 1;
    }

    fwrite(array, sizeof(struct line), ARRAY_ELEMS, f);
    fclose(f);

    free(res);
    free(array);

    printf("Generated random walk data for %d active elements out of %d allocated\n",
           ACTIVE_ARRAY_ELEMS, ARRAY_ELEMS);
    return 0;
}
