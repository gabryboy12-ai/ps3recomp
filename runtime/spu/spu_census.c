/*
 * ps3recomp - SPU_CENSUS=1: per-image time of the lifted SPU programs.
 *
 * Every lifted SPU program -- a SPURS job, a task, a policy module, an SPU
 * thread, a raw SPU -- starts through spu_run_with_halt(), which brackets
 * the run with spu_census_begin()/spu_census_end(). Per image_id (the index
 * the lifted workload table gives each image) this counts runs, inclusive
 * time and self time: runs can nest, so self time subtracts the nested runs
 * and the self times add up without double counting. At exit it prints the
 * table, heaviest self time first -- which middleware (Edge, zlib, audio,
 * physics...) is worth replacing natively, measured rather than assumed.
 *
 * Two clocks per run. Wall time is what the run took, including any time
 * it was blocked (mailboxes, DMA waits) or waiting for a core; CPU time is
 * what its host thread actually computed. Together they tell "heavy" from
 * "waiting": a program worth replacing natively is heavy in CPU time. SPU
 * programs run on several threads at once, so the totals can exceed the
 * run's wall time. CPU time is the thread clock (CLOCK_THREAD_CPUTIME_ID);
 * Windows has no per-thread clock fine enough for short runs (its thread
 * times tick at the scheduler quantum), so there the column reads "-".
 * Off (the default), each run costs one cached flag test.
 *
 * The table prints at exit -- which a title under development often never
 * reaches (Ctrl+C, a hang, a crash) -- so SPU_CENSUS_EVERY=<seconds> also
 * prints it, cumulative, whenever that long has passed since the last one,
 * checked as SPU programs finish: the latest table is always in the log.
 */
#include "spu_census.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif
#include "../platform/win32_compat.h"   /* InterlockedExchangeAdd64 off Windows */

#if defined(_MSC_VER)
#  define CENSUS_TLS __declspec(thread)
#else
#  define CENSUS_TLS __thread
#endif

#define CENSUS_IMAGES  256            /* image_id beyond this: one "other" row */
#define CENSUS_DEPTH   32

static volatile long long s_runs[CENSUS_IMAGES];
static volatile long long s_incl_ns[CENSUS_IMAGES];
static volatile long long s_self_ns[CENSUS_IMAGES];
static volatile long long s_cpu_self_ns[CENSUS_IMAGES];
static volatile long      s_state = -1;   /* -1 not looked up, 0 off, 1 on */
static uint64_t           s_t_start;
static uint64_t           s_every_ns;            /* SPU_CENSUS_EVERY, 0 = exit only */
static volatile long long s_last_report;

static CENSUS_TLS int      t_depth;
static CENSUS_TLS uint64_t t_child_ns[CENSUS_DEPTH];
static CENSUS_TLS uint64_t t_child_cpu[CENSUS_DEPTH];
static CENSUS_TLS uint64_t t_cpu0[CENSUS_DEPTH];

static uint64_t census_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

#ifdef _WIN32
#  define CENSUS_HAVE_CPU 0
static uint64_t census_cpu_now(void) { return 0; }
#else
#  define CENSUS_HAVE_CPU 1
static uint64_t census_cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static void census_report(void)
{
    long long total_self = 0;
    int order[CENSUS_IMAGES], n = 0;
    for (int i = 0; i < CENSUS_IMAGES; i++)
        if (s_runs[i]) { order[n++] = i; total_self += s_self_ns[i]; }
    for (int a = 1; a < n; a++)                       /* insertion sort, self desc */
        for (int b = a; b > 0 && s_self_ns[order[b]] > s_self_ns[order[b - 1]]; b--) {
            int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    fprintf(stderr, "[spu_census] %.1f s of run time; SPU time is summed across "
            "threads and includes time blocked on channels\n",
            (double)(census_now() - s_t_start) / 1e9);
    fprintf(stderr, "[spu_census] %5s %9s %11s %11s %6s %11s %12s\n",
            "image", "runs", "total ms", "self ms", "self%", "self cpu ms", "cpu us/run");
    for (int k = 0; k < n; k++) {
        const int i = order[k];
        char id[8];
        if (i == CENSUS_IMAGES - 1) snprintf(id, sizeof id, "other");
        else snprintf(id, sizeof id, "%d", i);
        char cpu[24] = "-", cpu_run[24] = "-";
        if (CENSUS_HAVE_CPU) {
            snprintf(cpu, sizeof cpu, "%.2f", (double)s_cpu_self_ns[i] / 1e6);
            snprintf(cpu_run, sizeof cpu_run, "%.1f", (double)s_cpu_self_ns[i] / 1e3 / (double)s_runs[i]);
        }
        fprintf(stderr, "[spu_census] %5s %9lld %11.2f %11.2f %5.1f%% %11s %12s\n", id,
                s_runs[i], (double)s_incl_ns[i] / 1e6, (double)s_self_ns[i] / 1e6,
                total_self ? 100.0 * (double)s_self_ns[i] / (double)total_self : 0.0,
                cpu, cpu_run);
    }
    fflush(stderr);
}

static int census_on(void)
{
    if (s_state >= 0) return (int)s_state;
    const char* e = getenv("SPU_CENSUS");
    const long on = (e && e[0] && e[0] != '0') ? 1 : 0;
    /* Several SPU threads can get here first; one wins and arms the report. */
    if (InterlockedCompareExchange(&s_state, on, -1) == -1 && on) {
        s_t_start = census_now();
        s_last_report = (long long)s_t_start;
        const char* ev = getenv("SPU_CENSUS_EVERY");
        if (ev && atof(ev) > 0.0) s_every_ns = (uint64_t)(atof(ev) * 1e9);
        atexit(census_report);
    }
    return (int)s_state;
}

uint64_t spu_census_begin(void)
{
    if (!census_on()) return 0;
    const int d = t_depth++;
    if (d < CENSUS_DEPTH) { t_child_ns[d] = 0; t_child_cpu[d] = 0; t_cpu0[d] = census_cpu_now(); }
    return census_now();
}

void spu_census_end(int image_id, uint64_t t0)
{
    if (!t0) return;
    const uint64_t incl = census_now() - t0;
    const int d = --t_depth;
    uint64_t self = incl, cpu_incl = 0, cpu_self = 0;
    if (d >= 0 && d < CENSUS_DEPTH) {
        if (t_child_ns[d] <= incl) self = incl - t_child_ns[d];
        cpu_incl = census_cpu_now() - t_cpu0[d];
        cpu_self = (t_child_cpu[d] <= cpu_incl) ? cpu_incl - t_child_cpu[d] : 0;
    }
    if (d > 0 && d - 1 < CENSUS_DEPTH) {       /* the parent's child time, both clocks */
        t_child_ns[d - 1]  += incl;
        t_child_cpu[d - 1] += cpu_incl;
    }
    const int i = (image_id >= 0 && image_id < CENSUS_IMAGES - 1) ? image_id : CENSUS_IMAGES - 1;
    InterlockedIncrement64(&s_runs[i]);
    InterlockedExchangeAdd64(&s_incl_ns[i], (long long)incl);
    InterlockedExchangeAdd64(&s_self_ns[i], (long long)self);
    InterlockedExchangeAdd64(&s_cpu_self_ns[i], (long long)cpu_self);
    if (s_every_ns) {
        const long long last = s_last_report, now = (long long)(t0 + incl);
        /* one thread wins the slot; the others carry on */
        if ((uint64_t)(now - last) >= s_every_ns &&
            InterlockedCompareExchange64(&s_last_report, now, last) == last)
            census_report();
    }
}
