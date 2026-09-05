#include <stddef.h>
#include <stdint.h>

void *memset(void *destination, int value, size_t length)
{
    uint8_t *out = (uint8_t *)destination;
    while (length-- != 0u) *out++ = (uint8_t)value;
    return destination;
}

void *memcpy(void *destination, const void *source, size_t length)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    while (length-- != 0u) *out++ = *in++;
    return destination;
}

void *memmove(void *destination, const void *source, size_t length)
{
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    if (out < in) {
        while (length-- != 0u) *out++ = *in++;
    } else if (out > in) {
        out += length;
        in += length;
        while (length-- != 0u) *--out = *--in;
    }
    return destination;
}

int memcmp(const void *left, const void *right, size_t length)
{
    const uint8_t *a = (const uint8_t *)left;
    const uint8_t *b = (const uint8_t *)right;
    while (length-- != 0u) {
        if (*a != *b) return *a < *b ? -1 : 1;
        ++a;
        ++b;
    }
    return 0;
}

/* ARM EABI helpers emitted for aggregate copies even in freestanding builds. */
void __aeabi_memcpy(void *destination, const void *source, size_t length)
{
    (void)memcpy(destination, source, length);
}

void __aeabi_memcpy4(void *destination, const void *source, size_t length)
{
    (void)memcpy(destination, source, length);
}

void __aeabi_memcpy8(void *destination, const void *source, size_t length)
{
    (void)memcpy(destination, source, length);
}

void __aeabi_memmove(void *destination, const void *source, size_t length)
{
    (void)memmove(destination, source, length);
}

void __aeabi_memset(void *destination, size_t length, int value)
{
    (void)memset(destination, value, length);
}

void __aeabi_memclr(void *destination, size_t length)
{
    (void)memset(destination, 0, length);
}

void __aeabi_memclr4(void *destination, size_t length)
{
    (void)memset(destination, 0, length);
}

void __aeabi_memclr8(void *destination, size_t length)
{
    (void)memset(destination, 0, length);
}

void __aeabi_memset4(void *destination, size_t length, int value)
{
    (void)memset(destination, value, length);
}

void __aeabi_memset8(void *destination, size_t length, int value)
{
    (void)memset(destination, value, length);
}
