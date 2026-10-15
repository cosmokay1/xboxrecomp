/* Optional host observation of completed kernel reads. No guest policy. */
#ifndef XBOX_FILE_READ_OBSERVER_H
#define XBOX_FILE_READ_OBSERVER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct XboxFileReadEvent {
    uintptr_t native_handle;
    const void *bytes;
    size_t requested, completed;
    int32_t status;
} XboxFileReadEvent;
typedef void (*XboxFileReadObserver)(const XboxFileReadEvent *event);
/* Configure only while file I/O is quiescent, normally before guest startup.
 * The callback runs synchronously on the reading thread. Handle and bytes are
 * borrowed until it returns, before a successful read signals its completion
 * event. The caller must keep the handle/buffer valid during the I/O call.
 * The callback must not modify/retain them, close the handle,
 * block on external work, or assume guest-lock ownership. Nested reads work but
 * do not recursively notify. This is a trusted host callback, not a sandbox.
 * bytes is non-NULL only for successful nonempty reads within requested bounds.
 * completed is zero on failures; this notification does not prove deserialization,
 * file-offset coordinates, a selected save, or immutable file contents.
 */
void xbox_SetFileReadObserver(XboxFileReadObserver observer);
/* Kernel-internal dispatch; callers preserve the original I/O result. */
void xbox_file_read_notify(uintptr_t handle, const void *buffer, size_t requested,
                          size_t completed, int32_t status);
#ifdef __cplusplus
}
#endif
#endif
