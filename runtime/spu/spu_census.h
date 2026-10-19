/*
 * ps3recomp - SPU_CENSUS=1: where the lifted SPU programs spend their time.
 *
 * Every lifted SPU program -- a SPURS job, a task, a policy module, an SPU
 * thread, a raw SPU -- starts through spu_run_with_halt(), which brackets
 * the run with spu_census_begin()/spu_census_end(). Per image_id (the index
 * the lifted workload table gives each image) this counts runs, total time
 * and self time: runs can nest, so self time subtracts the nested runs and
 * the self times add up without double counting.
 *
 * Two clocks. Wall time is what a run took, including time blocked on
 * channels or waiting for a core; CPU time is what its host thread actually
 * computed (CLOCK_THREAD_CPUTIME_ID; Windows has no per-thread clock fine
 * enough for short runs, so there the CPU columns read "-").
 *
 * Interpreted time: spu_interp_run_until() is bracketed too. Code that is not
 * the lifted image's -- a policy module DMAs another program into local store
 * and branches into it -- runs in the interpreter, and its time lands under
 * the running image. The "interp" columns say how much of each image's self
 * time that was. A longjmp out of the interpreter (a halt) skips the end of
 * its bracket; spu_census_end() closes it when the run that contains it ends.
 *
 * The table, heaviest self time first, prints at exit, and with
 * SPU_CENSUS_EVERY=<seconds> also whenever that long has passed since the
 * last one, checked as SPU programs finish -- a title under development
 * rarely exits cleanly. Off (the default), each hook is one load and a branch.
 *
 * Header-only, like include/ps3emu/ps3_ftrace.h: spu_channels.c and
 * spu_interp.c are also compiled on their own by the self-contained SPU tests
 * in the CI workflows, so a new .c to link would break them. The state is one
 * weak/selectany object the linker folds into a single copy; the functions
 * are static inline.
 */
#ifndef PS3RECOMP_SPU_CENSUS_H
#define PS3RECOMP_SPU_CENSUS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_MSC_VER)
#  define SPU_CENSUS_SHARED __declspec(selectany)
#  define SPU_CENSUS_TLS    __declspec(thread)
#else
#  define SPU_CENSUS_SHARED __attribute__((weak))
#  define SPU_CENSUS_TLS    __thread
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SPU_CENSUS_ROWS   256     /* image_id >= ROWS-1 shares the "other" row */
#define SPU_CENSUS_DEPTH  32      /* nested runs per thread */
#define SPU_CENSUS_SLOTS  128     /* SPU contexts with an interpreter bracket */

/* One open interpreter bracket per SPU context. A context runs on one host
 * thread at a time, so after the claim only that thread touches the slot. */
typedef struct spu_census_slot {
    const void* ctx;
    int         depth;
    int         image_id;
    uint64_t    t0, cpu0;
} spu_census_slot;

typedef struct spu_census_state {
    int             state;            /* 0 not looked up, 1 on, 2 off, 3 setting up */
    uint64_t        t_start, every_ns, last_report;
    long long       runs[SPU_CENSUS_ROWS];
    long long       incl_ns[SPU_CENSUS_ROWS];
    long long       self_ns[SPU_CENSUS_ROWS];
    long long       cpu_self_ns[SPU_CENSUS_ROWS];
    long long       interp_ns[SPU_CENSUS_ROWS];
    long long       interp_cpu_ns[SPU_CENSUS_ROWS];
    long long       dropped;          /* interpreter brackets with no free slot */
    spu_census_slot slot[SPU_CENSUS_SLOTS];
} spu_census_state;

/* selectany needs an initializer; {0} / {} are the forms no compiler flags */
#ifdef __cplusplus
SPU_CENSUS_SHARED spu_census_state g_spu_census = {};
#else
SPU_CENSUS_SHARED spu_census_state g_spu_census = { 0 };
#endif

static inline uint64_t spu_census_now(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);      /* no <windows.h> in a widely included header */
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#ifdef _WIN32
#  define SPU_CENSUS_HAVE_CPU 0
static inline uint64_t spu_census_cpu_now(void) { return 0; }
#else
#  define SPU_CENSUS_HAVE_CPU 1
static inline uint64_t spu_census_cpu_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

static inline void spu_census_add(long long* p, long long v)
{
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

static inline int spu_census_row(int image_id)
{
    return (image_id >= 0 && image_id < SPU_CENSUS_ROWS - 1) ? image_id : SPU_CENSUS_ROWS - 1;
}

static inline void spu_census_report(void)
{
    spu_census_state* s = &g_spu_census;
    long long total_self = 0;
    int order[SPU_CENSUS_ROWS], n = 0;
    for (int i = 0; i < SPU_CENSUS_ROWS; i++)
        if (__atomic_load_n(&s->runs[i], __ATOMIC_RELAXED)) { order[n++] = i; total_self += s->self_ns[i]; }
    for (int a = 1; a < n; a++)                       /* insertion sort, self desc */
        for (int b = a; b > 0 && s->self_ns[order[b]] > s->self_ns[order[b - 1]]; b--) {
            const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    fprintf(stderr, "[spu_census] %.1f s of run time; SPU time is summed across "
            "threads and includes time blocked on channels\n",
            (double)(spu_census_now() - s->t_start) / 1e9);
    fprintf(stderr, "[spu_census] %5s %9s %11s %11s %6s %11s %11s %10s %7s\n", "image", "runs",
            "total ms", "self ms", "self%", "self cpu ms", "cpu us/run", "interp ms", "interp%");
    for (int k = 0; k < n; k++) {
        const int i = order[k];
        char id[8], cpu[24] = "-", cpu_run[24] = "-";
        if (i == SPU_CENSUS_ROWS - 1) snprintf(id, sizeof id, "other");
        else snprintf(id, sizeof id, "%d", i);
        if (SPU_CENSUS_HAVE_CPU) {
            snprintf(cpu, sizeof cpu, "%.2f", (double)s->cpu_self_ns[i] / 1e6);
            snprintf(cpu_run, sizeof cpu_run, "%.1f", (double)s->cpu_self_ns[i] / 1e3 / (double)s->runs[i]);
        }
        double ip = s->self_ns[i] ? 100.0 * (double)s->interp_ns[i] / (double)s->self_ns[i] : 0.0;
        if (ip > 100.0) ip = 100.0;   /* a bracket attributed to an outer image */
        fprintf(stderr, "[spu_census] %5s %9lld %11.2f %11.2f %5.1f%% %11s %11s %10.2f %6.1f%%\n", id,
                s->runs[i], (double)s->incl_ns[i] / 1e6, (double)s->self_ns[i] / 1e6,
                total_self ? 100.0 * (double)s->self_ns[i] / (double)total_self : 0.0,
                cpu, cpu_run, (double)s->interp_ns[i] / 1e6, ip);
    }
    if (s->dropped)
        fprintf(stderr, "[spu_census] %lld interpreter brackets not timed (no free slot)\n", s->dropped);
    fflush(stderr);
}

static inline void spu_census_report_atexit(void) { spu_census_report(); }

static inline int spu_census_on(void)
{
    spu_census_state* s = &g_spu_census;
    const int st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (st) return st == 1;
    const char* e = getenv("SPU_CENSUS");
    const int on = (e && e[0] && e[0] != '0') ? 1 : 0;
    int expect = 0;
    if (on) {                       /* set the clocks up before anyone sees "on" */
        if (__atomic_compare_exchange_n(&s->state, &expect, 3, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            s->t_start = spu_census_now();
            s->last_report = s->t_start;
            const char* ev = getenv("SPU_CENSUS_EVERY");
            if (ev && atof(ev) > 0.0) s->every_ns = (uint64_t)(atof(ev) * 1e9);
            atexit(spu_census_report_atexit);
            __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
        }
    } else {
        __atomic_compare_exchange_n(&s->state, &expect, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return __atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == 1;   /* 3: still setting up */
}

/* The interpreter bracket of a context: find it, or claim a free slot. */
static inline spu_census_slot* spu_census_slot_of(const void* ctx, int claim)
{
    const uintptr_t h = ((uintptr_t)ctx >> 4) * 2654435761u;
    for (int k = 0; k < SPU_CENSUS_SLOTS; k++) {
        spu_census_slot* sl = &g_spu_census.slot[(h + (uintptr_t)k) % SPU_CENSUS_SLOTS];
        const void* owner = __atomic_load_n(&sl->ctx, __ATOMIC_ACQUIRE);
        if (owner == ctx) return sl;
        if (!owner) {
            if (!claim) return NULL;
            const void* expect = NULL;
            if (__atomic_compare_exchange_n(&sl->ctx, &expect, ctx, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return sl;
            if (expect == ctx) return sl;
        }
    }
    return NULL;
}

static inline void spu_census_interp_close(spu_census_slot* sl)
{
    const int i = spu_census_row(sl->image_id);
    spu_census_add(&g_spu_census.interp_ns[i], (long long)(spu_census_now() - sl->t0));
    spu_census_add(&g_spu_census.interp_cpu_ns[i], (long long)(spu_census_cpu_now() - sl->cpu0));
    sl->depth = 0;
}

static inline void spu_census_interp_begin(const void* ctx, int image_id)
{
    if (!spu_census_on()) return;
    spu_census_slot* sl = spu_census_slot_of(ctx, 1);
    if (!sl) { spu_census_add(&g_spu_census.dropped, 1); return; }
    if (sl->depth++ == 0) {               /* outermost: nested interpretation is inside it */
        sl->image_id = image_id;
        sl->t0 = spu_census_now();
        sl->cpu0 = spu_census_cpu_now();
    }
}

static inline void spu_census_interp_end(const void* ctx)
{
    if (__atomic_load_n(&g_spu_census.state, __ATOMIC_RELAXED) != 1) return;
    spu_census_slot* sl = spu_census_slot_of(ctx, 0);
    if (sl && sl->depth > 0 && --sl->depth == 0) spu_census_interp_close(sl);
}

/* Run nesting is tracked per thread. Only spu_channels.c brackets runs, so
 * these thread-locals (one copy per including file) need not be shared. */
typedef struct spu_census_tls {
    int      depth;
    uint64_t child_ns[SPU_CENSUS_DEPTH], child_cpu[SPU_CENSUS_DEPTH], cpu0[SPU_CENSUS_DEPTH];
} spu_census_tls;

static inline spu_census_tls* spu_census_thread(void)
{
    static SPU_CENSUS_TLS spu_census_tls t;
    return &t;
}

static inline uint64_t spu_census_begin(void)
{
    if (!spu_census_on()) return 0;
    spu_census_tls* t = spu_census_thread();
    const int d = t->depth++;
    if (d < SPU_CENSUS_DEPTH) { t->child_ns[d] = 0; t->child_cpu[d] = 0; t->cpu0[d] = spu_census_cpu_now(); }
    return spu_census_now();
}

static inline void spu_census_end(const void* ctx, int image_id, uint64_t t0)
{
    if (!t0) return;
    spu_census_state* s = &g_spu_census;
    /* An interpreter bracket opened during this run and left open by a
     * longjmp ends with the run. One opened before it is not ours to close. */
    spu_census_slot* sl = spu_census_slot_of(ctx, 0);
    if (sl && sl->depth > 0 && sl->t0 >= t0) spu_census_interp_close(sl);

    spu_census_tls* t = spu_census_thread();
    const uint64_t incl = spu_census_now() - t0;
    const int d = --t->depth;
    uint64_t self = incl, cpu_incl = 0, cpu_self = 0;
    if (d >= 0 && d < SPU_CENSUS_DEPTH) {
        if (t->child_ns[d] <= incl) self = incl - t->child_ns[d];
        cpu_incl = spu_census_cpu_now() - t->cpu0[d];
        cpu_self = (t->child_cpu[d] <= cpu_incl) ? cpu_incl - t->child_cpu[d] : 0;
    }
    if (d > 0 && d - 1 < SPU_CENSUS_DEPTH) {   /* the parent's child time, both clocks */
        t->child_ns[d - 1]  += incl;
        t->child_cpu[d - 1] += cpu_incl;
    }
    const int i = spu_census_row(image_id);
    spu_census_add(&s->runs[i], 1);
    spu_census_add(&s->incl_ns[i], (long long)incl);
    spu_census_add(&s->self_ns[i], (long long)self);
    spu_census_add(&s->cpu_self_ns[i], (long long)cpu_self);
    if (s->every_ns) {
        uint64_t last = __atomic_load_n(&s->last_report, __ATOMIC_RELAXED);
        const uint64_t now = t0 + incl;
        if (now - last >= s->every_ns &&           /* one thread wins the slot */
            __atomic_compare_exchange_n(&s->last_report, &last, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            spu_census_report();
    }
}

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_SPU_CENSUS_H */
