#ifndef POCKETJS_IPOD_PHOTO_NATIVE_LOADER_H
#define POCKETJS_IPOD_PHOTO_NATIVE_LOADER_H

#include <stdbool.h>
#include <stdint.h>

/* Admit a flat, unwrapped A1099 image before the terminal chainload path. */
bool pjs_native_image_admissible(const uint8_t *image, uint32_t length);

/* The caller must stop the guest and USB transport before entering here. */
void pjs_native_image_chainload(const uint8_t *image, uint32_t length)
    __attribute__((noreturn));

#endif
