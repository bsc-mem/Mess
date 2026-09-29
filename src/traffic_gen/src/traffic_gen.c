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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/time.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#include <time.h>
#include <fcntl.h>
#include <getopt.h>
#include "utils.h"
#if defined(_WIN32) && defined(_OPENMP)
#include <omp.h>
#endif

#if defined(_WIN32)

#include <windows.h>

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED    ((void*)-1)

static void* mess_mmap_anon(size_t length, int prot) {
    DWORD protect = ((prot & PROT_READ) && (prot & PROT_WRITE)) ? PAGE_READWRITE
                  : (prot & PROT_WRITE)                         ? PAGE_READWRITE
                  : (prot & PROT_READ)                          ? PAGE_READONLY
                                                                : PAGE_NOACCESS;
    void* p = VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, protect);
    return p ? p : MAP_FAILED;
}

#define mmap(addr, len, prot, flags, fd, off) mess_mmap_anon((len), (prot))

static int mess_munmap(void* addr, size_t length) {
    (void)length;                     /* MEM_RELEASE frees the whole reservation */
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

#define munmap(addr, len) mess_munmap((addr), (len))

static int mess_aligned_alloc(void** out, size_t alignment, size_t size) {
    void* p = _aligned_malloc(size, alignment);
    if (!p) return -1;
    *out = p;
    return 0;
}

#define posix_memalign(out, alignment, size) mess_aligned_alloc((out), (alignment), (size))
#define mess_aligned_free(p) _aligned_free(p)

static int mess_signal_ready(const char* posix_path) {
    char        object[512];
    const char* leaf = strrchr(posix_path, '/');
    int         attempt;

    leaf = leaf ? leaf + 1 : posix_path;
    snprintf(object, sizeof(object), "\\\\.\\pipe\\%s", leaf);

    for (attempt = 0; attempt < 6000; ++attempt) {
        DWORD  err;
        HANDLE h = CreateFileA(object, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            char  ready = 'R';
            DWORD written = 0;
            BOOL  ok = WriteFile(h, &ready, 1, &written, NULL);
            CloseHandle(h);
            return ok ? 0 : -1;
        }
        err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) break;
        Sleep(10);
    }
    return -1;
}

#else

#define mess_aligned_free(p) free(p)

static int mess_signal_ready(const char* fifo_path) {
    int fifo_fd = open(fifo_path, O_WRONLY);
    if (fifo_fd >= 0) {
        char ready = 'R';
        ssize_t n = write(fifo_fd, &ready, 1);
        (void)n;
        close(fifo_fd);
        return 0;
    }
    return -1;
}

#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

#ifndef TrafficGen_ARRAY_SIZE
#define TrafficGen_ARRAY_SIZE 320000000
#endif

#ifndef TrafficGen_TYPE
#define TrafficGen_TYPE double
#endif

static TrafficGen_TYPE* a;
static TrafficGen_TYPE* b;
static ssize_t array_elements;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1.0e-9;
}

static void release_array(TrafficGen_TYPE* ptr, size_t bytes, int via_mmap) {
    if (!ptr) return;
    if (via_mmap) {
        munmap(ptr, bytes);
    } else {
        mess_aligned_free(ptr);
    }
}


int main(int argc, char* argv[]) {
    int rd_percentage = 50;
    int pause = 0;
    int opt;
    int verbose = 0;
    ssize_t requested_array_elements = 0;
    int workers = 1;
    double self_benchmark_seconds = 0.0;
    const char* fifo_path = NULL;
    static struct option long_options[] = {
        {"size", no_argument, 0, 'S'},
        {"self-benchmark", required_argument, 0, 'B'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, ":r:p:w:v:a:B:Sf:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'S':
                printf("traffic_gen_array_elements=%llu\n", (unsigned long long)TrafficGen_ARRAY_SIZE);
                printf("traffic_gen_array_bytes=%llu\n",
                       (unsigned long long)TrafficGen_ARRAY_SIZE * (unsigned long long)sizeof(TrafficGen_TYPE));
                return 0;
            case 'r':
                rd_percentage = atoi(optarg);
                if (rd_percentage < 0 || rd_percentage > 100) {
                    return 1;
                }
                break;
            case 'p':
                {
                    char *endptr;
                    long val = strtol(optarg, &endptr, 10);
                    if (*endptr != '\0' || val < 0) {
                        return 1;
                    }
                    pause = (int)val;
                }
                break;
            case 'w':
                workers = atoi(optarg);
                if (workers <= 0) {
                    return 1;
                }
                break;
            case 'v':
                verbose = atoi(optarg);
                break;
            case 'a':
                {
                    char *endptr;
                    unsigned long long val = strtoull(optarg, &endptr, 10);
                    if (*endptr != '\0' || val == 0) {
                        return 1;
                    }
                    requested_array_elements = (ssize_t)val;
                }
                break;
            case 'B':
                {
                    char *endptr;
                    double val = strtod(optarg, &endptr);
                    if (*endptr != '\0' || val <= 0.0) {
                        return 1;
                    }
                    self_benchmark_seconds = val;
                }
                break;
            case 'f':
                fifo_path = optarg;
                break;
            default:
                return 1;
        }
    }

#if defined(_WIN32) && defined(_OPENMP)
    // POSIX calibration sets OMP_NUM_THREADS=1 in its command environment.
    // Windows launches directly, so enforce the same first-touch behavior here.
    if (self_benchmark_seconds > 0.0) omp_set_num_threads(1);
#endif

    int ratio_granularity = TrafficGen_get_ratio_granularity();
    if (ratio_granularity <= 0) {
        return 1;
    }
    if ((rd_percentage % ratio_granularity) != 0) {
        return 1;
    }

    array_elements = requested_array_elements > 0
        ? requested_array_elements
        : (ssize_t)TrafficGen_ARRAY_SIZE;
    if (array_elements > (ssize_t)TrafficGen_ARRAY_SIZE) {
        if (verbose == 1) {
            fprintf(stderr, "Requested active array elements %zd exceed compiled capacity %zd, capping to capacity\n",
                    array_elements, (ssize_t)TrafficGen_ARRAY_SIZE);
        }
        array_elements = (ssize_t)TrafficGen_ARRAY_SIZE;
    }

    if (workers > 1) {
        array_elements /= workers;
        if (array_elements < 1) array_elements = 1;
    }

    int loop_increment = TrafficGen_get_loop_increment(rd_percentage);
    if (loop_increment <= 0) loop_increment = 400;

    int alignment = loop_increment * 10;
    array_elements = (array_elements / alignment) * alignment;
    if (array_elements == 0) array_elements = alignment;
    if ((uint64_t)array_elements > SIZE_MAX / sizeof(TrafficGen_TYPE)) {
        fprintf(stderr, "ERROR: TrafficGen array is too large: %zd elements\n", array_elements);
        return 1;
    }
    size_t allocation_elements = (size_t)array_elements;
    size_t array_bytes = allocation_elements * sizeof(TrafficGen_TYPE);
    int a_via_mmap = 0;
    int b_via_mmap = 0;
    
#if defined(__linux__) && defined(MAP_HUGETLB)
    #define MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB)
#else
    #define MMAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
#endif
#if defined(_WIN32)
    #define MMAP_LABEL "Mapped"
    #define ALIGNED_LABEL "aligned allocation"
#else
    #define MMAP_LABEL "Huge page"
    #define ALIGNED_LABEL "posix_memalign"
#endif

    a = (TrafficGen_TYPE*)mmap(NULL, array_bytes, PROT_READ | PROT_WRITE, MMAP_FLAGS, -1, 0);
    if (a == MAP_FAILED) {
        if(verbose == 1){
            fprintf(stderr, "%s allocation failed for array A, falling back to %s\n", MMAP_LABEL, ALIGNED_LABEL);
        }
        a = NULL;
        if (posix_memalign((void**)&a, 64, array_bytes) != 0) {
            fprintf(stderr, "ERROR: Failed to allocate TrafficGen array A (%zu bytes)\n", array_bytes);
            return 1;
        }
    } else {
        a_via_mmap = 1;
        if(verbose == 1){
            fprintf(stdout, "%s allocation success for array A\n", MMAP_LABEL);
        }   
    }

    b = (TrafficGen_TYPE*)mmap(NULL, array_bytes, PROT_READ | PROT_WRITE, MMAP_FLAGS, -1, 0);
    if (b == MAP_FAILED) {
        if(verbose == 1){
            fprintf(stderr, "%s allocation failed for array B, falling back to %s\n", MMAP_LABEL, ALIGNED_LABEL);
        }
        b = NULL;
        if (posix_memalign((void**)&b, 64, array_bytes) != 0) {
            fprintf(stderr, "ERROR: Failed to allocate TrafficGen array B (%zu bytes)\n", array_bytes);
            release_array(a, array_bytes, a_via_mmap);
            return 1;
        }
    } else {
        b_via_mmap = 1;
        if(verbose == 1){
            fprintf(stdout, "%s allocation success for array B\n", MMAP_LABEL);
        }
    }



    fprintf(stdout, "TrafficGen started: ratio=%d, pause=%d, workers=%d, elements=%zd\n", 
            rd_percentage, pause, workers, array_elements);
    fflush(stdout);
    fflush(stderr);

#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (size_t j = 0; j < allocation_elements; j++) {
        a[j] = 1.0;
        b[j] = 2.0;
    }

    if (self_benchmark_seconds > 0.0) {
        const int samples = 3;
        struct sample_result { double gbps; double elapsed; uint64_t iterations; } measured[3];

        TrafficGen_copy_rw(a, b, &array_elements, &pause, rd_percentage);

        for (int sample = 0; sample < samples; ++sample) {
            uint64_t iterations = 0;
            const double start = now_seconds();
            double elapsed = 0.0;
            do {
                TrafficGen_copy_rw(a, b, &array_elements, &pause, rd_percentage);
                iterations++;
                elapsed = now_seconds() - start;
            } while (elapsed < self_benchmark_seconds);

            double gbps = 0.0;
            if (elapsed > 0.0) {
                gbps = ((double)array_elements * (double)sizeof(TrafficGen_TYPE) *
                        (double)iterations) / elapsed / 1.0e9;
            }
            measured[sample] = (struct sample_result){gbps, elapsed, iterations};
        }
        for (int i = 1; i < samples; ++i) {
            for (int j = i; j > 0 && measured[j].gbps < measured[j - 1].gbps; --j) {
                struct sample_result tmp = measured[j];
                measured[j] = measured[j - 1];
                measured[j - 1] = tmp;
            }
        }
        const struct sample_result median = measured[samples / 2];
        fprintf(stdout, "MESS_TGEN_SELF_BW_GBPS=%.6f MESS_TGEN_SELF_SECONDS=%.6f MESS_TGEN_SELF_ITERS=%llu MESS_TGEN_SELF_ELEMENTS=%zd\n",
                median.gbps, median.elapsed, (unsigned long long)median.iterations, array_elements);
        fflush(stdout);

        release_array(a, array_bytes, a_via_mmap);
        release_array(b, array_bytes, b_via_mmap);
        return 0;
    }

    if (fifo_path) {
        mess_signal_ready(fifo_path);
    }

    for (;;) {
        {
            TrafficGen_copy_rw(a, b, &array_elements, &pause, rd_percentage);
        }
    }

    release_array(a, array_bytes, a_via_mmap);
    release_array(b, array_bytes, b_via_mmap);
    return 0;
}
