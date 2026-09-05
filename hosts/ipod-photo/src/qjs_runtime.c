#include "qjs_runtime.h"
#include "storage.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include "heap.h"
#include "quickjs.h"
#include "timer.h"
#if defined(__arm__)
#include "pp5020.h"
#endif
#include "usb_device.h"
#include "audio_pcm.h"
#include "fs_store.h"

#define PJS_QJS_MEMORY_LIMIT (6u * 1024u * 1024u)
#define PJS_QJS_GC_THRESHOLD (512u * 1024u)
#define PJS_QJS_STACK_LIMIT (192u * 1024u)
/* Ordinary compiler bundles are much larger than the hand-written recovery
 * guest.  Preserve a hard boot bound, but allow the 80 MHz PP5020 enough time
 * to parse, mount styles/fonts, and create the first retained tree. */
/* Generated framework guests do substantially more parser/evaluator work on
 * the uncached 80 MHz ARM7 than the small recovery bundle.  Native host-call
 * time is excluded separately; retain a finite two-minute guest-CPU ceiling
 * so a malformed top-level script still cannot wedge standalone boot. */
#define PJS_QJS_BOOT_BUDGET_US 120000000u
/* The first physical A1099 HostOps candidate tripped the 100 ms watchdog on
 * the Hold edge while the guest performed retained-UI mutations. Keep frames
 * bounded, but leave enough PP5020 headroom for an interactive mutation burst.
 * Boot has a separate, deliberately larger bound because it parses the whole
 * generated framework bundle and installs baked assets. */
#define PJS_QJS_FRAME_BUDGET_US 250000u
#define PJS_QJS_MAX_PENDING_JOBS 64u
#define PJS_QJS_MALLOC_OVERHEAD 8u
#define PJS_QJS_ROOT_ID 1
#define PJS_QJS_HOST_ABI 1
#define PJS_QJS_MAX_LAUNCHER_APPS PJS_STORAGE_MAX_APPS
#define PJS_QJS_LAUNCHER_LABEL_BYTES 9u

/* The A1099 input decoder intentionally uses a compact, device-local bit
 * layout (input.h) for the native diagnostic screen.  The guest never sees
 * those bits: generated PocketJS apps consume the portable BTN mask from
 * contracts/spec/spec.ts.  Keep the values here in lock-step with that table
 * without making the freestanding target depend on generated TypeScript. */
#define PJS_FRAME_BTN_SELECT   0x0001u
#define PJS_FRAME_BTN_START    0x0008u
#define PJS_FRAME_BTN_RIGHT    0x0020u
#define PJS_FRAME_BTN_LEFT     0x0080u
#define PJS_FRAME_BTN_TRIANGLE 0x1000u
#define PJS_FRAME_BTN_CIRCLE   0x2000u
#define PJS_FRAME_ANALOG_CENTER 0x8080u

/* These are deliberately the first PocketJS HostOps required by the embedded
 * recovery guest. The ABI surface grows append-only toward the complete host
 * contract; none of these calls know about the LCD or JavaScript runtime. */
typedef enum {
    HOST_CREATE_NODE = 1,
    HOST_DESTROY_NODE,
    HOST_INSERT_BEFORE,
    HOST_REMOVE_CHILD,
    HOST_SET_STYLE,
    HOST_SET_PROP,
    HOST_SET_PROP_BATCH,
    HOST_SET_TEXT,
    HOST_REPLACE_TEXT,
    HOST_UPLOAD_TEXTURE,
    HOST_SET_IMAGE,
    HOST_SET_SPRITE,
    HOST_ANIMATE,
    HOST_CANCEL_ANIM,
    HOST_SET_FOCUS,
    HOST_SET_ACTIVE,
    HOST_LOAD_STYLES,
    HOST_LOAD_FONT_ATLAS,
    HOST_MEASURE_TEXT,
    HOST_FREE_TEXTURE,
    HOST_UPLOAD_IMG_ENTRY,
} HostOperation;

static JSRuntime *runtime;
static JSContext *context;
static JSValue global_value;
static JSValue frame_function;
/* Host-owned, non-portable input facts used only by the embedded recovery
 * guest.  Standard generated apps receive input through frame() and should
 * never need this object.  Keeping it under a target-prefixed name prevents
 * accidental collision with the framework namespace. */
static JSValue ipod_input_value;
static JSValue launcher_value;
static uint8_t launcher_labels[PJS_QJS_MAX_LAUNCHER_APPS]
                              [PJS_QJS_LAUNCHER_LABEL_BYTES];
static uint32_t launcher_count;
static uint32_t error_code;
static char error_text[128];
static uint32_t error_text_length;
static uint32_t execution_deadline;
static bool execution_timed_out;
#define PJS_WHEEL_DETENT_UNITS 8
static int32_t pending_wheel_units;
static bool wheel_release_pending;
/* Package storage remains pinned until runtime shutdown. Keep only its
 * validated app identity here; applications that never call fs need no ATA. */
static const uint8_t *fs_app_id;
static uint32_t fs_app_id_length;
static bool fs_open_attempted;
static int fs_open_result;
/* Chainloaded development images must enumerate before touching FAT/ATA.
 * The host enables persistent mounts once USB hardware is alive; packages
 * subsequently delivered over the service then receive the real store. */
static bool fs_mount_enabled;
/* The first A1099 package used a seven-argument, device-local frame callback.
 * Keep that package runnable while all new/generated guests use the standard
 * frame(buttons, analog?, touches?, hits?, touchSurfaces?) shape. */
static bool legacy_frame_abi;
static PjsQjsBootProfile boot_profile;
static bool boot_profile_active;

const PjsQjsBootProfile *qjs_runtime_boot_profile(void)
{
    return &boot_profile;
}

void qjs_runtime_enable_fs_mount(void)
{
    fs_mount_enabled = true;
}

bool qjs_runtime_set_launcher_catalog(const uint8_t *labels,
                                      uint32_t count, uint32_t stride)
{
    if (count > PJS_QJS_MAX_LAUNCHER_APPS ||
        (count != 0u && (labels == 0 || stride < PJS_QJS_LAUNCHER_LABEL_BYTES))) {
        return false;
    }
    launcher_count = count;
    for (uint32_t app = 0u; app < PJS_QJS_MAX_LAUNCHER_APPS; ++app) {
        for (uint32_t index = 0u; index < PJS_QJS_LAUNCHER_LABEL_BYTES; ++index) {
            launcher_labels[app][index] = 0u;
        }
        if (app >= count) continue;
        const uint8_t *source = labels + app * stride;
        for (uint32_t index = 0u; index < PJS_QJS_LAUNCHER_LABEL_BYTES; ++index) {
            launcher_labels[app][index] = source[index];
        }
        launcher_labels[app][PJS_QJS_LAUNCHER_LABEL_BYTES - 1u] = 0u;
    }
    return true;
}

static void service_usb(void);

static size_t qjs_malloc_usable_size(const void *pointer)
{
    return pjs_heap_usable_size(pointer);
}

static void *qjs_malloc(JSMallocState *state, size_t size)
{
    if (boot_profile_active) ++boot_profile.allocation_calls;
    service_usb();
    /* Include bookkeeping in the admission check, as in malloc_size, so an
     * allocation at the limit cannot exceed it by the surcharge. */
    if (size == 0u || size > SIZE_MAX - PJS_QJS_MALLOC_OVERHEAD ||
        size + PJS_QJS_MALLOC_OVERHEAD > state->malloc_limit ||
        state->malloc_size > state->malloc_limit -
            (size + PJS_QJS_MALLOC_OVERHEAD)) {
        return 0;
    }
    void *pointer = pjs_heap_alloc(size, 16u);
    if (pointer == 0) return 0;
    size_t usable = pjs_heap_usable_size(pointer);
    if (usable > SIZE_MAX - PJS_QJS_MALLOC_OVERHEAD ||
        usable + PJS_QJS_MALLOC_OVERHEAD > state->malloc_limit ||
        state->malloc_size > state->malloc_limit -
            (usable + PJS_QJS_MALLOC_OVERHEAD)) {
        pjs_heap_free(pointer);
        return 0;
    }
    ++state->malloc_count;
    state->malloc_size += usable + PJS_QJS_MALLOC_OVERHEAD;
    return pointer;
}

static void qjs_free(JSMallocState *state, void *pointer)
{
    if (boot_profile_active) ++boot_profile.allocation_calls;
    service_usb();
    if (pointer == 0) return;
    size_t usable = pjs_heap_usable_size(pointer);
    size_t accounted = usable > SIZE_MAX - PJS_QJS_MALLOC_OVERHEAD
        ? SIZE_MAX : usable + PJS_QJS_MALLOC_OVERHEAD;
    if (state->malloc_count != 0u) --state->malloc_count;
    if (state->malloc_size >= accounted) state->malloc_size -= accounted;
    else state->malloc_size = 0u;
    pjs_heap_free(pointer);
}

static void *qjs_realloc(JSMallocState *state, void *pointer, size_t size)
{
    if (boot_profile_active) ++boot_profile.allocation_calls;
    service_usb();
    if (pointer == 0) return size == 0u ? 0 : qjs_malloc(state, size);
    size_t old_size = pjs_heap_usable_size(pointer);
    if (size == 0u) {
        qjs_free(state, pointer);
        return 0;
    }
    size_t old_accounted = old_size > SIZE_MAX - PJS_QJS_MALLOC_OVERHEAD
        ? SIZE_MAX : old_size + PJS_QJS_MALLOC_OVERHEAD;
    size_t new_accounted = size > SIZE_MAX - PJS_QJS_MALLOC_OVERHEAD
        ? SIZE_MAX : size + PJS_QJS_MALLOC_OVERHEAD;
    if (old_accounted == SIZE_MAX || new_accounted > state->malloc_limit ||
        state->malloc_size < old_accounted ||
        state->malloc_size - old_accounted >
            state->malloc_limit - new_accounted) {
        return 0;
    }
    void *replacement = pjs_heap_realloc(pointer, size, 16u);
    if (replacement == 0) return 0;
    size_t new_size = pjs_heap_usable_size(replacement);
    if (new_size >= old_size) state->malloc_size += new_size - old_size;
    else state->malloc_size -= old_size - new_size;
    return replacement;
}

static const JSMallocFunctions qjs_allocators = {
    .js_malloc = qjs_malloc,
    .js_free = qjs_free,
    .js_realloc = qjs_realloc,
    .js_malloc_usable_size = qjs_malloc_usable_size,
};

static void execution_budget_start(uint32_t budget_us)
{
    execution_timed_out = false;
    execution_deadline = timer_now_us() + budget_us;
}

static void execution_budget_stop(void)
{
    execution_deadline = 0u;
}

static uint32_t execution_budget_pause(void)
{
    return execution_deadline == 0u ? 0u : timer_now_us();
}

static void execution_budget_resume(uint32_t paused_at)
{
    if (paused_at != 0u && execution_deadline != 0u) {
        execution_deadline += timer_now_us() - paused_at;
    }
}
/* Argument conversion remains inside the guest budget; only the already
 * converted native bridge call is excluded from that budget. */
#define PJS_UI_VOID(call) do { \
    uint32_t pjs_budget_paused = execution_budget_pause(); \
    call; \
    execution_budget_resume(pjs_budget_paused); \
} while (0)
#define PJS_UI_I32(call) ({ \
    uint32_t pjs_budget_paused = execution_budget_pause(); \
    int32_t pjs_result = (call); \
    execution_budget_resume(pjs_budget_paused); \
    pjs_result; \
})

static void service_usb(void)
{
    uint32_t paused_at = execution_budget_pause();
    uint32_t started = boot_profile_active ? timer_now_us() : 0u;
    pjs_usb_device_poll_cooperative();
    if (boot_profile_active) boot_profile.usb_service_us +=
        timer_now_us() - started;
    execution_budget_resume(paused_at);
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    (void)opaque;
    service_usb();
    bool expired = execution_deadline != 0u &&
                   (int32_t)(timer_now_us() - execution_deadline) >= 0;
    /* QuickJS invokes this on the main thread, not from a hardware IRQ.
     * Keep native PCM cooperative service alive inside a long guest turn
     * without relaxing the JavaScript execution budget. */
    if (!expired) {
        uint32_t paused_at = execution_budget_pause();
        pjs_audio_pcm_service();
        execution_budget_resume(paused_at);
        expired = execution_deadline != 0u &&
                  (int32_t)(timer_now_us() - execution_deadline) >= 0;
    }
    if (expired) execution_timed_out = true;
    return expired ? 1 : 0;
}

static int argument_i32(JSContext *ctx, int argc, JSValueConst *argv,
                        int index, int32_t *value)
{
    if (index >= argc) {
        *value = 0;
        return 0;
    }
    return JS_ToInt32(ctx, value, argv[index]);
}

static int argument_u32(JSContext *ctx, int argc, JSValueConst *argv,
                        int index, uint32_t *value)
{
    if (index >= argc) {
        *value = 0u;
        return 0;
    }
    return JS_ToUint32(ctx, value, argv[index]);
}

static int argument_f64(JSContext *ctx, int argc, JSValueConst *argv,
                        int index, double *value)
{
    if (index >= argc) {
        *value = 0.0;
        return 0;
    }
    return JS_ToFloat64(ctx, value, argv[index]);
}

/* Return 1 for an ArrayBuffer/typed-array view, 0 for an invalid value, and
 * -1 when the argument was omitted. Invalid values are deliberately treated
 * as a no-op/failed upload by this non-strict native host; they must not leave
 * a stale QuickJS exception behind. The borrowed pointer is consumed before
 * this operation returns. */
static int argument_bytes(JSContext *ctx, int argc, JSValueConst *argv,
                          int index, const uint8_t **bytes, size_t *length)
{
    if (index >= argc) return -1;

    size_t direct_length = 0u;
    uint8_t *direct = JS_GetArrayBuffer(ctx, &direct_length, argv[index]);
    if (!JS_HasException(ctx)) {
        *bytes = direct;
        *length = direct_length;
        return 1;
    }
    JSValue direct_error = JS_GetException(ctx);
    JS_FreeValue(ctx, direct_error);

    size_t offset = 0u;
    size_t view_length = 0u;
    size_t bytes_per_element = 0u;
    JSValue buffer = JS_GetTypedArrayBuffer(
        ctx, argv[index], &offset, &view_length, &bytes_per_element);
    if (JS_IsException(buffer)) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        return 0;
    }
    size_t buffer_length = 0u;
    uint8_t *base = JS_GetArrayBuffer(ctx, &buffer_length, buffer);
    if (JS_HasException(ctx)) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, buffer);
        return 0;
    }
    if (offset > buffer_length || view_length > buffer_length - offset) {
        JS_FreeValue(ctx, buffer);
        return 0;
    }
    (void)bytes_per_element;
    *bytes = base == 0 ? 0 : base + offset;
    *length = view_length;
    JS_FreeValue(ctx, buffer);
    return 1;
}

/* QuickJS exposes strings as temporary UTF-8 allocations. The core copies
 * text into its retained tree before the string is released. */
static int argument_string(JSContext *ctx, int argc, JSValueConst *argv,
                           int index, const char **text, size_t *length)
{
    if (index >= argc) return -1;
    *text = JS_ToCStringLen2(ctx, length, argv[index], 0);
    if (*text != 0) return 1;
    JSValue error = JS_GetException(ctx);
    JS_FreeValue(ctx, error);
    return 0;
}

/* Translate the local A1099 button packet into the portable PocketJS BTN
 * mask.  The centre/select key is the ordinary confirm action (CIRCLE), the
 * menu key is the ordinary cancel/back action (TRIANGLE), and play/pause is
 * START.  Wheel motion is a one-frame directional pulse; its absolute
 * position remains a target-local recovery fact in ui.__ipodInput. */
static uint32_t guest_buttons(const PjsCoreInput *input)
{
    uint32_t buttons = 0u;
    if ((input->buttons & PJS_BUTTON_SELECT) != 0u) {
        buttons |= PJS_FRAME_BTN_CIRCLE;
    }
    if ((input->buttons & PJS_BUTTON_RIGHT) != 0u) {
        buttons |= PJS_FRAME_BTN_RIGHT;
    }
    if ((input->buttons & PJS_BUTTON_LEFT) != 0u) {
        buttons |= PJS_FRAME_BTN_LEFT;
    }
    if ((input->buttons & PJS_BUTTON_PLAY) != 0u) {
        buttons |= PJS_FRAME_BTN_START;
    }
    if ((input->buttons & PJS_BUTTON_MENU) != 0u) {
        buttons |= PJS_FRAME_BTN_TRIANGLE;
    }
    /* The controller reports signed motion in 96 units per revolution, while
     * onButtonPress observes rising edges. Accumulate fractional motion into
     * eight-unit detents, emit at most one edge per guest frame, and clear
     * residue when the finger leaves the wheel. Physical Left/Right remain
     * held exactly as reported by the button packet. */
    if (input->hold) {
        pending_wheel_units = 0;
        wheel_release_pending = false;
    } else if (input->wheel_touched) {
        pending_wheel_units += input->wheel_delta;
    } else {
        pending_wheel_units = 0;
    }
    if (wheel_release_pending) {
        wheel_release_pending = false;
    } else if (pending_wheel_units >= PJS_WHEEL_DETENT_UNITS ||
               pending_wheel_units <= -PJS_WHEEL_DETENT_UNITS) {
        int32_t direction = pending_wheel_units > 0 ? 1 : -1;
        /* Emit one edge, then retain only the fractional remainder. */
        pending_wheel_units %= PJS_WHEEL_DETENT_UNITS;
        buttons |= direction > 0 ?
            PJS_FRAME_BTN_RIGHT : PJS_FRAME_BTN_LEFT;
        wheel_release_pending = true;
    }
    return buttons;
}

/* Keep the old recovery screen's wheel/Hold/power proof available without
 * making those device-specific words part of the generated-app frame ABI.
 * Values are replaced before every guest turn; the object itself is stable so
 * a recovery guest can retain a reference to it safely. */
static int update_ipod_input(const PjsCoreInput *input, uint32_t buttons)
{
    if (JS_SetPropertyStr(context, ipod_input_value, "buttons",
                          JS_NewUint32(context, buttons)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "nativeButtons",
                          JS_NewUint32(context, input->buttons)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "analog",
                          JS_NewUint32(context, PJS_FRAME_ANALOG_CENTER)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "wheelDelta",
                          JS_NewInt32(context, input->wheel_delta)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "wheelPosition",
                          JS_NewUint32(context, input->wheel_position)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "wheelTouched",
                          JS_NewBool(context, input->wheel_touched != 0u)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "hold",
                          JS_NewBool(context, input->hold != 0u)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "batteryMv",
                          JS_NewUint32(context, input->battery_mv)) < 0 ||
        JS_SetPropertyStr(context, ipod_input_value, "powerFlags",
                          JS_NewUint32(context, input->power_flags)) < 0) {
        return -1;
    }
    return 0;
}

static JSValue host_operation(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv, int magic)
{
    (void)this_value;
    int32_t a = 0;
    int32_t b = 0;
    int32_t c = 0;
    uint32_t ua = 0u;
    uint32_t ub = 0u;
    uint32_t uc = 0u;
    int bytes_result = 0;
    int text_result = 0;
    const uint8_t *bytes = 0;
    size_t bytes_length = 0u;
    const char *text = 0;
    size_t text_length = 0u;
    double value = 0.0;

    switch ((HostOperation)magic) {
    case HOST_CREATE_NODE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0) return JS_EXCEPTION;
        return JS_NewInt32(ctx, PJS_UI_I32(pjs_ui_create_node((uint32_t)a)));
    case HOST_DESTROY_NODE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0) return JS_EXCEPTION;
        PJS_UI_VOID(pjs_ui_destroy_node(a));
        return JS_UNDEFINED;
    case HOST_INSERT_BEFORE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0 ||
            argument_i32(ctx, argc, argv, 2, &c) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_insert_before(a, b, c));
        return JS_UNDEFINED;
    case HOST_REMOVE_CHILD:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_remove_child(a, b));
        return JS_UNDEFINED;
    case HOST_SET_STYLE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_set_style(a, b));
        return JS_UNDEFINED;
    case HOST_SET_PROP:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0 ||
            argument_f64(ctx, argc, argv, 2, &value) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_set_prop(a, (uint32_t)b, value));
        return JS_UNDEFINED;
    case HOST_SET_PROP_BATCH:
        bytes_result = argument_bytes(ctx, argc, argv, 0, &bytes, &bytes_length);
        if (bytes_result == 1) PJS_UI_VOID(pjs_ui_set_prop_batch(bytes, bytes_length));
        return JS_UNDEFINED;
    case HOST_SET_TEXT:
    case HOST_REPLACE_TEXT:
        text_result = argument_string(ctx, argc, argv, 1, &text, &text_length);
        if (text_result == 1) {
            if (argument_i32(ctx, argc, argv, 0, &a) < 0) {
                JS_FreeCString(ctx, text);
                return JS_EXCEPTION;
            }
            if (magic == HOST_SET_TEXT) {
                PJS_UI_VOID(pjs_ui_set_text(a, (const uint8_t *)text, text_length));
            } else {
                PJS_UI_VOID(pjs_ui_replace_text(a, (const uint8_t *)text, text_length));
            }
            JS_FreeCString(ctx, text);
        }
        return JS_UNDEFINED;
    case HOST_UPLOAD_TEXTURE:
        bytes_result = argument_bytes(ctx, argc, argv, 0, &bytes, &bytes_length);
        if (bytes_result != 1) return JS_NewInt32(ctx, -1);
        if (argument_u32(ctx, argc, argv, 1, &ua) < 0 ||
            argument_u32(ctx, argc, argv, 2, &ub) < 0 ||
            argument_u32(ctx, argc, argv, 3, &uc) < 0) {
            return JS_EXCEPTION;
        }
        return JS_NewInt32(ctx, PJS_UI_I32(pjs_ui_upload_texture(bytes, bytes_length, ua, ub, uc)));
    case HOST_SET_IMAGE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_set_image(a, b));
        return JS_UNDEFINED;
    case HOST_SET_SPRITE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0 ||
            argument_u32(ctx, argc, argv, 2, &ua) < 0 ||
            argument_u32(ctx, argc, argv, 3, &ub) < 0 ||
            argument_u32(ctx, argc, argv, 4, &uc) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_set_sprite(a, b, ua, ub, uc));
        return JS_UNDEFINED;
    case HOST_ANIMATE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_u32(ctx, argc, argv, 1, &ua) < 0 ||
            argument_f64(ctx, argc, argv, 2, &value) < 0 ||
            argument_u32(ctx, argc, argv, 4, &ub) < 0) {
            return JS_EXCEPTION;
        }
        if (argument_i32(ctx, argc, argv, 3, &b) < 0 ||
            argument_i32(ctx, argc, argv, 5, &c) < 0) {
            return JS_EXCEPTION;
        }
        return JS_NewInt32(ctx, PJS_UI_I32(pjs_ui_animate(
            a, ua, value, b < 0 ? 0u : (uint32_t)b, ub,
            c < 0 ? 0u : (uint32_t)c)));
    case HOST_CANCEL_ANIM:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0) return JS_EXCEPTION;
        PJS_UI_VOID(pjs_ui_cancel_anim(a));
        return JS_UNDEFINED;
    case HOST_SET_FOCUS:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0) return JS_EXCEPTION;
        PJS_UI_VOID(pjs_ui_set_focus(a));
        return JS_UNDEFINED;
    case HOST_SET_ACTIVE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0 ||
            argument_i32(ctx, argc, argv, 1, &b) < 0) {
            return JS_EXCEPTION;
        }
        PJS_UI_VOID(pjs_ui_set_active(a, b));
        return JS_UNDEFINED;
    case HOST_LOAD_STYLES:
    case HOST_LOAD_FONT_ATLAS:
        bytes_result = argument_bytes(ctx, argc, argv, 0, &bytes, &bytes_length);
        if (bytes_result != 1) return JS_NewBool(ctx, 0);
        return JS_NewBool(ctx, magic == HOST_LOAD_STYLES ?
            PJS_UI_I32(pjs_ui_load_styles(bytes, bytes_length)) != 0 :
            PJS_UI_I32(pjs_ui_load_font_atlas(bytes, bytes_length)) != 0);
    case HOST_MEASURE_TEXT:
        text_result = argument_string(ctx, argc, argv, 0, &text, &text_length);
        if (text_result != 1) return JS_NewFloat64(ctx, 0.0);
        if (argument_u32(ctx, argc, argv, 1, &ua) < 0) {
            JS_FreeCString(ctx, text);
            return JS_EXCEPTION;
        }
        uint32_t pjs_budget_paused = execution_budget_pause();
        value = (double)pjs_ui_measure_text(
            (const uint8_t *)text, text_length, ua);
        execution_budget_resume(pjs_budget_paused);
        JS_FreeCString(ctx, text);
        return JS_NewFloat64(ctx, value);
    case HOST_FREE_TEXTURE:
        if (argument_i32(ctx, argc, argv, 0, &a) < 0) return JS_EXCEPTION;
        PJS_UI_VOID(pjs_ui_free_texture(a));
        return JS_UNDEFINED;
    case HOST_UPLOAD_IMG_ENTRY:
        bytes_result = argument_bytes(ctx, argc, argv, 0, &bytes, &bytes_length);
        if (bytes_result != 1) return JS_NewInt32(ctx, -1);
        return JS_NewInt32(ctx, PJS_UI_I32(pjs_ui_upload_img_entry(bytes, bytes_length)));
    default:
        return JS_ThrowInternalError(ctx, "unknown PocketJS HostOp");
    }
}

typedef enum {
    AUDIO_CREATE_STREAM = 1,
    AUDIO_DESTROY_STREAM,
    AUDIO_WRITE_PCM,
    AUDIO_PLAY,
    AUDIO_PAUSE,
    AUDIO_STOP,
    AUDIO_SET_VOLUME,
    AUDIO_END_STREAM,
    AUDIO_POLL,
} AudioOperation;

static JSValue audio_operation(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv, int magic)
{
    (void)this_value;
    int32_t handle = 0;
    uint32_t rate = 0u;
    uint32_t channels = 0u;
    double volume = 0.0;
    const uint8_t *bytes = 0;
    size_t length = 0u;
    uint32_t paused_at;
    int32_t created;

    switch ((AudioOperation)magic) {
    case AUDIO_CREATE_STREAM:
        if (argument_u32(ctx, argc, argv, 0, &rate) < 0 ||
            argument_u32(ctx, argc, argv, 1, &channels) < 0) {
            return JS_EXCEPTION;
        }
        paused_at = execution_budget_pause();
        created = pjs_audio_pcm_create_stream(rate, channels);
        execution_budget_resume(paused_at);
        return JS_NewInt32(ctx, created);
    case AUDIO_DESTROY_STREAM:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        paused_at = execution_budget_pause();
        pjs_audio_pcm_destroy_stream(handle);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_WRITE_PCM: {
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        int result = argument_bytes(ctx, argc, argv, 1, &bytes, &length);
        if (result != 1) return JS_NewInt32(ctx, 0);
        paused_at = execution_budget_pause();
        uint32_t accepted = pjs_audio_pcm_write_bytes(handle, bytes, length);
        execution_budget_resume(paused_at);
        return JS_NewInt32(ctx, accepted > INT32_MAX ? INT32_MAX :
                                                (int32_t)accepted);
    }
    case AUDIO_PLAY:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        /* DMA boundaries and codec waits have their own native deadlines.
         * Charge the JS watchdog for guest work, as with filesystem calls. */
        paused_at = execution_budget_pause();
        pjs_audio_pcm_play(handle);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_PAUSE:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        paused_at = execution_budget_pause();
        pjs_audio_pcm_pause(handle);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_STOP:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        paused_at = execution_budget_pause();
        pjs_audio_pcm_stop(handle);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_SET_VOLUME:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0 ||
            argument_f64(ctx, argc, argv, 1, &volume) < 0) {
            return JS_EXCEPTION;
        }
        paused_at = execution_budget_pause();
        pjs_audio_pcm_set_volume(handle, volume);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_END_STREAM:
        if (argument_i32(ctx, argc, argv, 0, &handle) < 0) return JS_EXCEPTION;
        paused_at = execution_budget_pause();
        pjs_audio_pcm_end_stream(handle);
        execution_budget_resume(paused_at);
        return JS_UNDEFINED;
    case AUDIO_POLL: {
        paused_at = execution_budget_pause();
        const char *event = pjs_audio_pcm_poll();
        execution_budget_resume(paused_at);
        return event == 0 ? JS_UNDEFINED : JS_NewString(ctx, event);
    }
    default:
        return JS_ThrowInternalError(ctx, "unknown PocketJS audio op");
    }
}

static int add_audio_operation(JSValueConst object, const char *name, int arity,
                               AudioOperation operation)
{
    JSValue function = JS_NewCFunctionMagic(
        context, audio_operation, name, arity, JS_CFUNC_generic_magic,
        (int)operation);
    if (JS_IsException(function)) return -1;
    return JS_SetPropertyStr(context, object, name, function);
}

static int install_audio(void)
{
    JSValue audio = JS_NewObject(context);
    if (JS_IsException(audio)) return -1;
    if (add_audio_operation(audio, "createStream", 2, AUDIO_CREATE_STREAM) < 0 ||
        add_audio_operation(audio, "destroyStream", 1, AUDIO_DESTROY_STREAM) < 0 ||
        add_audio_operation(audio, "writePcm", 2, AUDIO_WRITE_PCM) < 0 ||
        add_audio_operation(audio, "play", 1, AUDIO_PLAY) < 0 ||
        add_audio_operation(audio, "pause", 1, AUDIO_PAUSE) < 0 ||
        add_audio_operation(audio, "stop", 1, AUDIO_STOP) < 0 ||
        add_audio_operation(audio, "setVolume", 2, AUDIO_SET_VOLUME) < 0 ||
        add_audio_operation(audio, "endStream", 1, AUDIO_END_STREAM) < 0 ||
        add_audio_operation(audio, "poll", 0, AUDIO_POLL) < 0) {
        JS_FreeValue(context, audio);
        return -1;
    }
    /* JS_SetPropertyStr consumes audio on either result. */
    return JS_SetPropertyStr(context, global_value, "audio", audio);
}

/* The store owns the filesystem policy and encoding.  The bridge deliberately
 * only deals in JSON lines, so the native implementation can use its own
 * bounded scratch/allocator and remain independent of QuickJS internals. */
#define PJS_FS_RESULT_BYTES (96u * 1024u)
#define PJS_FS_MAX_PATH_BYTES 160u
static char fs_result[PJS_FS_RESULT_BYTES];

typedef enum {
    FS_READ = 1, FS_WRITE, FS_REMOVE, FS_LIST, FS_STAT,
    FS_MKDIR, FS_RENAME, FS_USAGE, FS_LAST_ERROR,
} FsOperation;

static JSValue fs_json_result(int rc, uint32_t length)
{
    if (rc < 0) {
        static const char error_json[] = "{\"error\":\"fs operation failed\"}";
        return JS_NewString(context, error_json);
    }
    if (length >= PJS_FS_RESULT_BYTES) {
        return JS_ThrowInternalError(context, "fs result too large");
    }
    return JS_NewStringLen(context, fs_result, length);
}

static JSValue fs_operation(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv, int magic)
{
    (void)this_value;
    const char *path = 0;
    const char *to = 0;
    const char *payload = 0;
    size_t path_length = 0u;
    size_t to_length = 0u;
    size_t payload_length = 0u;
    uint32_t offset = 0u;
    uint32_t limit = 0u;
    int32_t mode = 0;
    int32_t recursive = 0;
    uint32_t length = 0u;
    int rc = -1;
    bool invalid_path = false;
    uint32_t budget_paused_at = 0u;

    if (magic != FS_USAGE && magic != FS_LAST_ERROR) {
        if (argument_string(ctx, argc, argv, 0, &path, &path_length) != 1)
            return JS_ThrowTypeError(ctx, "fs path must be a string");
        if (path_length > PJS_FS_MAX_PATH_BYTES ||
            memchr(path, '\0', path_length) != 0) {
            invalid_path = true;
        }
    }
    if (invalid_path) goto done;
    /* The QuickJS watchdog bounds guest CPU execution, not time spent waiting
     * in a synchronous native host service.  ATA/FAT operations have their
     * own bounded hardware timeouts; excluding that elapsed time prevents a
     * valid filesystem transaction from surfacing as a JS infinite loop. */
    budget_paused_at = execution_budget_pause();
    bool can_mount = true;
    if (magic != FS_LAST_ERROR) {
        if (!can_mount) goto done;
        if (!fs_open_attempted) {
            fs_open_attempted = true;
            fs_open_result = pjs_fs_store_open(fs_app_id, fs_app_id_length);
        }
        if (fs_open_result < 0) {
            rc = fs_open_result;
            goto done;
        }
    }
    switch ((FsOperation)magic) {
    case FS_READ:
        if (argument_u32(ctx, argc, argv, 1, &offset) < 0 ||
            argument_u32(ctx, argc, argv, 2, &limit) < 0) goto done;
        rc = pjs_fs_read_json(path, offset, limit, fs_result,
                              PJS_FS_RESULT_BYTES, &length);
        break;
    case FS_WRITE:
        if (argument_string(ctx, argc, argv, 1,
                            &payload, &payload_length) != 1) goto done;
        if (argument_i32(ctx, argc, argv, 2, &mode) < 0) goto done;
        rc = pjs_fs_write(path, payload, payload_length, mode);
        break;
    case FS_REMOVE:
        if (argument_i32(ctx, argc, argv, 1, &recursive) < 0) goto done;
        rc = pjs_fs_remove(path, recursive);
        break;
    case FS_LIST:
        if (argument_u32(ctx, argc, argv, 1, &offset) < 0) goto done;
        rc = pjs_fs_list_json(path, offset, fs_result,
                              PJS_FS_RESULT_BYTES, &length);
        break;
    case FS_STAT:
        rc = pjs_fs_stat_json(path, fs_result, PJS_FS_RESULT_BYTES, &length);
        break;
    case FS_MKDIR:
        rc = pjs_fs_mkdir(path);
        break;
    case FS_RENAME:
        if (argument_string(ctx, argc, argv, 1, &to, &to_length) != 1) goto done;
        if (to_length > PJS_FS_MAX_PATH_BYTES ||
            memchr(to, '\0', to_length) != 0) goto done;
        rc = pjs_fs_rename(path, to);
        break;
    case FS_USAGE:
        rc = pjs_fs_usage_json(fs_result, PJS_FS_RESULT_BYTES, &length);
        break;
    case FS_LAST_ERROR:
        rc = pjs_fs_last_error(fs_result, PJS_FS_RESULT_BYTES, &length);
        break;
    default:
        return JS_ThrowInternalError(ctx, "unknown fs operation");
    }
done:
    execution_budget_resume(budget_paused_at);
    if (payload != 0) JS_FreeCString(ctx, payload);
    if (path != 0) JS_FreeCString(ctx, path);
    if (to != 0) JS_FreeCString(ctx, to);
    if (magic == FS_WRITE || magic == FS_REMOVE || magic == FS_MKDIR ||
        magic == FS_RENAME) {
        return JS_NewInt32(ctx, rc < 0 ? 1 : 0);
    }
    return fs_json_result(rc, length);
}

static int add_fs_operation(JSValueConst object, const char *name, int arity,
                            FsOperation operation)
{
    JSValue function = JS_NewCFunctionMagic(
        context, fs_operation, name, arity, JS_CFUNC_generic_magic,
        (int)operation);
    if (JS_IsException(function)) return -1;
    return JS_SetPropertyStr(context, object, name, function);
}

static int install_fs(void)
{
    JSValue fs = JS_NewObject(context);
    if (JS_IsException(fs)) return -1;
    if (add_fs_operation(fs, "read", 3, FS_READ) < 0 ||
        add_fs_operation(fs, "write", 3, FS_WRITE) < 0 ||
        add_fs_operation(fs, "remove", 2, FS_REMOVE) < 0 ||
        add_fs_operation(fs, "list", 2, FS_LIST) < 0 ||
        add_fs_operation(fs, "stat", 1, FS_STAT) < 0 ||
        add_fs_operation(fs, "mkdir", 1, FS_MKDIR) < 0 ||
        add_fs_operation(fs, "rename", 2, FS_RENAME) < 0 ||
        add_fs_operation(fs, "usage", 0, FS_USAGE) < 0 ||
        add_fs_operation(fs, "lastError", 0, FS_LAST_ERROR) < 0) {
        JS_FreeValue(context, fs);
        return -1;
    }
    return JS_SetPropertyStr(context, global_value, "fs", fs);
}

static int add_operation(JSValueConst object, const char *name, int arity,
                         HostOperation operation)
{
    JSValue function = JS_NewCFunctionMagic(
        context, host_operation, name, arity, JS_CFUNC_generic_magic, (int)operation);
    if (JS_IsException(function)) return -1;
    return JS_SetPropertyStr(context, object, name, function);
}

static int install_host(void)
{
    JSValue ui = JS_NewObject(context);
    if (JS_IsException(ui)) return -1;
    if (add_operation(ui, "createNode", 1, HOST_CREATE_NODE) < 0 ||
        add_operation(ui, "destroyNode", 1, HOST_DESTROY_NODE) < 0 ||
        add_operation(ui, "insertBefore", 3, HOST_INSERT_BEFORE) < 0 ||
        add_operation(ui, "removeChild", 2, HOST_REMOVE_CHILD) < 0 ||
        add_operation(ui, "setStyle", 2, HOST_SET_STYLE) < 0 ||
        add_operation(ui, "setProp", 3, HOST_SET_PROP) < 0 ||
        add_operation(ui, "setPropBatch", 1, HOST_SET_PROP_BATCH) < 0 ||
        add_operation(ui, "setText", 2, HOST_SET_TEXT) < 0 ||
        add_operation(ui, "replaceText", 2, HOST_REPLACE_TEXT) < 0 ||
        add_operation(ui, "uploadTexture", 4, HOST_UPLOAD_TEXTURE) < 0 ||
        add_operation(ui, "setImage", 2, HOST_SET_IMAGE) < 0 ||
        add_operation(ui, "setSprite", 5, HOST_SET_SPRITE) < 0 ||
        add_operation(ui, "animate", 6, HOST_ANIMATE) < 0 ||
        add_operation(ui, "cancelAnim", 1, HOST_CANCEL_ANIM) < 0 ||
        add_operation(ui, "setFocus", 1, HOST_SET_FOCUS) < 0 ||
        add_operation(ui, "setActive", 2, HOST_SET_ACTIVE) < 0 ||
        add_operation(ui, "loadStyles", 1, HOST_LOAD_STYLES) < 0 ||
        add_operation(ui, "loadFontAtlas", 1, HOST_LOAD_FONT_ATLAS) < 0 ||
        add_operation(ui, "measureText", 2, HOST_MEASURE_TEXT) < 0 ||
        add_operation(ui, "freeTexture", 1, HOST_FREE_TEXTURE) < 0 ||
        add_operation(ui, "uploadImgEntry", 1, HOST_UPLOAD_IMG_ENTRY) < 0 ||
        JS_SetPropertyStr(context, ui, "__host", JS_NewString(context, "ipod-photo")) < 0 ||
        JS_SetPropertyStr(context, ui, "__hostAbi", JS_NewInt32(context, PJS_QJS_HOST_ABI)) < 0 ||
        JS_SetPropertyStr(context, ui, "__root", JS_NewInt32(context, PJS_QJS_ROOT_ID)) < 0 ||
        JS_SetPropertyStr(context, ui, "__tickHz", JS_NewInt32(context, 60)) < 0) {
        JS_FreeValue(context, ui);
        return -1;
    }

    JSValue viewport = JS_NewObject(context);
    if (JS_IsException(viewport)) {
        JS_FreeValue(context, ui);
        return -1;
    }
    if (JS_SetPropertyStr(context, viewport, "w", JS_NewInt32(context, 220)) < 0 ||
        JS_SetPropertyStr(context, viewport, "h", JS_NewInt32(context, 176)) < 0) {
        JS_FreeValue(context, viewport);
        JS_FreeValue(context, ui);
        return -1;
    }
    /* JS_SetPropertyStr consumes viewport whether it succeeds or fails. */
    if (JS_SetPropertyStr(context, ui, "__viewport", viewport) < 0) {
        JS_FreeValue(context, ui);
        return -1;
    }

    ipod_input_value = JS_NewObject(context);
    if (JS_IsException(ipod_input_value) ||
        JS_SetPropertyStr(context, ui, "__ipodInput",
                          JS_DupValue(context, ipod_input_value)) < 0) {
        JS_FreeValue(context, ui);
        return -1;
    }

    launcher_value = JS_UNDEFINED;
    if (launcher_count != 0u) {
        JSValue apps = JS_NewArray(context);
        launcher_value = JS_NewObject(context);
        if (JS_IsException(apps) || JS_IsException(launcher_value)) {
            JS_FreeValue(context, apps);
            JS_FreeValue(context, ui);
            return -1;
        }
        for (uint32_t index = 0u; index < launcher_count; ++index) {
            JSValue label = JS_NewString(
                context, (const char *)launcher_labels[index]);
            if (JS_IsException(label) ||
                JS_SetPropertyUint32(context, apps, index, label) < 0) {
                JS_FreeValue(context, apps);
                JS_FreeValue(context, ui);
                return -1;
            }
        }
        if (JS_SetPropertyStr(context, launcher_value, "selected",
                              JS_NewInt32(context, -1)) < 0 ||
            JS_SetPropertyStr(context, ui, "__ipodApps", apps) < 0 ||
            JS_SetPropertyStr(context, ui, "__ipodLauncher",
                              JS_DupValue(context, launcher_value)) < 0) {
            JS_FreeValue(context, ui);
            return -1;
        }
    }

    /* JS_SetPropertyStr consumes ui whether it succeeds or fails. */
    if (JS_SetPropertyStr(context, global_value, "ui", ui) < 0 ||
        JS_SetPropertyStr(context, global_value, "__simHz", JS_NewInt32(context, 60)) < 0) {
        return -1;
    }
    if (install_audio() < 0) return -1;
    if (install_fs() < 0) return -1;
    return 0;
}

/* Feed the cheap, non-image part of a PAK before QuickJS evaluates the
 * generated bundle.  Image/sprite entries deliberately disable this fast
 * path until the native name-table ABI is complete; JS then owns the whole
 * pack consistently. */
static int prefeed_pak_styles_fonts(const uint8_t *pak, uint32_t length)
{
    const uint32_t header = 32u;
    const uint32_t entry_size = 24u;
    if (pak == 0 || length < header ||
        pak[0] != 'D' || pak[1] != 'C' || pak[2] != 'P' || pak[3] != 'K' ||
        pak[4] != 1u || pak[5] != 0u) return 0;
    uint32_t count = (uint32_t)pak[8] | ((uint32_t)pak[9] << 8) |
        ((uint32_t)pak[10] << 16) | ((uint32_t)pak[11] << 24);
    uint32_t dir = (uint32_t)pak[12] | ((uint32_t)pak[13] << 8) |
        ((uint32_t)pak[14] << 16) | ((uint32_t)pak[15] << 24);
    uint32_t names = (uint32_t)pak[16] | ((uint32_t)pak[17] << 8) |
        ((uint32_t)pak[18] << 16) | ((uint32_t)pak[19] << 24);
    if (dir < header || dir > length || names > length ||
        count > (length - dir) / entry_size) {
        return -1;
    }
    const uint8_t *styles = 0;
    uint32_t styles_len = 0u;
    for (uint32_t index = 0u; index < count; ++index) {
        uint32_t e = dir + index * entry_size;
        uint32_t blob_off = (uint32_t)pak[e + 4u] |
            ((uint32_t)pak[e + 5u] << 8) | ((uint32_t)pak[e + 6u] << 16) |
            ((uint32_t)pak[e + 7u] << 24);
        uint32_t blob_len = (uint32_t)pak[e + 8u] |
            ((uint32_t)pak[e + 9u] << 8) | ((uint32_t)pak[e + 10u] << 16) |
            ((uint32_t)pak[e + 11u] << 24);
        uint32_t name_off = (uint32_t)pak[e + 12u] |
            ((uint32_t)pak[e + 13u] << 8) | ((uint32_t)pak[e + 14u] << 16) |
            ((uint32_t)pak[e + 15u] << 24);
        uint32_t name_len = (uint32_t)pak[e + 16u] |
            ((uint32_t)pak[e + 17u] << 8);
        if (blob_off > length || blob_len > length - blob_off ||
            name_off > length - names || name_len > length - names - name_off) {
            return -1;
        }
        const uint8_t *name = pak + names + name_off;
        bool is_styles = name_len == 9u && memcmp(name, "ui:styles", 9u) == 0;
        bool is_font = name_len >= 8u && memcmp(name, "ui:font.", 8u) == 0;
        bool is_image = name_len >= 7u && memcmp(name, "ui:img.", 7u) == 0;
        bool is_sprite = name_len >= 10u && memcmp(name, "ui:sprite.", 10u) == 0;
        if (is_image || is_sprite) return 0;
        if (is_styles) {
            if (styles != 0) return -1;
            styles = pak + blob_off;
            styles_len = blob_len;
        } else if (is_font) {
            /* Font keys are ASCII compiler output; reject control bytes so a
             * malformed key cannot alias a later JS-side lookup. */
            for (uint32_t c = 8u; c < name_len; ++c) {
                if (name[c] < 0x21u || name[c] > 0x7eu) return -1;
            }
        }
    }
    if (styles == 0) return 0;
    if (pjs_ui_load_styles(styles, styles_len) == 0) return -1;
    for (uint32_t index = 0u; index < count; ++index) {
        uint32_t e = dir + index * entry_size;
        uint32_t blob_off = (uint32_t)pak[e + 4u] |
            ((uint32_t)pak[e + 5u] << 8) | ((uint32_t)pak[e + 6u] << 16) |
            ((uint32_t)pak[e + 7u] << 24);
        uint32_t name_off = (uint32_t)pak[e + 12u] |
            ((uint32_t)pak[e + 13u] << 8) | ((uint32_t)pak[e + 14u] << 16) |
            ((uint32_t)pak[e + 15u] << 24);
        uint32_t name_len = (uint32_t)pak[e + 16u] |
            ((uint32_t)pak[e + 17u] << 8);
        const uint8_t *name = pak + names + name_off;
        if (name_len >= 8u && memcmp(name, "ui:font.", 8u) == 0) {
            uint32_t blob_len = (uint32_t)pak[e + 8u] |
                ((uint32_t)pak[e + 9u] << 8) | ((uint32_t)pak[e + 10u] << 16) |
                ((uint32_t)pak[e + 11u] << 24);
            if (pjs_ui_load_font_atlas(pak + blob_off, blob_len) == 0) return -1;
        }
    }
    JSValue ui = JS_GetPropertyStr(context, global_value, "ui");
    if (JS_IsException(ui)) return -1;
    JSValue textures = JS_NewObject(context);
    if (JS_IsException(textures)) {
        JS_FreeValue(context, ui);
        return -1;
    }
    /* SetProperty consumes its value on both success and failure. The ui
     * object is already held by global_value; release only our local ref. */
    if (JS_SetPropertyStr(context, ui, "__textures", textures) < 0) {
        JS_FreeValue(context, ui);
        return -1;
    }
    JSValue sprites = JS_NewObject(context);
    if (JS_IsException(sprites)) {
        JS_FreeValue(context, ui);
        return -1;
    }
    int published = JS_SetPropertyStr(context, ui, "__sprites", sprites);
    JS_FreeValue(context, ui);
    if (published < 0) return -1;
    return 1;
}

static void discard_exception(void)
{
    if (context == 0) return;
    JSValue exception = JS_GetException(context);
    JS_FreeValue(context, exception);
}

static void capture_exception(void)
{
    error_text_length = 0u;
    error_text[0] = 0;
    if (context == 0) return;
    JSValue exception = JS_GetException(context);
    size_t length = 0u;
    const char *text = JS_ToCStringLen2(context, &length, exception, 0);
    if (text != 0) {
        if (length >= sizeof(error_text)) length = sizeof(error_text) - 1u;
        memcpy(error_text, text, length);
        error_text[length] = 0;
        error_text_length = (uint32_t)length;
        JS_FreeCString(context, text);
    } else if (JS_HasException(context)) {
        JSValue conversion_error = JS_GetException(context);
        JS_FreeValue(context, conversion_error);
    }
    JS_FreeValue(context, exception);
}

static bool drain_jobs(uint32_t error)
{
    for (uint32_t count = 0u; count < PJS_QJS_MAX_PENDING_JOBS; ++count) {
        JSContext *pending = 0;
        int result = JS_ExecutePendingJob(runtime, &pending);
        if (result > 0) continue;
        if (result < 0) {
            error_code = error;
            capture_exception();
            return false;
        }
        return true;
    }
    /* A guest can legitimately leave more work queued than fits in one
     * cooperative turn.  Keep the remaining jobs in QuickJS's queue and
     * service them after the next frame; exceptions and interrupt-budget
     * failures above still fail the turn. */
    (void)error;
    return true;
}

bool qjs_runtime_boot(const PjsGuestPackage *guest)
{
    /* Boot is also the app replacement boundary.  Callers normally shut down
     * explicitly, but keeping this path self-contained prevents a failed or
     * interrupted switch from leaking the previous runtime and its retained
     * host objects. */
    if (context != 0 || runtime != 0) qjs_runtime_shutdown();
    boot_profile = (PjsQjsBootProfile){0};
#if defined(__arm__)
    /* Capture the inherited clock configuration; do not infer CPU speed
     * from the SoC's rated maximum or change clock ownership to profile it. */
    boot_profile.clock_source = PP_CLOCK_SOURCE;
    boot_profile.pll_control = MMIO32(0x60006034u);
    boot_profile.memory_timing = MMIO32(0x70000034u);
    boot_profile.device_init = PP_DEV_INIT2;
#endif
    boot_profile_active = true;
    pending_wheel_units = 0;
    wheel_release_pending = false;
    fs_app_id = 0;
    fs_app_id_length = 0u;
    fs_open_attempted = false;
    fs_open_result = 0;
    pjs_fs_store_close();
    /* Guest ownership is strict: no stream may survive an app replacement,
     * even if the previous runtime exited before its ordinary shutdown path. */
    (void)pjs_audio_pcm_reset();
    error_code = PJS_QJS_ERROR_NONE;
    error_text_length = 0u;
    error_text[0] = 0;
    runtime = 0;
    context = 0;
    global_value = JS_UNDEFINED;
    frame_function = JS_UNDEFINED;
    ipod_input_value = JS_UNDEFINED;
    launcher_value = JS_UNDEFINED;
    legacy_frame_abi = false;
    if (guest == 0 || guest->javascript == 0 || guest->javascript_length < 2u ||
        guest->javascript[guest->javascript_length - 1u] != 0u) {
        error_code = PJS_QJS_ERROR_EVAL;
        boot_profile_active = false;
        return false;
    }

    uint32_t phase_started = timer_now_us();
    runtime = JS_NewRuntime2(&qjs_allocators, 0);
    boot_profile.runtime_us = timer_now_us() - phase_started;
    if (runtime == 0) {
        error_code = PJS_QJS_ERROR_RUNTIME;
        boot_profile_active = false;
        return false;
    }
    JS_SetMemoryLimit(runtime, PJS_QJS_MEMORY_LIMIT);
    JS_SetGCThreshold(runtime, PJS_QJS_GC_THRESHOLD);
    JS_SetMaxStackSize(runtime, PJS_QJS_STACK_LIMIT);
    JS_SetCanBlock(runtime, 0);
    JS_SetInterruptHandler(runtime, interrupt_handler, 0);

    phase_started = timer_now_us();
    context = JS_NewContext(runtime);
    boot_profile.context_us = timer_now_us() - phase_started;
    if (context == 0) {
        error_code = PJS_QJS_ERROR_CONTEXT;
        qjs_runtime_shutdown();
        return false;
    }
    global_value = JS_GetGlobalObject(context);
    if (JS_IsException(global_value)) {
        error_code = PJS_QJS_ERROR_HOST;
        discard_exception();
        qjs_runtime_shutdown();
        return false;
    }
    /* The package id, unlike the package hash, remains stable across package
     * updates and is the filesystem's per-app identity. */
    if (guest->app_id == 0 || guest->app_id_length == 0u ||
        memchr(guest->app_id, '\0', guest->app_id_length) != 0) {
        error_code = PJS_QJS_ERROR_HOST;
        qjs_runtime_shutdown();
        return false;
    }
    fs_app_id = guest->app_id;
    fs_app_id_length = guest->app_id_length;
    phase_started = timer_now_us();
    if (install_host() < 0) {
        error_code = PJS_QJS_ERROR_HOST;
        discard_exception();
        qjs_runtime_shutdown();
        return false;
    }
    boot_profile.host_us = timer_now_us() - phase_started;

    if (guest->pak != 0 && guest->pak_length != 0u) {
        phase_started = timer_now_us();
        int prefeed = prefeed_pak_styles_fonts(guest->pak, guest->pak_length);
        boot_profile.native_pak_us = timer_now_us() - phase_started;
        if (prefeed < 0) {
            error_code = PJS_QJS_ERROR_HOST;
            qjs_runtime_shutdown();
            return false;
        }
    }

    if (guest->pak != 0 && guest->pak_length != 0u) {
        JSValue pak = JS_NewArrayBuffer(
            context, (uint8_t *)guest->pak, guest->pak_length, 0, 0, 0);
        if (JS_IsException(pak) ||
            JS_SetPropertyStr(context, global_value, "__pak", pak) < 0) {
            error_code = PJS_QJS_ERROR_HOST;
            discard_exception();
            qjs_runtime_shutdown();
            return false;
        }
    }

    phase_started = timer_now_us();
    execution_budget_start(PJS_QJS_BOOT_BUDGET_US);
    JSValue result = JS_Eval(
        context,
        (const char *)guest->javascript,
        guest->javascript_length - 1u,
        "embedded-recovery.js",
        JS_EVAL_TYPE_GLOBAL
    );
    execution_budget_stop();
    boot_profile.eval_us = timer_now_us() - phase_started;
    if (JS_IsException(result)) {
        error_code = execution_timed_out ?
            PJS_QJS_ERROR_BOOT_BUDGET : PJS_QJS_ERROR_EVAL;
        capture_exception();
        qjs_runtime_shutdown();
        return false;
    }
    JS_FreeValue(context, result);

    frame_function = JS_GetPropertyStr(context, global_value, "frame");
    if (!JS_IsFunction(context, frame_function)) {
        error_code = PJS_QJS_ERROR_FRAME_EXPORT;
        qjs_runtime_shutdown();
        return false;
    }
    /* The historical A1099 recovery package used seven device-local words.
     * Function.length is stable for both that package and the generated
     * framework wrapper (whose standard input surface is at most five words),
     * so this compatibility decision needs no app-specific marker. */
    JSValue frame_length = JS_GetPropertyStr(context, frame_function, "length");
    if (!JS_IsException(frame_length)) {
        int32_t arity = 0;
        if (JS_ToInt32(context, &arity, frame_length) == 0) {
            legacy_frame_abi = arity >= 7;
        } else {
            discard_exception();
        }
    } else {
        discard_exception();
    }
    JS_FreeValue(context, frame_length);
    phase_started = timer_now_us();
    execution_budget_start(PJS_QJS_BOOT_BUDGET_US);
    bool jobs_ok = drain_jobs(PJS_QJS_ERROR_PENDING_JOB);
    execution_budget_stop();
    boot_profile.jobs_us = timer_now_us() - phase_started;
    if (!jobs_ok) {
        if (execution_timed_out) error_code = PJS_QJS_ERROR_BOOT_BUDGET;
        qjs_runtime_shutdown();
        return false;
    }
    boot_profile_active = false;
    return true;
}

bool qjs_runtime_frame(const PjsCoreInput *input)
{
    if (context == 0 || runtime == 0 || input == 0) return false;
    /* Publish hardware facts before the guest turn. poll() can drain this
     * frozen batch, while new native facts wait for the following tick. */
    pjs_audio_pcm_begin_tick();
    uint32_t buttons = guest_buttons(input);
    JSValue arguments[7];
    size_t argument_count;

    if (legacy_frame_abi) {
        /* Compatibility for the already-qualified disk package. */
        arguments[0] = JS_NewUint32(context, input->buttons);
        arguments[1] = JS_NewInt32(context, input->wheel_delta);
        arguments[2] = JS_NewUint32(context, input->wheel_position);
        arguments[3] = JS_NewBool(context, input->wheel_touched != 0u);
        arguments[4] = JS_NewBool(context, input->hold != 0u);
        arguments[5] = JS_NewUint32(context, input->battery_mv);
        arguments[6] = JS_NewUint32(context, input->power_flags);
        argument_count = 7u;
    } else {
        /* Standard generated-app ABI.  A1099 has no analog nub or touch
         * surface, so the centered analog word is the complete input frame;
         * optional touch arguments are intentionally omitted. */
        arguments[0] = JS_NewUint32(context, buttons);
        arguments[1] = JS_NewUint32(context, PJS_FRAME_ANALOG_CENTER);
        argument_count = 2u;
    }

    execution_budget_start(PJS_QJS_FRAME_BUDGET_US);
    if (update_ipod_input(input, buttons) < 0) {
        execution_budget_stop();
        error_code = PJS_QJS_ERROR_HOST;
        discard_exception();
        for (size_t index = 0u; index < argument_count; ++index) {
            JS_FreeValue(context, arguments[index]);
        }
        return false;
    }
    JSValue result = JS_Call(context, frame_function, global_value,
                             (int)argument_count, arguments);
    for (size_t index = 0u; index < argument_count; ++index) {
        JS_FreeValue(context, arguments[index]);
    }
    if (JS_IsException(result)) {
        execution_budget_stop();
        error_code = execution_timed_out ?
            PJS_QJS_ERROR_FRAME_BUDGET : PJS_QJS_ERROR_FRAME_CALL;
        capture_exception();
        return false;
    }
    JS_FreeValue(context, result);
    bool jobs_ok = drain_jobs(PJS_QJS_ERROR_PENDING_JOB);
    execution_budget_stop();
    return jobs_ok;
}

uint32_t qjs_runtime_error_code(void)
{
    return error_code;
}

const char *qjs_runtime_error_text(uint32_t *length)
{
    if (length != 0) *length = error_text_length;
    return error_text;
}

int32_t qjs_runtime_launcher_selection(void)
{
    if (context == 0 || launcher_count == 0u ||
        JS_IsUndefined(launcher_value)) return -1;
    JSValue selected = JS_GetPropertyStr(context, launcher_value, "selected");
    if (JS_IsException(selected)) {
        discard_exception();
        return -1;
    }
    int32_t index = -1;
    if (JS_ToInt32(context, &index, selected) < 0) {
        discard_exception();
        index = -1;
    }
    JS_FreeValue(context, selected);
    return index >= 0 && (uint32_t)index < launcher_count ? index : -1;
}

void qjs_runtime_shutdown(void)
{
    boot_profile_active = false;
    pending_wheel_units = 0;
    wheel_release_pending = false;
    execution_budget_stop();
    fs_app_id = 0;
    fs_app_id_length = 0u;
    fs_open_attempted = false;
    pjs_fs_store_close();
    (void)pjs_audio_pcm_reset();
    if (context != 0) {
        JS_FreeValue(context, launcher_value);
        JS_FreeValue(context, ipod_input_value);
        JS_FreeValue(context, frame_function);
        JS_FreeValue(context, global_value);
        JS_FreeContext(context);
        context = 0;
    }
    if (runtime != 0) {
        JS_FreeRuntime(runtime);
        runtime = 0;
    }
    global_value = JS_UNDEFINED;
    frame_function = JS_UNDEFINED;
    ipod_input_value = JS_UNDEFINED;
    launcher_value = JS_UNDEFINED;
    legacy_frame_abi = false;
}
