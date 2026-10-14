#define _GNU_SOURCE
#include "platform/win32_compat.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <errno.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr,"Failed: %s at %d\n",#test,__LINE__); abort(); } } while (0)
static SRWLOCK lock = SRWLOCK_INIT;
static CONDITION_VARIABLE condition = CONDITION_VARIABLE_INIT;
static atomic_int entered;
static int ready;
static ULONG mode;

static void *waiter(void *unused)
{
    (void)unused;
    if (mode) AcquireSRWLockShared(&lock); else AcquireSRWLockExclusive(&lock);
    atomic_store(&entered,1);
    while (!ready) CHECK(SleepConditionVariableSRW(&condition,&lock,INFINITE,mode));
    if (mode) ReleaseSRWLockShared(&lock); else ReleaseSRWLockExclusive(&lock);
    return NULL;
}

typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE condition;
    atomic_int entered;
    int ready;
} WaitGroup;

static void *group_waiter(void *argument)
{
    WaitGroup *group=(WaitGroup *)argument;
    AcquireSRWLockShared(&group->lock);
    atomic_fetch_add(&group->entered,1);
    while (!group->ready)
        CHECK(SleepConditionVariableSRW(&group->condition,&group->lock,INFINITE,CONDITION_VARIABLE_LOCKMODE_SHARED));
    ReleaseSRWLockShared(&group->lock);
    return NULL;
}

int main(void)
{
    /* Repeated immediate wake at release-to-wait exercises the lost-wakeup gap. */
    for (int shared=0;shared<2;shared++) {
        mode=shared ? CONDITION_VARIABLE_LOCKMODE_SHARED : 0;
        for (int i=0;i<250;i++) {
            ready=0;atomic_store(&entered,0);
            pthread_t thread;CHECK(pthread_create(&thread,NULL,waiter,NULL)==0);
            while (!atomic_load(&entered)) SwitchToThread();
            AcquireSRWLockExclusive(&lock);
            ready=1;
            if (i & 1) WakeConditionVariable(&condition); else WakeAllConditionVariable(&condition);
            ReleaseSRWLockExclusive(&lock);
            CHECK(pthread_join(thread,NULL)==0);
        }
        if (mode) AcquireSRWLockShared(&lock); else AcquireSRWLockExclusive(&lock);
        CHECK(!SleepConditionVariableSRW(&condition,&lock,0,mode));
        CHECK(GetLastError()==ERROR_TIMEOUT);
        /* Inspect the underlying POSIX lock independently to prove reacquisition. */
        int rc=pthread_rwlock_trywrlock((pthread_rwlock_t *)lock.Ptr);
        CHECK(rc==EBUSY || rc==EDEADLK);
        if (mode) ReleaseSRWLockShared(&lock); else ReleaseSRWLockExclusive(&lock);
    }
    AcquireSRWLockExclusive(&lock);
    CHECK(!SleepConditionVariableSRW(&condition,&lock,0,2));
    CHECK(GetLastError()==ERROR_INVALID_PARAMETER);
    int rc=pthread_rwlock_trywrlock((pthread_rwlock_t *)lock.Ptr);
    CHECK(rc==EBUSY || rc==EDEADLK);
    ReleaseSRWLockExclusive(&lock);
    /* Shared waiters race first use, then contend on one broadcast/reacquire. */
    WaitGroup group={SRWLOCK_INIT,CONDITION_VARIABLE_INIT,0,0};
    atomic_init(&group.entered,0);
    pthread_t threads[8];
    for (int i=0;i<8;i++) CHECK(pthread_create(&threads[i],NULL,group_waiter,&group)==0);
    while (atomic_load(&group.entered)!=8) SwitchToThread();
    AcquireSRWLockExclusive(&group.lock);
    group.ready=1;
    WakeAllConditionVariable(&group.condition);
    ReleaseSRWLockExclusive(&group.lock);
    for (int i=0;i<8;i++) CHECK(pthread_join(threads[i],NULL)==0);
    CRITICAL_SECTION cs;InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    CHECK(!SleepConditionVariableCS(&condition,&cs,0));
    CHECK(GetLastError()==ERROR_TIMEOUT);
    LeaveCriticalSection(&cs);DeleteCriticalSection(&cs);
    puts("POSIX SRW wake, shared/exclusive timeout and reacquisition verified");
    return 0;
}
