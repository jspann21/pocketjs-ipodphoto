#ifndef POCKETJS_IPOD_PHOTO_QJS_RUNTIME_H
#define POCKETJS_IPOD_PHOTO_QJS_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "core_bridge.h"

#define PJS_QJS_ERROR_NONE 0u
#define PJS_QJS_ERROR_RUNTIME 1u
#define PJS_QJS_ERROR_CONTEXT 2u
#define PJS_QJS_ERROR_HOST 3u
#define PJS_QJS_ERROR_EVAL 4u
#define PJS_QJS_ERROR_FRAME_EXPORT 5u
#define PJS_QJS_ERROR_FRAME_CALL 6u
#define PJS_QJS_ERROR_PENDING_JOB 7u
#define PJS_QJS_ERROR_FRAME_BUDGET 8u
#define PJS_QJS_ERROR_BOOT_BUDGET 9u

typedef struct {
    uint32_t runtime_us;
    uint32_t context_us;
    uint32_t host_us;
    uint32_t native_pak_us;
    uint32_t eval_us;
    uint32_t jobs_us;
    uint32_t allocation_calls;
    uint32_t usb_service_us;
    uint32_t clock_source;
    uint32_t pll_control;
    uint32_t memory_timing;
    uint32_t device_init;
} PjsQjsBootProfile;

bool qjs_runtime_boot(const PjsGuestPackage *guest);
void qjs_runtime_enable_fs_mount(void);
bool qjs_runtime_frame(const PjsCoreInput *input);
bool qjs_runtime_set_launcher_catalog(const uint8_t *labels,
                                      uint32_t count, uint32_t stride);
int32_t qjs_runtime_launcher_selection(void);
uint32_t qjs_runtime_error_code(void);
const char *qjs_runtime_error_text(uint32_t *length);
const PjsQjsBootProfile *qjs_runtime_boot_profile(void);
void qjs_runtime_shutdown(void);

#endif
