#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "guest_memory.h"
#if defined(_WIN32)
#include <windows.h>
#include <string.h>
#elif defined(__linux__)
#include <sys/uio.h>
#include <unistd.h>
#else
#error "Checked guest-memory copies require Windows or Linux"
#endif

static bool copy_bounds(uintptr_t base, uint64_t address, const void *buffer,
                        size_t size, uintptr_t *host)
{
    if (address > UINT32_MAX || (uint64_t)size > UINT64_C(0x100000000) - address ||
        address > UINTPTR_MAX - base)
        return false;
    *host = base + (uintptr_t)address;
    if (size > UINTPTR_MAX - *host || (size && !buffer) ||
        size > UINTPTR_MAX - (uintptr_t)buffer)
        return false;
    return true;
}

bool xbox_guest_memory_read(uintptr_t base, uint64_t address, void *destination, size_t size)
{
    uintptr_t host;
    if (!copy_bounds(base,address,destination,size,&host)) return false;
    if (!size) return true;
#if defined(_WIN32)
    __try { memcpy(destination,(const void *)host,size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
#else
    struct iovec local = {destination,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_readv(getpid(),&local,1,&remote,1,0);
    return copied >= 0 && (size_t)copied == size;
#endif
}

bool xbox_guest_memory_write(uintptr_t base, uint64_t address, const void *source, size_t size)
{
    uintptr_t host;
    if (!copy_bounds(base,address,source,size,&host)) return false;
    if (!size) return true;
#if defined(_WIN32)
    __try { memcpy((void *)host,source,size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
#else
    struct iovec local = {(void *)source,size};
    struct iovec remote = {(void *)host,size};
    ssize_t copied = process_vm_writev(getpid(),&local,1,&remote,1,0);
    return copied >= 0 && (size_t)copied == size;
#endif
}
