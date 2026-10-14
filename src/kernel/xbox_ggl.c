/* Global guest lock -- see xbox_ggl.h for why. */
#define XBOX_GGL_IMPL
#include "../platform/xbox_winnt.h"
#if defined(_WIN32)
#include <dbghelp.h>
#else
#include "../platform/win32_compat.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_ggl.h"
#if defined(_WIN32)
#pragma comment(lib, "dbghelp.lib")
#endif

static int g_enabled = -1;
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_turn = CONDITION_VARIABLE_INIT;
static volatile LONG g_waiters;
static unsigned long long g_next_seq;

/* Waiting threads. The lock goes to the highest priority waiter, FIFO among
 * equals -- the Xbox scheduler's rule. The timer/DPC thread runs at the top
 * priority, so vblank ISRs and DPCs preempt guest threads at their next kernel
 * call the way an interrupt would, instead of queueing behind them. */
#define GGL_MAX_WAITERS 64
static struct { DWORD tid; int prio; unsigned long long seq; } g_wait[GGL_MAX_WAITERS];
static int g_nwait;
static volatile LONG g_best_waiting_prio = -1000;
static XBOX_THREAD_LOCAL int t_prio;
static volatile DWORD g_owner;
static volatile LONGLONG g_owner_since;      /* QPC when the owner got the lock */
static volatile LONGLONG g_owner_last_point; /* QPC of its last kernel call */
static volatile unsigned int g_owner_last_site; /* guest return address of that call */
static LONGLONG g_qpc_freq, g_quantum_ticks;

static XBOX_THREAD_LOCAL int t_depth;

/* Resolve the native instruction where a stalled guest-lock owner is
 * actually spinning.  The last kernel return site only identifies the most
 * recent service call and can be far from the bad generated branch. */
/* Native PC/symbol inspection uses the Windows debugger API only. */
#if defined(_WIN32)
static void ggl_report_owner_pc(DWORD tid)
{
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_QUERY_INFORMATION, FALSE, tid);
    CONTEXT context;
    DWORD64 pc, displacement = 0;
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    SYMBOL_INFO *symbol = (SYMBOL_INFO *)storage;
    IMAGEHLP_LINE64 line;
    DWORD line_displacement = 0;
    static LONG symbols_ready;
    if (!thread) return;
    if (SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); return; }
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(thread, &context)) {
        ResumeThread(thread); CloseHandle(thread); return;
    }
#if defined(_M_X64) || defined(__x86_64__)
    pc = context.Rip;
#else
    pc = context.Eip;
#endif
    ResumeThread(thread);
    CloseHandle(thread);
    if (InterlockedCompareExchange(&symbols_ready, 1, 0) == 0) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
    }
    memset(storage, 0, sizeof(storage));
    symbol->SizeOfStruct = sizeof(*symbol);
    symbol->MaxNameLen = MAX_SYM_NAME;
    memset(&line, 0, sizeof(line));
    line.SizeOfStruct = sizeof(line);
    if (SymFromAddr(GetCurrentProcess(), pc, &displacement, symbol)) {
        fprintf(stderr, "  [GGL] owner native PC=%p %s+0x%llX",
                (void *)(uintptr_t)pc, symbol->Name,
                (unsigned long long)displacement);
        if (SymGetLineFromAddr64(GetCurrentProcess(), pc, &line_displacement, &line))
            fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "  [GGL] owner native PC=%p (symbol unavailable)\n",
                (void *)(uintptr_t)pc);
    }
    fflush(stderr);
}

#endif /* _WIN32 native PC diagnostics */

/* Per-thread accounting, reported by the watchdog every 10 s: how long each
 * guest thread held the guest CPU and how long it queued for it. */
#define GGL_STAT_SLOTS 32
static struct { volatile DWORD tid; volatile LONGLONG held, waited; volatile LONG grants; } g_stat[GGL_STAT_SLOTS];
static XBOX_THREAD_LOCAL int t_stat = -1;

static int ggl_stat_slot(void)
{
    if (t_stat < 0) {
        DWORD tid = GetCurrentThreadId();
        for (int i = 0; i < GGL_STAT_SLOTS; i++) {
            if (InterlockedCompareExchange((volatile LONG *)&g_stat[i].tid, (LONG)tid, 0) == 0
                || g_stat[i].tid == tid) { t_stat = i; break; }
        }
    }
    return t_stat;
}

/* The title's main game thread (the one polling the controllers; set by the
 * title's input layer) and how long a thread has waited for the lock in all,
 * in QPC ticks -- the frame-statistics hitch log reports the game thread's. */
volatile DWORD g_xbox_game_tid;
LONGLONG xbox_ggl_waited_qpc(DWORD tid)
{
    for (int i = 0; i < GGL_STAT_SLOTS; i++)
        if (g_stat[i].tid == tid) return g_stat[i].waited;
    return 0;
}

static LONGLONG qpc_now(void)
{
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

static DWORD WINAPI ggl_watchdog(LPVOID unused)
{
    DWORD reported = 0;
    int tick = 0;
    (void)unused;
    for (;;) {
        Sleep(1000);
        if (++tick % 10 == 0) {
            fprintf(stderr, "  [GGL] stats (ms held / ms waiting / grants):");
            for (int i = 0; i < GGL_STAT_SLOTS; i++)
                if (g_stat[i].tid && g_stat[i].grants)
                    fprintf(stderr, " t%lu=%lld/%lld/%ld", (unsigned long)g_stat[i].tid,
                            g_stat[i].held * 1000 / g_qpc_freq,
                            g_stat[i].waited * 1000 / g_qpc_freq, (long)g_stat[i].grants);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        {
            /* Stall detector: no grants for 10 s while threads wait. Name the
             * owner (or none -- a lost wake-up) and nudge the waiters. */
            static LONG last_total = -1;
            static int stalled_s;
            LONG total = 0;
            for (int i = 0; i < GGL_STAT_SLOTS; i++) total += g_stat[i].grants;
            if (total == last_total && g_waiters > 0) {
                if (++stalled_s == 10) {
                    fprintf(stderr, "  [GGL] STALL: no grants for 10 s; owner=%lu waiters=%ld "
                                    "best waiting prio=%ld last kernel call returned to guest 0x%08X\n",
                            (unsigned long)g_owner, (long)g_waiters, (long)g_best_waiting_prio, g_owner_last_site);
                    fflush(stderr);
#if defined(_WIN32)
                    if (g_owner) ggl_report_owner_pc(g_owner);
#endif
                    WakeAllConditionVariable(&g_turn);
                }
            } else {
                stalled_s = 0;
            }
            last_total = total;
        }
        DWORD owner = g_owner;
        LONGLONG last = g_owner_last_point;
        if (!owner || !g_waiters || !last) { reported = 0; continue; }
        LONGLONG ms = (qpc_now() - last) * 1000 / g_qpc_freq;
        if (ms >= 2000 && owner != reported) {
            fprintf(stderr, "  [GGL] thread %lu has held the guest CPU for %lld ms "
                            "without a kernel call (last one returned to guest 0x%08X); "
                            "%ld thread(s) waiting -- a guest spin loop the lock cannot preempt?\n",
                    (unsigned long)owner, ms, g_owner_last_site, (long)g_waiters);
            fflush(stderr);
            reported = owner;
        }
    }
}

int xbox_ggl_enabled(void)
{
    if (g_enabled < 0) {
        const char *v = getenv("RECOMP_GGL");
        LARGE_INTEGER f;
        int enabled = v && *v && strcmp(v, "0") != 0;
        QueryPerformanceFrequency(&f);
        g_qpc_freq = f.QuadPart;
        {
            const char *q = getenv("RECOMP_GGL_QUANTUM_MS");
            int ms = q ? atoi(q) : 2;
            if (ms < 1) ms = 1;
            g_quantum_ticks = g_qpc_freq * ms / 1000;
        }
        if (enabled) {
            const char *w = getenv("RECOMP_GGL_WATCHDOG");
            if (!w || strcmp(w, "0") != 0) {
                HANDLE t = CreateThread(NULL, 0, ggl_watchdog, NULL, 0, NULL);
                if (t) CloseHandle(t);
            }
            fprintf(stderr, "  [GGL] global guest lock enabled (quantum %lld ms)\n",
                    g_quantum_ticks * 1000 / g_qpc_freq);
            fflush(stderr);
        }
        g_enabled = enabled;
    }
    return g_enabled;
}

static XBOX_THREAD_LOCAL LONGLONG t_acquired_at;

/* Caller holds g_lock. */
static void ggl_update_best(void)
{
    int best = -1000;
    for (int i = 0; i < g_nwait; i++)
        if (g_wait[i].prio > best) best = g_wait[i].prio;
    g_best_waiting_prio = best;
}

/* Caller holds g_lock: is this waiter the one the lock should go to? */
static int ggl_is_next(unsigned long long seq, int prio)
{
    for (int i = 0; i < g_nwait; i++) {
        if (g_wait[i].prio > prio) return 0;
        if (g_wait[i].prio == prio && g_wait[i].seq < seq) return 0;
    }
    return 1;
}

static void ggl_acquire_raw(void)
{
    LONGLONG asked = qpc_now();
    int slot = ggl_stat_slot();
    DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&g_lock);
    DWORD held_by = g_owner;                     /* who we end up waiting on (hitch diagnosis) */
    unsigned int held_site = g_owner_last_site;
    if (g_owner != 0 || g_nwait > 0) {
        unsigned long long seq = g_next_seq++;
        int me = g_nwait < GGL_MAX_WAITERS ? g_nwait++ : GGL_MAX_WAITERS - 1;
        g_wait[me].tid = tid; g_wait[me].prio = t_prio; g_wait[me].seq = seq;
        ggl_update_best();
        InterlockedIncrement(&g_waiters);
        while (g_owner != 0 || !ggl_is_next(seq, t_prio))
            SleepConditionVariableSRW(&g_turn, &g_lock, INFINITE, 0);
        InterlockedDecrement(&g_waiters);
        for (int i = 0; i < g_nwait; i++)
            if (g_wait[i].tid == tid) { g_wait[i] = g_wait[--g_nwait]; break; }
        ggl_update_best();
    }
    g_owner = tid;
    g_owner_since = g_owner_last_point = t_acquired_at = qpc_now();
    ReleaseSRWLockExclusive(&g_lock);
    if (slot >= 0) {
        g_stat[slot].waited += t_acquired_at - asked;
        InterlockedIncrement(&g_stat[slot].grants);
    }
    {   /* A long wait for the lock stalls whichever frame this thread is
         * building: say who held it (first 200, waits over 10 ms). */
        static volatile LONG logged;
        LONGLONG ms = (t_acquired_at - asked) * 1000 / g_qpc_freq;
        if (ms >= 10 && held_by && held_by != tid && InterlockedIncrement(&logged) <= 200)
            fprintf(stderr, "  [GGL] thread %lu waited %lld ms for thread %lu (its last guest site 0x%08X)\n",
                    (unsigned long)tid, ms, (unsigned long)held_by, held_site);
    }
}

static void ggl_release_raw(void)
{
    int slot = ggl_stat_slot();
    if (slot >= 0)
        g_stat[slot].held += qpc_now() - t_acquired_at;
    AcquireSRWLockExclusive(&g_lock);
    g_owner = 0;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_turn);
}

void xbox_ggl_enter(void)
{
    if (!xbox_ggl_enabled()) return;
    if (t_depth++ == 0)
        ggl_acquire_raw();
}

void xbox_ggl_leave(void)
{
    if (!xbox_ggl_enabled() || t_depth <= 0) return;
    if (--t_depth == 0)
        ggl_release_raw();
}

int xbox_ggl_release_all(void)
{
    int depth = t_depth;
    if (depth > 0) {
        t_depth = 0;
        ggl_release_raw();
    }
    return depth;
}

void xbox_ggl_reacquire(int depth)
{
    if (depth > 0) {
        ggl_acquire_raw();
        t_depth = depth;
    }
}

void xbox_ggl_set_priority(int prio)
{
    t_prio = prio;
}

void xbox_ggl_preempt_point(unsigned int guest_site)
{
    LONGLONG now;
    if (t_depth <= 0) return;
    now = qpc_now();
    g_owner_last_point = now;
    g_owner_last_site = guest_site;
    if (g_waiters > 0 &&
        (g_best_waiting_prio > t_prio ||
         (g_best_waiting_prio == t_prio && now - g_owner_since >= g_quantum_ticks))) {
        int depth = xbox_ggl_release_all();
        xbox_ggl_reacquire(depth);
    }
}

/* Blocking primitives: give up the guest CPU for the duration. A thread that
 * does not hold it (host-only threads) goes straight through. */
#define GGL_BLOCKING(expr) do { int _d = xbox_ggl_release_all(); expr; xbox_ggl_reacquire(_d); } while (0)

void xbox_ggl_Sleep(DWORD ms)
{
    GGL_BLOCKING(Sleep(ms));
}

DWORD xbox_ggl_SleepEx(DWORD ms, BOOL alertable)
{
    DWORD r;
    GGL_BLOCKING(r = SleepEx(ms, alertable));
    return r;
}

BOOL xbox_ggl_SwitchToThread(void)
{
    BOOL r;
    GGL_BLOCKING(r = SwitchToThread());
    return r;
}

DWORD xbox_ggl_WaitForSingleObject(HANDLE h, DWORD ms)
{
    DWORD r;
    if (ms == 0) return WaitForSingleObject(h, 0);   /* a poll, not a block */
    GGL_BLOCKING(r = WaitForSingleObject(h, ms));
    return r;
}

DWORD xbox_ggl_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    DWORD r;
    if (ms == 0) return WaitForSingleObjectEx(h, 0, alertable);
    GGL_BLOCKING(r = WaitForSingleObjectEx(h, ms, alertable));
    return r;
}

DWORD xbox_ggl_WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    DWORD r;
    if (ms == 0) return WaitForMultipleObjects(n, h, all, 0);
    GGL_BLOCKING(r = WaitForMultipleObjects(n, h, all, ms));
    return r;
}

DWORD xbox_ggl_WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable)
{
    DWORD r;
    if (ms == 0) return WaitForMultipleObjectsEx(n, h, all, 0, alertable);
    GGL_BLOCKING(r = WaitForMultipleObjectsEx(n, h, all, ms, alertable));
    return r;
}

void xbox_ggl_EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (TryEnterCriticalSection(cs)) return;
    GGL_BLOCKING(EnterCriticalSection(cs));
}

BOOL xbox_ggl_SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    BOOL r;
    GGL_BLOCKING(r = SleepConditionVariableCS(cv, cs, ms));
    return r;
}

BOOL xbox_ggl_SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags)
{
    BOOL r;
    GGL_BLOCKING(r = SleepConditionVariableSRW(cv, l, ms, flags));
    return r;
}
