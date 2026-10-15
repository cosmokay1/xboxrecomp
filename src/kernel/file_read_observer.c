#include "file_read_observer.h"

static XboxFileReadObserver observer;
#ifdef _MSC_VER
static __declspec(thread) int notifying;
#else
static _Thread_local int notifying;
#endif

void xbox_SetFileReadObserver(XboxFileReadObserver next)
{
    observer = next;
}

void xbox_file_read_notify(uintptr_t handle, const void *buffer, size_t requested,
                          size_t completed, int32_t status)
{
    XboxFileReadObserver callback = observer;
    XboxFileReadEvent event;
    if (!callback || notifying) return;
    event.native_handle = handle;
    event.status = status;
    event.requested = requested;
    event.completed = status == 0 ? completed : 0;
    event.bytes = status == 0 && completed && completed <= requested ? buffer : NULL;
    notifying = 1;
    callback(&event);
    notifying = 0;
}
