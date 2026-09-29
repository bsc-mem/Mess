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
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#endif
#ifdef __linux__
#include <linux/perf_event.h>
#include <asm/unistd.h>
#else
struct perf_event_attr { int dummy; };
#define PERF_EVENT_IOC_RESET   0
#define PERF_EVENT_IOC_ENABLE  0
#define PERF_EVENT_IOC_DISABLE 0
#endif
#include <time.h>

#if !defined(_WIN32)
static int mess_thread_clock(struct timespec *ts) {
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, ts);
}
#endif

#if defined(_WIN32)

#include <windows.h>
#include <io.h>

// Windows exposes thread CPU time in 100 ns units through GetThreadTimes.
static int mess_thread_clock(struct timespec *ts) {
    FILETIME created, exited, kernel, user;
    ULARGE_INTEGER kernel_ticks, user_ticks;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return -1;
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    const unsigned long long ticks = kernel_ticks.QuadPart + user_ticks.QuadPart;
    ts->tv_sec = (time_t)(ticks / 10000000ULL);
    ts->tv_nsec = (long)((ticks % 10000000ULL) * 100ULL);
    return 0;
}

typedef HANDLE mess_sem_t;
#define MESS_SEM_FAILED NULL

static HANDLE mess_open_named_semaphore(const char *posix_name) {
    char  object[512];
    const char *bare = (posix_name[0] == '/') ? posix_name + 1 : posix_name;
    size_t i;

    snprintf(object, sizeof(object), "Local\\%s", bare);
    for (i = 0; object[i] != '\0'; ++i) {
        if (object[i] == '/') object[i] = '_';
        /* leave the one backslash that follows "Local" alone */
        if (i > 6 && object[i] == '\\') object[i] = '_';
    }

    return CreateSemaphoreA(NULL, 0, LONG_MAX, object);
}

static int mess_sem_wait(mess_sem_t s) {
    return (WaitForSingleObject(s, INFINITE) == WAIT_OBJECT_0) ? 0 : -1;
}

static int mess_sem_post(mess_sem_t s) {
    return ReleaseSemaphore(s, 1, NULL) ? 0 : -1;
}

static void mess_sem_close(mess_sem_t s) {
    if (s) CloseHandle(s);
}

static int mess_pipe_write(const char *posix_path, const char *data) {
    char   object[512];
    const char *leaf = strrchr(posix_path, '/');
    HANDLE h;
    DWORD  written = 0;
    BOOL   ok;

    leaf = leaf ? leaf + 1 : posix_path;
    snprintf(object, sizeof(object), "\\\\.\\pipe\\%s", leaf);

    h = CreateFileA(object, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    ok = WriteFile(h, data, (DWORD)strlen(data), &written, NULL);
    CloseHandle(h);
    return ok ? 0 : -1;
}

#define mess_fsync(fd) _commit(fd)


static int mess_perf_ioctl(int fd, int request, int arg) {
    (void)fd; (void)request; (void)arg;
    return -1;
}

static int mess_win_large_pages = 0;

static int mess_aligned_alloc(void **out, size_t alignment, size_t size) {
    void *p = _aligned_malloc(size, alignment);
    if (!p) return -1;
    *out = p;
    return 0;
}

static int mess_enable_lock_memory_privilege(void) {
    HANDLE           token;
    TOKEN_PRIVILEGES tp;
    LUID             luid;
    int              enabled;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return 0;
    }
    if (!LookupPrivilegeValueW(NULL, L"SeLockMemoryPrivilege", &luid)) {
        CloseHandle(token);
        return 0;
    }
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
    enabled = (GetLastError() == ERROR_SUCCESS);
    CloseHandle(token);
    return enabled;
}

static int mess_hugepage_alloc(void **out, size_t size) {
    SIZE_T page = GetLargePageMinimum();
    void  *p;

    if (page == 0) return -1;                        // CPU or OS says no 
    if (!mess_enable_lock_memory_privilege()) return -1;

    p = VirtualAlloc(NULL, ((size + page - 1) / page) * page,
                     MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    if (!p) return -1;    /* usually physical fragmentation; caller falls back */

    *out = p;
    mess_win_large_pages = 1;
    return 0;
}

static void mess_aligned_free(void *p) {
    if (mess_win_large_pages) VirtualFree(p, 0, MEM_RELEASE);
    else                      _aligned_free(p);
}


static const char *mess_local_path(const char *posix_path) {
    static char resolved[512];
    char        tmp[MAX_PATH + 1];
    DWORD       n;
    size_t      i;

    if (strncmp(posix_path, "/tmp/", 5) != 0) return posix_path;

    n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n > MAX_PATH) return posix_path;
    while (n > 0 && (tmp[n - 1] == '\\' || tmp[n - 1] == '/')) tmp[--n] = '\0';

    snprintf(resolved, sizeof(resolved), "%s\\%s", tmp, posix_path + 5);
    for (i = strlen(tmp) + 1; resolved[i] != '\0'; ++i) {
        if (resolved[i] == '/') resolved[i] = '\\';
    }
    return resolved;
}

#define MESS_READ_BINARY "rb"

#else

#define mess_local_path(p) (p)
#define MESS_READ_BINARY "r"

typedef sem_t* mess_sem_t;
#define MESS_SEM_FAILED SEM_FAILED

static mess_sem_t mess_open_named_semaphore(const char *posix_name) {
    return sem_open(posix_name, O_CREAT, 0666, 0);
}

static int mess_sem_wait(mess_sem_t s)  { return sem_wait(s); }
static int mess_sem_post(mess_sem_t s)  { return sem_post(s); }
static void mess_sem_close(mess_sem_t s) { sem_close(s); }

static int mess_pipe_write(const char *posix_path, const char *data) {
    FILE *fp = fopen(posix_path, "w");
    if (!fp) return -1;
    fputs(data, fp);
    fflush(fp);
    fclose(fp);
    return 0;
}

#define mess_fsync(fd) fsync(fd)

static int mess_perf_ioctl(int fd, int request, int arg) {
    return ioctl(fd, request, arg);
}

static int mess_aligned_alloc(void **out, size_t alignment, size_t size) {
    return posix_memalign(out, alignment, size);
}

static int mess_hugepage_alloc(void **out, size_t size) {
    (void)out; (void)size;
    return -1;
}

#define mess_aligned_free(p) free(p)

#endif  /* _WIN32 */

#ifndef ARRAY_ELEMS
#pragma message "ARRAY_ELEMS not defined. Using the default value."
#define ARRAY_ELEMS {{ARRAY_ELEMS_VALUE}}
#endif

#ifndef ACTIVE_ARRAY_ELEMS
#define ACTIVE_ARRAY_ELEMS ARRAY_ELEMS
#endif

#ifndef ITERS
#pragma message "ITERS not defined. Using the default value."
#define ITERS {{ITERS_VALUE}}
#endif

#ifndef BURST_ITERS
#define BURST_ITERS ITERS
#endif

#ifndef LOOP_ACCESSES
#error "LOOP_ACCESSES must match the generated loop.h"
#endif

#ifndef PTRCHASE_USE_TLB1
#define PTRCHASE_USE_TLB1 {{PTRCHASE_USE_TLB1}}
#endif
#ifndef PTRCHASE_TLB1_EVENT
#define PTRCHASE_TLB1_EVENT {{PTRCHASE_TLB1_EVENT}}
#endif
#ifndef PTRCHASE_USE_TLB2
#define PTRCHASE_USE_TLB2 {{PTRCHASE_USE_TLB2}}
#endif
#ifndef PTRCHASE_TLB2_EVENT
#define PTRCHASE_TLB2_EVENT {{PTRCHASE_TLB2_EVENT}}
#endif
#ifndef PTRCHASE_DISABLE_HUGEPAGE
#define PTRCHASE_DISABLE_HUGEPAGE {{PTRCHASE_DISABLE_HUGEPAGE_VALUE}}
#endif

#ifndef PTRCHASE_ALLOC_ALIGNMENT
#define PTRCHASE_ALLOC_ALIGNMENT {{PTRCHASE_ALLOC_ALIGNMENT_VALUE}}
#endif

#ifndef CACHE_LINE
#define CACHE_LINE {{PTRCHASE_CACHE_LINE_VALUE}}
#endif

struct line{
    struct line *next;
    uint8_t pad[CACHE_LINE-8];
};

struct line *array;
volatile int keep_running = 1;

static char mess_pc_start_sem[64];
static char mess_pc_done_sem[64];
static char mess_pc_pipe[128];
static char mess_pc_ready_flag[128];

static int init_ipc_names() {
    const char* unique_id = getenv("MESS_UNIQUE_ID");
    if (!unique_id || unique_id[0] == '\0') {
        fprintf(stderr, "ERROR: MESS_UNIQUE_ID environment variable not set.\n");
        fprintf(stderr, "ptr_chase must be launched by the mess benchmark, not standalone.\n");
        return -1;
    }
    snprintf(mess_pc_start_sem, sizeof(mess_pc_start_sem), "/mess_pc_start_%s", unique_id);
    snprintf(mess_pc_done_sem, sizeof(mess_pc_done_sem), "/mess_pc_done_%s", unique_id);
    snprintf(mess_pc_pipe, sizeof(mess_pc_pipe), "/tmp/mess_ptrchase_pipe_%s", unique_id);
    snprintf(mess_pc_ready_flag, sizeof(mess_pc_ready_flag), "/tmp/ptr_chase_ready_%s.flag", unique_id);
    return 0;
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
#ifdef __linux__
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
#else
    return -1;
#endif
}

int fd_cycles = -1;
int fd_insts = -1;
int fd_tlb1 = -1;
int fd_tlb2 = -1;
int err_cycles = 0;
int err_insts = 0;
int err_tlb1 = 0;
int err_tlb2 = 0;

static int open_perf_event(uint32_t type, uint64_t config, int *out_errno) {
#ifdef __linux__
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = type;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    int fd = perf_event_open(&pe, 0, -1, -1, 0);
    if (fd == -1 && out_errno) *out_errno = errno;
    return fd;
#else
    if (out_errno) *out_errno = ENOSYS;
    return -1;
#endif
}

void setup_perf() {
#ifdef __linux__
    fd_cycles = open_perf_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, &err_cycles);
    if (fd_cycles == -1) { fprintf(stderr, "perf_event_open(cycles) failed: %s (errno=%d)\n", strerror(err_cycles), err_cycles); }

    fd_insts = open_perf_event(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, &err_insts);
    if (fd_insts == -1) { fprintf(stderr, "perf_event_open(instructions) failed: %s (errno=%d)\n", strerror(err_insts), err_insts); }

#if PTRCHASE_USE_TLB1
    fd_tlb1 = open_perf_event(PERF_TYPE_RAW, PTRCHASE_TLB1_EVENT, &err_tlb1);
    if (fd_tlb1 == -1) { fprintf(stderr, "perf_event_open(tlb1, type=RAW, config=0x%x) failed: %s (errno=%d)\n", PTRCHASE_TLB1_EVENT, strerror(err_tlb1), err_tlb1); }
    else { fprintf(stderr, "perf_event_open(tlb1, type=RAW, config=0x%x) OK fd=%d\n", PTRCHASE_TLB1_EVENT, fd_tlb1); }
#endif

#if PTRCHASE_USE_TLB2
    fd_tlb2 = open_perf_event(PERF_TYPE_RAW, PTRCHASE_TLB2_EVENT, &err_tlb2);
    if (fd_tlb2 == -1) { fprintf(stderr, "perf_event_open(tlb2, type=RAW, config=0x%x) failed: %s (errno=%d)\n", PTRCHASE_TLB2_EVENT, strerror(err_tlb2), err_tlb2); }
    else { fprintf(stderr, "perf_event_open(tlb2, type=RAW, config=0x%x) OK fd=%d\n", PTRCHASE_TLB2_EVENT, fd_tlb2); }
#endif
#endif

    /* Write persistent debug file (survives process exit, not cleaned by mess) */
    FILE *dbg = fopen(mess_local_path("/tmp/ptr_chase_perf_debug.txt"), "w");
    if (dbg) {
        fprintf(dbg, "=== ptr_chase perf_event_open debug ===\n");
        fprintf(dbg, "pid=%d\n", getpid());
        fprintf(dbg, "fd_cycles=%d (errno=%d)\n", fd_cycles, err_cycles);
        fprintf(dbg, "fd_insts=%d (errno=%d)\n", fd_insts, err_insts);
        fprintf(dbg, "USE_TLB1=%d TLB1_EVENT=0x%x fd_tlb1=%d (errno=%d)\n", PTRCHASE_USE_TLB1, PTRCHASE_TLB1_EVENT, fd_tlb1, err_tlb1);
        fprintf(dbg, "USE_TLB2=%d TLB2_EVENT=0x%x fd_tlb2=%d (errno=%d)\n", PTRCHASE_USE_TLB2, PTRCHASE_TLB2_EVENT, fd_tlb2, err_tlb2);
        fprintf(dbg, "DISABLE_HUGEPAGE=%d ALLOC_ALIGNMENT=%d CACHE_LINE=%d\n", PTRCHASE_DISABLE_HUGEPAGE, PTRCHASE_ALLOC_ALIGNMENT, CACHE_LINE);
        fprintf(dbg, "ARRAY_ELEMS=%d ITERS=%d BURST_ITERS=%d\n", ARRAY_ELEMS, ITERS, BURST_ITERS);
        fclose(dbg);
        fprintf(stderr, "ptr_chase: debug info written to /tmp/ptr_chase_perf_debug.txt\n");
    }
}

void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        keep_running = 0;
    }
}

int main(int argc, char *argv[]) {
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    int r;
    long unsigned int array_bytes = ARRAY_ELEMS * sizeof(struct line);
    int using_hugepages = 0;

    r = -1;
    if (!PTRCHASE_DISABLE_HUGEPAGE) {
        r = mess_hugepage_alloc((void **)&array, array_bytes);
        if (r == 0) {
            using_hugepages = 1;
        }
    }
    if (r != 0) {
        r = mess_aligned_alloc((void **)&array, PTRCHASE_ALLOC_ALIGNMENT, array_bytes);
    }
    if (r != 0) {
        fprintf(stderr, "Allocation of array failed, return code is %d\n", r);
        fflush(stderr);
        exit(1);
    }

#if PTRCHASE_DISABLE_HUGEPAGE
#if defined(MADV_NOHUGEPAGE)
    if (madvise(array, array_bytes, MADV_NOHUGEPAGE) != 0) {
        perror("madvise(MADV_NOHUGEPAGE) failed");
    } else {
        fprintf(stderr, "ptr_chase: MADV_NOHUGEPAGE enabled for this target\n");
        fflush(stderr);
    }
#endif
#else
#if defined(MADV_HUGEPAGE)
    if (madvise(array, array_bytes, MADV_HUGEPAGE) != 0) {
        perror("madvise(MADV_HUGEPAGE) failed");
    } else {
        fprintf(stderr, "madvise(MADV_HUGEPAGE) success\n");
        using_hugepages = 1;
    }
#endif
#endif
    fprintf(stderr, "ptr_chase: memory allocated (hugepages=%d)\n", using_hugepages);
    fflush(stderr);
    
    fprintf(stderr, "ptr_chase: opening array.dat...\n");
    fflush(stderr);
    FILE *f = fopen("array.dat", MESS_READ_BINARY);
    if (f == NULL) {
        fprintf(stderr, "Random walk file cannot be located. cwd=");
        fflush(stderr);
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd))) {
            fprintf(stderr, "%s\n", cwd);
        } else {
            fprintf(stderr, "(unknown)\n");
        }
        fflush(stderr);
        mess_aligned_free(array);
        exit(1);
    }
    
    r = fread(array,sizeof(*array),ARRAY_ELEMS,f);
    if (r != ARRAY_ELEMS) {
        fprintf(stderr, "Reading of the array from file failed, return code is %d\n", r);
        fflush(stderr);
        mess_aligned_free(array);
        exit(1);
    }
    fclose(f);
    
    setup_perf();
    fprintf(stderr, "ptr_chase: perf fds: cycles=%d insts=%d tlb1=%d tlb2=%d (independent, no grouping)\n",
            fd_cycles, fd_insts, fd_tlb1, fd_tlb2);
    fflush(stderr);

    if (init_ipc_names() != 0) {
        fflush(stderr);
        mess_aligned_free(array);
        return 1;
    }

    mess_sem_t start_sem = mess_open_named_semaphore(mess_pc_start_sem);
    mess_sem_t done_sem  = mess_open_named_semaphore(mess_pc_done_sem);
    if (start_sem == MESS_SEM_FAILED || done_sem == MESS_SEM_FAILED) {
        perror("sem_open");
        fflush(stderr);
        mess_aligned_free(array);
        return 1;
    }
    fprintf(stderr, "ptr_chase: semaphores opened\n");
    fflush(stderr);

    FILE *ready_file = fopen(mess_local_path(mess_pc_ready_flag), "w");
    if (ready_file) {
        fprintf(ready_file, "%d\n", getpid());
        fflush(ready_file);
        mess_fsync(fileno(ready_file));
        fclose(ready_file);
        fprintf(stderr, "ptr_chase: ready flag created\n");
        fflush(stderr);
    } else {
        perror("fopen ready_file");
        fflush(stderr);
        mess_sem_close(start_sem);
        mess_sem_close(done_sem);
        mess_aligned_free(array);
        return 1;
    }
    
    uint64_t current_offset = (uint64_t)array[0].next;

    fprintf(stderr, "ptr_chase: entering main loop\n");
    fflush(stderr);

    while (keep_running) {
        fprintf(stderr, "ptr_chase: waiting for start_sem\n");
        fflush(stderr);
        if (mess_sem_wait(start_sem) == -1) {
            if (errno == EINTR && keep_running) {
                continue;
            }
            break;
        }
        if (!keep_running) {
            break;
        }
        fprintf(stderr, "ptr_chase: got start_sem\n");
        fflush(stderr);

        uint64_t burst_start_offset = current_offset;

        if (fd_cycles != -1) mess_perf_ioctl(fd_cycles, PERF_EVENT_IOC_RESET, 0);
        if (fd_insts != -1)  mess_perf_ioctl(fd_insts,  PERF_EVENT_IOC_RESET, 0);
        if (fd_tlb1 != -1)   mess_perf_ioctl(fd_tlb1,   PERF_EVENT_IOC_RESET, 0);
        if (fd_tlb2 != -1)   mess_perf_ioctl(fd_tlb2,   PERF_EVENT_IOC_RESET, 0);

        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);

        if (fd_cycles != -1) mess_perf_ioctl(fd_cycles, PERF_EVENT_IOC_ENABLE, 0);
        if (fd_insts != -1)  mess_perf_ioctl(fd_insts,  PERF_EVENT_IOC_ENABLE, 0);
        if (fd_tlb1 != -1)   mess_perf_ioctl(fd_tlb1,   PERF_EVENT_IOC_ENABLE, 0);
        if (fd_tlb2 != -1)   mess_perf_ioctl(fd_tlb2,   PERF_EVENT_IOC_ENABLE, 0);

        // CPU time includes memory stalls but excludes descheduling.
        // Counter control and IPC are outside this loop interval.
        struct timespec thread_start = {0, 0}, thread_end = {0, 0};
        const int thread_clock_ok = mess_thread_clock(&thread_start) == 0;
        {
            uint64_t dummy_rcx;
            (void)dummy_rcx;
{{PTRCHASE_BURST_LOOP}}
        }
        long long pointer_chase_thread_ns = 0;
        if (thread_clock_ok && mess_thread_clock(&thread_end) == 0) {
            pointer_chase_thread_ns = (thread_end.tv_sec - thread_start.tv_sec) * 1000000000LL +
                                      (thread_end.tv_nsec - thread_start.tv_nsec);
        }

        if (fd_cycles != -1) mess_perf_ioctl(fd_cycles, PERF_EVENT_IOC_DISABLE, 0);
        if (fd_insts != -1)  mess_perf_ioctl(fd_insts,  PERF_EVENT_IOC_DISABLE, 0);
        if (fd_tlb1 != -1)   mess_perf_ioctl(fd_tlb1,   PERF_EVENT_IOC_DISABLE, 0);
        if (fd_tlb2 != -1)   mess_perf_ioctl(fd_tlb2,   PERF_EVENT_IOC_DISABLE, 0);

        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        long long duration_ns = (ts_end.tv_sec - ts_start.tv_sec) * 1000000000LL + (ts_end.tv_nsec - ts_start.tv_nsec);

        long long cycles = 0;
        long long insts = 0;
        long long tlb1 = 0;
        long long tlb2 = 0;

        if (fd_cycles != -1) read(fd_cycles, &cycles, sizeof(cycles));
        const unsigned long long accesses = (unsigned long long)BURST_ITERS * LOOP_ACCESSES;
        if (fd_insts != -1) read(fd_insts, &insts, sizeof(long long));
        if (fd_tlb1 != -1) read(fd_tlb1, &tlb1, sizeof(long long));
        if (fd_tlb2 != -1) read(fd_tlb2, &tlb2, sizeof(long long));

        char burst_msg[512];
        snprintf(burst_msg, sizeof(burst_msg),
                 "cycles=%lld instructions=%lld tlb1=%lld tlb2=%lld duration_ns=%lld accesses=%llu pointer_chase_thread_ns=%lld hugepages=%d pfd_cy=%d pfd_in=%d pfd_t1=%d pfd_t2=%d perr_t1=%d perr_t2=%d pcfg_t1=%d pcfg_t2=%d\n",
                 cycles, insts, tlb1, tlb2, duration_ns, accesses, pointer_chase_thread_ns, using_hugepages,
                 fd_cycles, fd_insts, fd_tlb1, fd_tlb2, err_tlb1, err_tlb2,
                 (int)PTRCHASE_TLB1_EVENT, (int)PTRCHASE_TLB2_EVENT);

        if (mess_pipe_write(mess_pc_pipe, burst_msg) != 0) {
            printf("BURST_DATA: cycles=%lld instructions=%lld tlb1=%lld tlb2=%lld duration_ns=%lld accesses=%llu pointer_chase_thread_ns=%lld hugepages=%d pfd_t1=%d pfd_t2=%d perr_t1=%d perr_t2=%d\n",
                   cycles, insts, tlb1, tlb2, duration_ns, accesses, pointer_chase_thread_ns, using_hugepages,
                   fd_tlb1, fd_tlb2, err_tlb1, err_tlb2);
            fflush(stdout);
        }

        mess_sem_post(done_sem);
    }

    if (fd_cycles != -1) close(fd_cycles);
    if (fd_insts != -1) close(fd_insts);
    if (fd_tlb1 != -1) close(fd_tlb1);
    if (fd_tlb2 != -1) close(fd_tlb2);

    mess_sem_close(start_sem);
    mess_sem_close(done_sem);
#if !defined(_WIN32)
    sem_unlink(mess_pc_start_sem);
    sem_unlink(mess_pc_done_sem);
#endif

    remove(mess_local_path(mess_pc_ready_flag));
    mess_aligned_free(array);
    printf("Done walking!\n");
    return 0;
}
