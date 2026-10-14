/*
 * Global guest lock: run translated guest code on one host thread at a time.
 *
 * The Xbox has a single CPU core. Titles and the XDK were written for that,
 * and quietly depend on it: a worker that checks "nothing to do" and then
 * suspends itself cannot lose the requester's resume in between, a lock-free
 * list touched by two threads is never touched by both at once, a DPC never
 * runs in parallel with the thread it interrupted. The runtime maps each guest
 * thread onto a real host thread, so all of those assumptions break at once --
 * NFL 2K5 hit four separate races of this kind in one day (lost resumes,
 * suspends landing inside host fprintf, a starved scheduler pump, a lock cycle
 * through XAPI's thread-notify lock).
 *
 * With RECOMP_GGL=1 every host thread that runs guest code holds this lock
 * while it does, so guest code executes one thread at a time, exactly like the
 * original machine. The lock is released:
 *   - inside every host blocking primitive the kernel layer uses (the macros
 *     below reroute Sleep, waits, contended EnterCriticalSection and condition
 *     variable sleeps), because on the Xbox a thread that blocks gives up the
 *     CPU;
 *   - at kernel-call boundaries once the holder has run for a quantum and
 *     someone is waiting (xbox_ggl_preempt_point), standing in for the
 *     scheduler's timer preemption.
 * A guest loop that spins on memory without ever calling the kernel cannot be
 * preempted here; the watchdog reports it (RECOMP_GGL_WATCHDOG, default on).
 *
 * The lock is a FIFO ticket lock so round-robin between ready threads is fair.
 * It is recursive per thread: the bridge sometimes runs guest callbacks from
 * inside a kernel call on a thread that already holds it.
 */
#ifndef XBOX_GGL_H
#define XBOX_GGL_H

#ifdef __cplusplus
extern "C" {
#endif

int  xbox_ggl_enabled(void);
void xbox_ggl_enter(void);          /* take (recursive) */
void xbox_ggl_leave(void);          /* drop one level */
int  xbox_ggl_release_all(void);    /* drop every level, return the depth */
void xbox_ggl_reacquire(int depth); /* restore a depth from release_all */
void xbox_ggl_preempt_point(unsigned int guest_site); /* kernel-call boundary: maybe yield */
void xbox_ggl_set_priority(int prio); /* this thread; higher wins, default 0 */

#ifndef XBOX_GGL_IMPL
#if defined(_WIN32)
#include <windows.h>
#else
#include "../platform/win32_compat.h"
#endif
void  xbox_ggl_Sleep(DWORD ms);
DWORD xbox_ggl_SleepEx(DWORD ms, BOOL alertable);
BOOL  xbox_ggl_SwitchToThread(void);
DWORD xbox_ggl_WaitForSingleObject(HANDLE h, DWORD ms);
DWORD xbox_ggl_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD xbox_ggl_WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
DWORD xbox_ggl_WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all, DWORD ms, BOOL alertable);
void  xbox_ggl_EnterCriticalSection(LPCRITICAL_SECTION cs);
BOOL  xbox_ggl_SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms);
BOOL  xbox_ggl_SleepConditionVariableSRW(PCONDITION_VARIABLE cv, PSRWLOCK l, DWORD ms, ULONG flags);

#define Sleep(ms)                          xbox_ggl_Sleep(ms)
#define SleepEx(ms, a)                     xbox_ggl_SleepEx(ms, a)
#define SwitchToThread()                   xbox_ggl_SwitchToThread()
#define WaitForSingleObject(h, ms)         xbox_ggl_WaitForSingleObject(h, ms)
#define WaitForSingleObjectEx(h, ms, a)    xbox_ggl_WaitForSingleObjectEx(h, ms, a)
#define WaitForMultipleObjects(n, h, w, ms)      xbox_ggl_WaitForMultipleObjects(n, h, w, ms)
#define WaitForMultipleObjectsEx(n, h, w, ms, a) xbox_ggl_WaitForMultipleObjectsEx(n, h, w, ms, a)
#define EnterCriticalSection(cs)           xbox_ggl_EnterCriticalSection(cs)
#define SleepConditionVariableCS(cv, cs, ms)     xbox_ggl_SleepConditionVariableCS(cv, cs, ms)
#define SleepConditionVariableSRW(cv, l, ms, f)  xbox_ggl_SleepConditionVariableSRW(cv, l, ms, f)
#endif

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GGL_H */
