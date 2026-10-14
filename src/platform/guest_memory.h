#ifndef XBOX_GUEST_MEMORY_H
#define XBOX_GUEST_MEMORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Checked leaf copies only. A false result may have copied a prefix: callers
 * must discard failed reads and must not assume failed writes are atomic.
 * Zero-sized copies accept NULL buffers but still validate guest/host bounds.
 * Supported implementations: Windows SEH and Linux process_vm_* only. */
bool xbox_guest_memory_read(uintptr_t base, uint64_t address, void *destination, size_t size);
bool xbox_guest_memory_write(uintptr_t base, uint64_t address, const void *source, size_t size);
#ifdef __cplusplus
}
#endif
#endif
