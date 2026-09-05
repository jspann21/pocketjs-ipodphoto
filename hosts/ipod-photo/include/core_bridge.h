#ifndef POCKETJS_IPOD_PHOTO_CORE_BRIDGE_H
#define POCKETJS_IPOD_PHOTO_CORE_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "power.h"

#define PJS_CORE_MAX_DAMAGE_REGIONS 8u

typedef struct {
    uint32_t buttons;
    int32_t wheel_delta;
    uint32_t wheel_position;
    uint32_t wheel_touched;
    uint32_t hold;
    uint32_t battery_mv;
    uint32_t power_flags;
    uint32_t dropped_ticks;
    uint32_t cache_enabled;
    uint32_t last_frame_us;
    uint32_t runtime_status;
} PjsCoreInput;

typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} PjsCoreDamageRect;

typedef struct {
    uint32_t count;
    uint32_t full_redraw;
    uint32_t area;
    uint32_t reserved;
    PjsCoreDamageRect regions[PJS_CORE_MAX_DAMAGE_REGIONS];
} PjsCoreDamagePlan;

_Static_assert(sizeof(PjsCoreInput) == 44u, "core input ABI changed");
_Static_assert(sizeof(PjsCoreDamageRect) == 16u, "damage rectangle ABI changed");
_Static_assert(sizeof(PjsCoreDamagePlan) == 144u, "damage plan ABI changed");


#define PJS_RUNTIME_DISABLED 0u
#define PJS_RUNTIME_PACKAGE_ADMITTED 1u
#define PJS_RUNTIME_READY 2u
#define PJS_RUNTIME_ERROR 3u
#define PJS_RUNTIME_READY_DISK 4u
#define PJS_RUNTIME_ERROR_BUDGET 5u

#define PJS_BOOT_SOURCE_NONE 0u
#define PJS_BOOT_SOURCE_PENDING 1u
#define PJS_BOOT_SOURCE_ACTIVE 2u
#define PJS_BOOT_SOURCE_LAST_GOOD 3u
#define PJS_BOOT_SOURCE_LEGACY_APP 4u
#define PJS_BOOT_SOURCE_EMBEDDED 5u
#define PJS_BOOT_SOURCE_CATALOG 6u
#define PJS_BOOT_SOURCE_PACKAGE_BANK 7u

#define PJS_BOOT_FAILURE_NONE 0u
#define PJS_BOOT_FAILURE_STORAGE 1u
#define PJS_BOOT_FAILURE_PACKAGE 2u
#define PJS_BOOT_FAILURE_QUICKJS 3u
#define PJS_BOOT_FAILURE_FRAME 4u
#define PJS_BOOT_FAILURE_EMBEDDED_PACKAGE 5u
#define PJS_BOOT_FAILURE_EMBEDDED_QUICKJS 6u
#define PJS_BOOT_FAILURE_MEMORY 7u

typedef struct {
    const uint8_t *javascript;
    uint32_t javascript_length;
    const uint8_t *pak;
    uint32_t pak_length;
    const uint8_t *plan;
    uint32_t plan_length;
    uint32_t package_hash_low;
    uint32_t package_hash_high;
    uint32_t variant_hash_low;
    uint32_t variant_hash_high;
    const uint8_t *app_id;
    uint32_t app_id_length;
} PjsGuestPackage;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(PjsGuestPackage) == 48u, "guest package ABI changed");
#endif

#define PJS_CORE_BACKEND_RUST_MAGIC 0x52555354u
#define PJS_CORE_BACKEND_STUB_MAGIC 0x53545542u

uint32_t pjs_core_backend_marker(void);
int32_t pjs_core_init(void);
int32_t pjs_core_step(const PjsCoreInput *input);
int32_t pjs_core_render(uint16_t *pixels, uint32_t pixel_count);
int32_t pjs_core_render_damage(uint16_t *pixels, uint32_t pixel_count,
                               PjsCoreDamagePlan *damage);
uint32_t pjs_core_needs_render(void);
uint32_t pjs_core_frame(void);
uint32_t pjs_core_draw_words(void);
void pjs_core_shutdown(void);
void pjs_core_set_boot_diagnostic(uint32_t source, uint32_t failure_stage,
                                  uint32_t failure_code,
                                  uint32_t sector_reads);
void pjs_core_set_app_diagnostic(uint32_t selected, uint32_t count,
                                 uint32_t sector_reads);
void pjs_core_set_persistence_diagnostic(uint32_t mode, uint32_t slot,
                                         uint32_t generation,
                                         uint32_t error);
void pjs_core_set_lineage_diagnostic(uint32_t mode, uint32_t source,
                                     uint32_t generation, uint32_t error);
void pjs_core_set_kernel_diagnostic(uint32_t mode, uint32_t error);

int32_t pjs_package_open_ipod_photo(const uint8_t *bytes, uint32_t length,
                                    PjsGuestPackage *guest);
int32_t pjs_ui_create_node(uint32_t node_type);
void pjs_ui_destroy_node(int32_t id);
void pjs_ui_insert_before(int32_t parent, int32_t child, int32_t anchor);
void pjs_ui_remove_child(int32_t parent, int32_t child);
void pjs_ui_set_style(int32_t id, int32_t style_id);
void pjs_ui_set_prop(int32_t id, uint32_t prop, double value);
void pjs_ui_set_prop_batch(const uint8_t *bytes, size_t length);
void pjs_ui_set_text(int32_t id, const uint8_t *text, size_t length);
void pjs_ui_replace_text(int32_t id, const uint8_t *text, size_t length);
int32_t pjs_ui_upload_texture(const uint8_t *bytes, size_t length,
                              uint32_t width, uint32_t height,
                              uint32_t pixel_storage);
int32_t pjs_ui_upload_img_entry(const uint8_t *bytes, size_t length);
void pjs_ui_free_texture(int32_t handle);
void pjs_ui_set_image(int32_t id, int32_t texture);
void pjs_ui_set_sprite(int32_t id, int32_t atlas, uint32_t frames,
                       uint32_t columns, uint32_t step);
int32_t pjs_ui_animate(int32_t id, uint32_t prop, double to,
                       uint32_t duration_ms, uint32_t easing,
                       uint32_t delay_ms);
void pjs_ui_cancel_anim(int32_t animation_id);
void pjs_ui_set_focus(int32_t id);
void pjs_ui_set_active(int32_t id, int32_t active);
int32_t pjs_ui_load_styles(const uint8_t *bytes, size_t length);
int32_t pjs_ui_load_font_atlas(const uint8_t *bytes, size_t length);
float pjs_ui_measure_text(const uint8_t *text, size_t length,
                          uint32_t font_slot);

#endif
