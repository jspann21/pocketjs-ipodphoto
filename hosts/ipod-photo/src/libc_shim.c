#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/reent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

#include "heap.h"
#include "panic.h"
#include "timer.h"

static void set_reent_errno(struct _reent *reent, int value)
{
    if (reent != 0) reent->_errno = value;
    errno = value;
}

void *malloc(size_t size)
{
    void *pointer = pjs_heap_alloc(size, 16u);
    if (pointer == 0) errno = ENOMEM;
    return pointer;
}

void free(void *pointer)
{
    pjs_heap_free(pointer);
}

void *calloc(size_t count, size_t size)
{
    if (size != 0u && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return 0;
    }
    size_t total = count * size;
    void *pointer = pjs_heap_alloc(total, 16u);
    if (pointer == 0) {
        errno = ENOMEM;
        return 0;
    }
    uint8_t *bytes = pointer;
    for (size_t index = 0u; index < total; ++index) bytes[index] = 0u;
    return pointer;
}

void *realloc(void *pointer, size_t size)
{
    void *replacement = pjs_heap_realloc(pointer, size, 16u);
    if (replacement == 0 && size != 0u) errno = ENOMEM;
    return replacement;
}

size_t malloc_usable_size(void *pointer)
{
    return pjs_heap_usable_size(pointer);
}

void *_malloc_r(struct _reent *reent, size_t size)
{
    void *pointer = pjs_heap_alloc(size, 16u);
    if (pointer == 0) set_reent_errno(reent, ENOMEM);
    return pointer;
}

void _free_r(struct _reent *reent, void *pointer)
{
    (void)reent;
    pjs_heap_free(pointer);
}

void *_calloc_r(struct _reent *reent, size_t count, size_t size)
{
    if (size != 0u && count > SIZE_MAX / size) {
        set_reent_errno(reent, ENOMEM);
        return 0;
    }
    size_t total = count * size;
    void *pointer = pjs_heap_alloc(total, 16u);
    if (pointer == 0) {
        set_reent_errno(reent, ENOMEM);
        return 0;
    }
    uint8_t *bytes = pointer;
    for (size_t index = 0u; index < total; ++index) bytes[index] = 0u;
    return pointer;
}

void *_realloc_r(struct _reent *reent, void *pointer, size_t size)
{
    void *replacement = pjs_heap_realloc(pointer, size, 16u);
    if (replacement == 0 && size != 0u) set_reent_errno(reent, ENOMEM);
    return replacement;
}

void __malloc_lock(struct _reent *reent) { (void)reent; }
void __malloc_unlock(struct _reent *reent) { (void)reent; }

int posix_memalign(void **result, size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) ||
        (alignment & (alignment - 1u)) != 0u) {
        return EINVAL;
    }
    void *pointer = pjs_heap_alloc(size, alignment);
    if (pointer == 0) return ENOMEM;
    *result = pointer;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u ||
        size % alignment != 0u) {
        errno = EINVAL;
        return 0;
    }
    void *pointer = pjs_heap_alloc(size, alignment);
    if (pointer == 0) errno = ENOMEM;
    return pointer;
}

/* These libc clocks run on the main CPU. Sample at least once per hardware
 * timer wrap (about 71 minutes); extend unsigned deltas so normal running
 * applications do not see Date.now() move backwards at that boundary. */
static uint64_t libc_uptime_us(void)
{
    static uint32_t previous;
    static uint64_t elapsed;
    uint32_t now = timer_now_us();
    elapsed += (uint32_t)(now - previous);
    previous = now;
    return elapsed;
}

int _gettimeofday(struct timeval *value, void *timezone_value)
{
    (void)timezone_value;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t now = libc_uptime_us();
    value->tv_sec = (time_t)(now / 1000000u);
    value->tv_usec = (suseconds_t)(now % 1000000u);
    return 0;
}

int gettimeofday(struct timeval *value, void *timezone_value)
{
    return _gettimeofday(value, timezone_value);
}

time_t time(time_t *result)
{
    time_t seconds = (time_t)(libc_uptime_us() / 1000000u);
    if (result != 0) *result = seconds;
    return seconds;
}

int clock_gettime(clockid_t clock_id, struct timespec *value)
{
    (void)clock_id;
    if (value == 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t now = libc_uptime_us();
    value->tv_sec = (time_t)(now / 1000000u);
    value->tv_nsec = (long)((now % 1000000u) * 1000u);
    return 0;
}

clock_t _times(struct tms *buffer)
{
    if (buffer != 0) {
        buffer->tms_utime = 0;
        buffer->tms_stime = 0;
        buffer->tms_cutime = 0;
        buffer->tms_cstime = 0;
    }
    return (clock_t)(timer_now_us() / 10000u);
}

int _write(int descriptor, const void *buffer, size_t length)
{
    (void)descriptor;
    (void)buffer;
    return length > (size_t)INT32_MAX ? INT32_MAX : (int)length;
}

int _read(int descriptor, void *buffer, size_t length)
{
    (void)descriptor;
    (void)buffer;
    (void)length;
    return 0;
}

int _close(int descriptor)
{
    (void)descriptor;
    return 0;
}

int _fstat(int descriptor, struct stat *status)
{
    (void)descriptor;
    if (status == 0) {
        errno = EINVAL;
        return -1;
    }
    *status = (struct stat){0};
    status->st_mode = S_IFCHR;
    return 0;
}

int _stat(const char *path, struct stat *status)
{
    (void)path;
    (void)status;
    errno = ENOENT;
    return -1;
}

int _isatty(int descriptor)
{
    (void)descriptor;
    return 1;
}

off_t _lseek(int descriptor, off_t offset, int whence)
{
    (void)descriptor;
    (void)offset;
    (void)whence;
    return 0;
}

int _open(const char *path, int flags, ...)
{
    (void)path;
    (void)flags;
    errno = ENOENT;
    return -1;
}

int _unlink(const char *path)
{
    (void)path;
    errno = ENOENT;
    return -1;
}

int _link(const char *old_path, const char *new_path)
{
    (void)old_path;
    (void)new_path;
    errno = ENOENT;
    return -1;
}

void *_sbrk(ptrdiff_t increment)
{
    (void)increment;
    errno = ENOMEM;
    return (void *)-1;
}

int _getpid(void) { return 1; }

int _kill(int process, int signal)
{
    (void)process;
    (void)signal;
    errno = EINVAL;
    return -1;
}

int raise(int signal)
{
    (void)signal;
    errno = EINVAL;
    return -1;
}

char *environ[] = {0};

void abort(void)
{
    panic_code(0x51414254u); /* QABT */
}

void _exit(int status)
{
    panic_code(0x51455800u | ((uint32_t)status & 0xffu)); /* QEXx */
}

void __assert_func(const char *file, int line, const char *function,
                   const char *expression)
{
    (void)file;
    (void)line;
    (void)function;
    (void)expression;
    panic_code(0x51415352u); /* QASR */
}
