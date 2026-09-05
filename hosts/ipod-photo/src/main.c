#include "backlight.h"
#include "audio.h"
#include "audio_stream_gate.h"
#include "audio_pcm.h"
#include "audio_clock.h"
#include "audio_dma.h"
#include "cache.h"
#include "core_bridge.h"
#include "cpu_idle.h"
#include "heap.h"
#include "input.h"
#include "irq.h"
#include "lcd.h"
#include "panic.h"
#include "platform.h"
#include "power.h"
#include "qjs_runtime.h"
#include "pp5020.h"
#include "scheduler.h"
#include "storage.h"
#include "timer.h"
#include "timer_irq.h"
#include "native_loader.h"
#include "usb_device.h"
#include "usb_protocol.h"










static PjsUsbDiagnostics batch_diagnostics;
#define RECORD_KERNEL(mode, code) do { \
    batch_diagnostics.kernel_mode = (mode); \
    if ((code) != 0u && batch_diagnostics.first_kernel_error == 0u) \
        batch_diagnostics.first_kernel_error = (code); \
} while (0)
#define RECORD_LINEAGE(source, generation, error) do { \
    batch_diagnostics.lineage_source = (source); \
    batch_diagnostics.lineage_generation = (generation); \
    if ((error) != 0u) batch_diagnostics.lineage_error = (error); \
} while (0)
/* Production keeps fault diagnostics visible while avoiding routine probe
 * status overlays on the application UI. */
#define pjs_core_set_kernel_diagnostic(mode, code) do { \
    RECORD_KERNEL((mode), (code)); \
    if ((code) != 0u) pjs_core_set_kernel_diagnostic((mode), (code)); \
} while (0)
#define pjs_core_set_persistence_diagnostic(event, slot, generation, error) do { \
    if ((error) != 0u) pjs_core_set_persistence_diagnostic( \
        (event), (slot), (generation), (error)); \
} while (0)
#define pjs_core_set_boot_diagnostic(source, stage, code, reads) do { \
    if ((stage) != 0u || (code) != 0u) pjs_core_set_boot_diagnostic( \
        (source), (stage), (code), (reads)); \
} while (0)
#define pjs_core_set_lineage_diagnostic(mode, source, generation, error) do { \
    RECORD_LINEAGE((source), (generation), (error)); \
    if ((mode) == 3u || (mode) == 4u || (mode) >= 6u || (error) != 0u) \
        pjs_core_set_lineage_diagnostic((mode), (source), (generation), (error)); \
} while (0)








#define PJS_PLACEHOLDER_BATTERY_MV 3800u
#define PJS_WHEEL_DELTA_LIMIT 127
#define PJS_LAUNCHER_PACKAGE_HASH_LOW pjs_launcher_hash_low
extern const uint32_t pjs_launcher_hash_low;
#define PJS_LAUNCHER_PACKAGE_HASH_HIGH pjs_launcher_hash_high
extern const uint32_t pjs_launcher_hash_high;
#define PJS_MEMORY_GUARD_WORDS 64u
#define PJS_STACK_GUARD_PATTERN 0x53544b47u
#define PJS_HEAP_GUARD_PATTERN 0x48454147u
#define PJS_RUNTIME_MIN_LARGEST_FREE (8u * 1024u * 1024u)
/* Observer requests cannot upload packages; satisfy the protocol's buffer
 * invariant without reserving the development service's 4 MiB arena. */
#define PJS_USB_STAGING_BYTES 32u
#define PJS_USB_WIRE_PAYLOAD_BYTES PJS_USB_PROTOCOL_MAX_PAYLOAD

extern unsigned char __heap_start;
extern unsigned char __heap_end;
extern unsigned char __ram_end;
extern unsigned char __stack_bottom;
extern const uint8_t pjs_embedded_package[];
extern const uint32_t pjs_embedded_package_length;

static uint16_t framebuffer[PJS_FRAME_PIXELS] __attribute__((aligned(16)));
static bool status_hold, presented_hold;
static uint32_t last_presented_damage_area;

static void poll_system_input(PjsInputState *input)
{
    input_poll(input);
    status_hold = input->hold;
}

static PjsUsbPerformance observed_performance;
static void idle_until_service(void)
{
    if (pjs_usb_cdc_configured() &&
        (pjs_usb_cdc_rx_available() != 0u || !pjs_usb_cdc_tx_idle())) return;
    pjs_audio_stream_gate_refill();
    cpu_idle_wait();
}
static volatile uint32_t *stack_guard;
static volatile uint32_t *heap_guard;

static uint8_t usb_protocol_rx[PJS_USB_PROTOCOL_HEADER_BYTES +
                               PJS_USB_WIRE_PAYLOAD_BYTES]
    __attribute__((aligned(16)));
static uint8_t usb_protocol_tx[PJS_USB_PROTOCOL_HEADER_BYTES +
                               PJS_USB_WIRE_PAYLOAD_BYTES]
    __attribute__((aligned(16)));

static int32_t usb_protocol_read(void *context, uint8_t *bytes,
                                 uint32_t capacity)
{
    (void)context;
    if (!pjs_usb_cdc_configured()) return 0;
    return pjs_usb_cdc_read(bytes, capacity);
}

static int32_t usb_protocol_write(void *context, const uint8_t *bytes,
                                  uint32_t length)
{
    (void)context;
    if (!pjs_usb_cdc_configured()) return 0;
    return pjs_usb_cdc_write(bytes, length);
}


typedef struct {
    const char *file_name;
    uint32_t source;
} PjsBootSlot;

static const char pending_package[11] =
    {'P','E','N','D','I','N','G',' ','P','K','T'};
static const char active_package[11] =
    {'A','C','T','I','V','E',' ',' ','P','K','T'};
static const char last_good_package[11] =
    {'L','A','S','T','G','O','O','D','P','K','T'};
static const char legacy_package[11] =
    {'A','P','P',' ',' ',' ',' ',' ','P','K','T'};
static const char launcher_package[11] =
    {'L','A','U','N','C','H','E','R','P','K','T'};

static const PjsBootSlot boot_slots[] = {
    {pending_package, PJS_BOOT_SOURCE_PENDING},
    {active_package, PJS_BOOT_SOURCE_ACTIVE},
    {last_good_package, PJS_BOOT_SOURCE_LAST_GOOD},
    {legacy_package, PJS_BOOT_SOURCE_LEGACY_APP},
};

static void disable_interrupt_sources(void)
{
    PP_CPU_INT_DIS = 0xffffffffu;
    PP_COP_INT_DIS = 0xffffffffu;
    PP_INT_FORCED_CLR = 0xffffffffu;
    PP_CPU_HI_INT_DIS = 0xffffffffu;
    PP_COP_HI_INT_DIS = 0xffffffffu;
    PP_HI_INT_FORCED_CLR = 0xffffffffu;
}

static void initialize_heap(void)
{
    uintptr_t start = (uintptr_t)&__heap_start;
    uintptr_t end = (uintptr_t)&__heap_end;
    if (end <= start + PJS_HEAP_TOP_GUARD) panic_code(0x48454131u); /* HEA1 */
    end -= PJS_HEAP_TOP_GUARD;
    stack_guard = (volatile uint32_t *)(uintptr_t)&__stack_bottom;
    heap_guard = (volatile uint32_t *)end;
    for (uint32_t index = 0u; index < PJS_MEMORY_GUARD_WORDS; ++index) {
        stack_guard[index] = PJS_STACK_GUARD_PATTERN;
        heap_guard[index] = PJS_HEAP_GUARD_PATTERN;
    }
    if (!pjs_heap_init((void *)start, end - start) || !pjs_heap_validate()) {
        panic_code(0x48454132u); /* HEA2 */
    }
}

static bool memory_integrity_ok(void)
{
    if (!pjs_heap_validate()) return false;
    for (uint32_t index = 0u; index < PJS_MEMORY_GUARD_WORDS; ++index) {
        if (stack_guard[index] != PJS_STACK_GUARD_PATTERN ||
            heap_guard[index] != PJS_HEAP_GUARD_PATTERN) return false;
    }
    return true;
}

static bool runtime_memory_admitted(void)
{
    if (!memory_integrity_ok()) return false;
    PjsHeapStats stats = {0};
    pjs_heap_stats(&stats);
    return stats.largest_free >= PJS_RUNTIME_MIN_LARGEST_FREE;
}

static int32_t clamp_wheel_delta(int32_t value)
{
    if (value < -PJS_WHEEL_DELTA_LIMIT) return -PJS_WHEEL_DELTA_LIMIT;
    if (value > PJS_WHEEL_DELTA_LIMIT) return PJS_WHEEL_DELTA_LIMIT;
    return value;
}

/* Retained across warm app sessions, reset only by a hardware boot. These
 * counters let one host log distinguish app switches from actual restarts. */
static uint32_t diagnostics_sample_us;
static void publish_usb_power(PjsUsbProtocol *protocol,
                              const PjsPowerTelemetry *power)
{
    uint32_t now = timer_now_us();
    if (!protocol->diagnostics_valid ||
        (uint32_t)(now - diagnostics_sample_us) >= 1000000u) {
        PjsHeapStats heap = {0};
        pjs_heap_stats(&heap);
        batch_diagnostics.heap_free = (uint32_t)heap.free_bytes;
        batch_diagnostics.heap_largest_free = (uint32_t)heap.largest_free;
        batch_diagnostics.heap_allocated = (uint32_t)heap.allocated_bytes;
        batch_diagnostics.audio_error = pjs_audio_pcm_last_error();
        batch_diagnostics.audio_busy = pjs_audio_pcm_busy();
        PjsAudioDmaSnapshot dma = {0};
        pjs_audio_dma_snapshot(&dma);
        batch_diagnostics.audio_underruns = dma.underruns;
        if (dma.fault != 0u) batch_diagnostics.audio_dma_fault = dma.fault;
        diagnostics_sample_us = now;
    }
    protocol->diagnostics = batch_diagnostics;
    protocol->diagnostics_valid = true;
    const PjsUsbPowerTelemetry snapshot = {
        .battery_raw = power->battery_raw,
        .battery_mv = power->battery_mv,
        .flags = power->flags,
        .sample_count = power->samples,
        .failure_count = power->consecutive_failures,
        .charger_requested_mode = power_charger_mode(),
        .gpo_enable = PP_GPO32_ENABLE,
        .gpo_value = PP_GPO32_VAL,
        .gpo_input = PP_GPO32_INPUT_VAL,
        .usb_configured = pjs_usb_cdc_configured(),
        .usb_suspended = pjs_usb_device_suspended(),
    };
    pjs_usb_protocol_publish_power(protocol, &snapshot);
}

static uint32_t runtime_failure_status(void)
{
    return qjs_runtime_error_code() == PJS_QJS_ERROR_FRAME_BUDGET ?
        PJS_RUNTIME_ERROR_BUDGET : PJS_RUNTIME_ERROR;
}

static uint32_t error_magnitude(int32_t result)
{
    return result < 0 ? (uint32_t)(-(int64_t)result) : (uint32_t)result;
}

static void reset_core_after_failed_guest(void)
{
    pjs_core_shutdown();
    if (pjs_core_init() != 0) panic_code(0x50314331u); /* P1C1 */
}

static bool boot_embedded_recovery(PjsGuestPackage *guest,
                                   PjsCoreInput *input)
{
    reset_core_after_failed_guest();
    *guest = (PjsGuestPackage){0};
    if (pjs_package_open_ipod_photo(
            pjs_embedded_package, pjs_embedded_package_length, guest) != 0) {
        return false;
    }
    input->runtime_status = PJS_RUNTIME_PACKAGE_ADMITTED;
    if (!qjs_runtime_boot(guest) || !qjs_runtime_frame(input)) {
        qjs_runtime_shutdown();
        return false;
    }
    input->runtime_status = PJS_RUNTIME_READY;
    return true;
}

static void remember_boot_failure(uint32_t stage, uint32_t code,
                                  uint32_t *failure_stage,
                                  uint32_t *failure_code)
{
    if (*failure_stage != PJS_BOOT_FAILURE_NONE) return;
    *failure_stage = stage;
    *failure_code = code;
}

static PjsCoreInput core_input(const PjsInputState *input,
                               const PjsPowerTelemetry *power,
                               const PjsScheduler *scheduler,
                               int32_t wheel_delta,
                               bool cache_enabled,
                               uint32_t last_frame_us,
                               uint32_t runtime_status)
{
    return (PjsCoreInput){
        .buttons = input->buttons,
        .wheel_delta = wheel_delta,
        .wheel_position = input->wheel_position,
        .wheel_touched = input->wheel_touched ? 1u : 0u,
        .hold = input->hold ? 1u : 0u,
        .battery_mv = power->battery_mv,
        .power_flags = power->flags,
        .dropped_ticks = scheduler->dropped_ticks,
        .cache_enabled = cache_enabled ? 1u : 0u,
        .last_frame_us = last_frame_us,
        .runtime_status = runtime_status,
    };
}

static uint32_t render_and_present(void)
{
    pjs_audio_stream_gate_refill();
    uint32_t started = timer_now_us();
    last_presented_damage_area = 0u;
    PjsCoreDamagePlan damage = {0};
    if (pjs_core_render_damage(framebuffer, (uint32_t)PJS_FRAME_PIXELS, &damage) != 0) {
        panic_code(0x50314333u); /* P1C3 */
    }
    /* Composite the system lock in the status bar without changing the
     * renderer's retained pixels. Unlock restores the app beneath it. */
    uint16_t beneath[12u * 12u];
    if (status_hold) {
        for (uint32_t y = 0u; y < 12u; ++y)
            for (uint32_t x = 0u; x < 12u; ++x) {
                uint32_t at = (y + 6u) * 220u + x + 104u;
                beneath[y * 12u + x] = framebuffer[at];
                bool shackle = y < 6u && x >= 3u && x <= 8u &&
                    (y < 2u || x < 5u || x > 6u);
                bool body = y >= 5u && y <= 10u && x >= 1u && x <= 10u;
                bool keyhole = x >= 5u && x <= 6u && y >= 7u && y <= 9u;
                if (shackle || body) framebuffer[at] = keyhole ? 0xffffu : 0x18e3u;
            }
    }
    if (status_hold != presented_hold) {
        if (damage.count < PJS_CORE_MAX_DAMAGE_REGIONS) {
            damage.regions[damage.count++] = (PjsCoreDamageRect){104, 6, 116, 18};
            damage.area += 144u;
        } else {
            damage.count = 1u;
            damage.full_redraw = 1u;
            damage.area = PJS_FRAME_PIXELS;
            damage.regions[0] = (PjsCoreDamageRect){0, 0, 220, 176};
        }
    }
    observed_performance.render_us = timer_now_us() - started;
    pjs_audio_stream_gate_refill();
    uint32_t lcd_started = timer_now_us();
    if (damage.count != 0u) {
        /* LCD transfer is programmed CPU I/O, not memory DMA. The CPU reads
         * cached framebuffer pixels and copies them into the bridge, so no
         * cache writeback is required for this presentation path. */
        if (!lcd_present_damage(framebuffer, PJS_FRAME_PIXELS, &damage)) {
            panic_code(0x50314c32u); /* P1L2 */
        }
        last_presented_damage_area = damage.area;
        ++observed_performance.present_count;
    }
    if (status_hold)
        for (uint32_t y = 0u; y < 12u; ++y)
            for (uint32_t x = 0u; x < 12u; ++x)
                framebuffer[(y + 6u) * 220u + x + 104u] = beneath[y * 12u + x];
    presented_hold = status_hold;
    observed_performance.lcd_us = timer_now_us() - lcd_started;
    uint32_t elapsed = timer_now_us() - started;
    observed_performance.last_present_us = elapsed;
    if (elapsed > observed_performance.max_present_us)
        observed_performance.max_present_us = elapsed;
    return elapsed;
}

static uint32_t kernel_power_mode(uint8_t flags)
{
    if ((flags & PJS_POWER_USB) != 0u) return 2u;
    if ((flags & PJS_POWER_FIREWIRE) != 0u) return 3u;
    return 1u;
}

static bool kernel_lcd_cycle(void)
{
    PjsLcdClockState clock_state = {0};
    lcd_clock_snapshot(&clock_state);
    backlight_enable(false);
    if (!lcd_sleep()) {
        backlight_enable(true);
        return false;
    }
    timer_delay_us(750000u);
    if (!lcd_wake() || !lcd_present(framebuffer, PJS_FRAME_PIXELS)) {
        backlight_enable(true);
        return false;
    }
    if (!lcd_clock_state_matches(&clock_state)) {
        backlight_enable(true);
        return false;
    }
    backlight_enable(true);
    return true;
}

static uint32_t kernel_shutdown_preflight(bool runtime_active,
                                          bool lineage_ready,
                                          uint8_t filtered_power)
{
    if (!runtime_active || !lineage_ready || !memory_integrity_ok()) return 1u;
    if (!lcd_ready()) return 2u;
    if ((filtered_power & PJS_POWER_SOURCE_UNSTABLE) != 0u) return 3u;
    PjsStorageDiskHandoff handoff = {0};
    if (pjs_storage_ata_quiesce(&handoff) != PJS_STORAGE_OK) {
        return 10u + handoff.state;
    }
    return 0u;
}

#define PJS_POWER_IDLE_SLEEP_US 120000000u
#define PJS_POWER_CHORD_HOLD_US 2000000u

static bool kernel_commit_clean_lineage(PjsLineageState *lineage)
{
    if (lineage == 0 || lineage->available == 0u) {
        return false;
    }
    /* A previous terminal attempt may have committed ACTIVE and then failed
     * while flushing or standing by the disk. Treat that already-clean
     * record as idempotent so a retry can finish the storage sequence. */
    if (lineage->record.phase == PJS_LINEAGE_PHASE_ACTIVE &&
        lineage->record.trial_source == PJS_BOOT_SOURCE_NONE &&
        lineage->record.failure_stage == PJS_BOOT_FAILURE_NONE &&
        lineage->record.failure_code == 0u) return true;
    if (lineage->record.phase == PJS_LINEAGE_PHASE_QUEUED)
        return lineage->record.trial_source == PJS_BOOT_SOURCE_PACKAGE_BANK &&
            (lineage->record.trial_hash_low != 0u || lineage->record.trial_hash_high != 0u) &&
            lineage->record.failure_stage == 0u && lineage->record.failure_code == 0u;
    if (lineage->record.phase != PJS_LINEAGE_PHASE_RUNNING) return false;
    PjsLineageRecord clean = lineage->record;
    clean.phase = PJS_LINEAGE_PHASE_ACTIVE;
    clean.trial_source = PJS_BOOT_SOURCE_NONE;
    clean.trial_hash_low = 0u;
    clean.trial_hash_high = 0u;
    clean.failure_stage = PJS_BOOT_FAILURE_NONE;
    clean.failure_code = 0u;
    return pjs_storage_lineage_write(lineage, &clean) == PJS_STORAGE_OK;
}

static bool kernel_suspend(PjsPowerLifecycle *lifecycle,
                           PjsStorageDiskPower *disk_power,
                           PjsAudioState *audio)
{
    if (lifecycle == 0 || disk_power == 0 || lifecycle->suspended != 0u ||
        !lcd_ready()) return false;
    /* Quiesce the codec before touching the disk rail or panel. A codec I2C
     * fault is diagnostic only; the audio module always disables its FIFO and
     * the lifecycle can still take the safe storage path. */
    if (pjs_audio_pcm_suspend() != 0) return false;
    if (audio != 0) (void)pjs_audio_stop(audio);
    PjsLcdClockState clock_state = {0};
    lcd_clock_snapshot(&clock_state);
    if (pjs_storage_disk_flush_standby_off(disk_power) != PJS_STORAGE_OK) {
        (void)pjs_audio_pcm_resume();
        return false;
    }
    backlight_suspend();
    bool panel_sleep = lcd_sleep();
    if (!panel_sleep || !lcd_clock_state_matches(&clock_state)) {
        /* The disk is already in a safe state. Leave the backlight on if the
         * panel refuses the sleep command so a failed suspend is visible. */
        backlight_resume();
        (void)pjs_storage_disk_power_on();
        (void)pjs_audio_pcm_resume();
        return false;
    }
    power_lifecycle_set_suspended(lifecycle, true);
    ++batch_diagnostics.sleeps;
    return true;
}

static bool kernel_resume(PjsPowerLifecycle *lifecycle,
                          PjsStorageDiskPower *disk_power)
{
    if (lifecycle == 0 || lifecycle->suspended == 0u) return false;
    PjsLcdClockState clock_state = {0};
    lcd_clock_snapshot(&clock_state);
    if (pjs_storage_disk_power_on() != PJS_STORAGE_OK ||
        !lcd_wake() || !lcd_present(framebuffer, PJS_FRAME_PIXELS) ||
        !lcd_clock_state_matches(&clock_state)) {
        /* A resume refusal keeps the unit in the suspended state with the
         * display dark; no disk command is attempted after a failed wake. */
        return false;
    }
    if (pjs_audio_pcm_resume() != 0) {
        /* Keep a refused wake visibly asleep and storage quiescent. The
         * audio fault remains latched until an explicit runtime reset. */
        (void)lcd_sleep();
        (void)pjs_storage_disk_flush_standby_off(disk_power);
        return false;
    }
    power_lifecycle_set_suspended(lifecycle, false);
    ++batch_diagnostics.wakes;
    presented_hold = !status_hold; /* Restore the overlay after the LCD wake copy. */
    backlight_resume();
    return true;
}

static bool kernel_shutdown_storage(PjsLineageState *lineage,
                                     PjsStorageDiskPower *disk_power,
                                     PjsAudioState *audio)
{
    /* No storage command is issued while the codec can still be clocking. */
    if (pjs_audio_stream_gate_active()) {
        int result = pjs_audio_stream_gate_cancel();
        uint32_t mode, error;
        (void)pjs_audio_stream_gate_status(&mode, &error);
        if (result != 0) return false;
    }
    if (audio != 0) (void)pjs_audio_stop(audio);
    /* The lineage write can dirty the disk cache, so flush only after it is
     * committed. An already-clean ACTIVE record is accepted above, making a
     * retry safe if flush or standby failed after the first commit. */
    if (!kernel_commit_clean_lineage(lineage)) return false;
    if (pjs_storage_disk_flush(disk_power) != PJS_STORAGE_OK) return false;
    if (pjs_storage_disk_standby(disk_power) != PJS_STORAGE_OK) return false;
    return pjs_storage_disk_power_off(disk_power) == PJS_STORAGE_OK;
}

/* Play alone for two seconds is system shutdown in Apps and guest runtimes.
 * Flush while the runtime is still intact; a failed preflight keeps it alive. */
static void system_shutdown_key(const PjsInputState *input, uint32_t now,
                                uint32_t *started, bool *fired,
                                PjsLineageState *lineage,
                                PjsStorageDiskPower *disk, PjsAudioState *audio,
                                uint8_t sources)
{
    if (input->hold || input->buttons != PJS_BUTTON_PLAY) {
        *started = 0u;
        *fired = false;
        return;
    }
    if (*fired) return;
    if (*started == 0u) { *started = now; return; }
    if ((uint32_t)(now - *started) < PJS_POWER_CHORD_HOLD_US) return;
    *fired = true;
    if (pjs_audio_pcm_reset() != 0 || !kernel_shutdown_storage(lineage, disk, audio)) {
        pjs_core_set_kernel_diagnostic(8u, 75u);
        return;
    }
    qjs_runtime_shutdown();
    pjs_core_shutdown();
    timer_irq_stop();
    irq_disable_global();
    pjs_usb_device_shutdown();
    backlight_suspend();
    (void)lcd_sleep();
    (void)power_charger_set_mode(PJS_POWER_CHARGER_SUSPEND);
    /* CHGWAK would immediately wake an iPod whose USB cable is attached. */
    uint8_t wake = PJS_POWER_WAKE_EXTERNAL;
    if (sources & PJS_POWER_USB) wake &= ~PJS_POWER_WAKE_USB;
    for (uint32_t attempt = 0u; attempt < 3u; ++attempt)
        if (power_request_standby(wake) == PJS_POWER_RESULT_OK) break;
    for (;;) PP_CPU_CTL = PP_PROC_SLEEP;
}

static void kernel_stop_runtime(PjsStorageFile *disk_package,
                                bool *runtime_active)
{
    if (runtime_active != 0 && *runtime_active) {
        qjs_runtime_shutdown();
        *runtime_active = false;
    }
    if (disk_package != 0) pjs_storage_release(disk_package);
}

static const char *package_name_for_source(uint32_t source)
{
    for (uint32_t index = 0u;
         index < sizeof(boot_slots) / sizeof(boot_slots[0]); ++index) {
        if (boot_slots[index].source == source) return boot_slots[index].file_name;
    }
    return 0;
}

static bool package_hash_matches(const PjsGuestPackage *guest,
                                 uint32_t low, uint32_t high)
{
    return guest->package_hash_low == low && guest->package_hash_high == high;
}

static bool retained_bank_identity(const PjsLineageRecord *record,
                                   uint32_t *low_out, uint32_t *high_out)
{
    if (record == 0 || low_out == 0 || high_out == 0) return false;
    const uint32_t sources[2] = {record->active_source,
                                 record->last_good_source};
    const uint32_t lows[2] = {record->active_hash_low,
                              record->last_good_hash_low};
    const uint32_t highs[2] = {record->active_hash_high,
                               record->last_good_hash_high};
    for (uint32_t ref = 0u; ref < 2u; ++ref) {
        if (sources[ref] != PJS_BOOT_SOURCE_PACKAGE_BANK ||
            (lows[ref] == 0u && highs[ref] == 0u) ||
            (lows[ref] == record->rejected_hash_low &&
             highs[ref] == record->rejected_hash_high)) continue;
        for (uint32_t bank = 0u; bank < PJS_PACKAGE_STORE_BANK_COUNT; ++bank) {
            PjsPackageStoreHeader header = {0};
            if (pjs_storage_package_bank_inspect(bank, &header) !=
                    PJS_STORAGE_OK ||
                header.hash_low != lows[ref] || header.hash_high != highs[ref])
                continue;
            PjsStorageFile file = {0};
            PjsPackageStoreHeader checked = {0};
            PjsGuestPackage guest = {0};
            int32_t result = pjs_storage_load_package_bank(
                bank, lows[ref], highs[ref], &file, &checked);
            bool admitted = result == PJS_STORAGE_OK &&
                pjs_package_open_ipod_photo(file.bytes, file.length, &guest) == 0 &&
                guest.package_hash_low == lows[ref] &&
                guest.package_hash_high == highs[ref];
            pjs_storage_release(&file);
            if (admitted) {
                *low_out = lows[ref];
                *high_out = highs[ref];
                return true;
            }
        }
    }
    return false;
}

/* Catalog directory entries carry names, not lineage identity. Resolve an
 * identity by opening each bounded entry and comparing the admitted package
 * hash; the filename is never synthesized or written back to FAT. */
static int32_t catalog_index_for_hash(const PjsStorageCatalog *catalog,
                                      uint32_t low, uint32_t high)
{
    if (catalog == 0 || (low == 0u && high == 0u)) return -1;
    for (uint32_t index = 0u; index < catalog->count &&
         index < PJS_STORAGE_MAX_APPS; ++index) {
        PjsStorageFile file = {0};
        if (pjs_storage_load_app(
                &file, catalog->apps[index].file_name) != PJS_STORAGE_OK) continue;
        PjsGuestPackage package = {0};
        bool match = pjs_package_open_ipod_photo(
            file.bytes, file.length, &package) == 0 &&
            package_hash_matches(&package, low, high);
        pjs_storage_release(&file);
        if (match) return (int32_t)index;
    }
    return -1;
}

typedef struct {
    uint32_t source;
    uint32_t hash_low;
    uint32_t hash_high;
    int32_t catalog_index;
} BootCandidate;

static bool candidate_add(BootCandidate candidates[8], uint32_t *count,
                          uint32_t source, uint32_t low, uint32_t high,
                          int32_t catalog_index)
{
    if (source == PJS_BOOT_SOURCE_NONE || source > PJS_BOOT_SOURCE_PACKAGE_BANK) {
        return false;
    }
    for (uint32_t index = 0u; index < *count; ++index) {
        if (candidates[index].source == source &&
            candidates[index].hash_low == low &&
            candidates[index].hash_high == high &&
            candidates[index].catalog_index == catalog_index) return true;
    }
    if (*count >= 8u) return false;
    candidates[(*count)++] = (BootCandidate){source, low, high, catalog_index};
    return true;
}

static void catalog_labels(const PjsStorageCatalog *catalog,
                           uint8_t labels[PJS_STORAGE_MAX_APPS][9])
{
    for (uint32_t app = 0u; app < PJS_STORAGE_MAX_APPS; ++app) {
        for (uint32_t index = 0u; index < 9u; ++index) labels[app][index] = 0u;
        if (app >= catalog->count) continue;
        for (uint32_t index = 0u; index < 8u; ++index) {
            char character = catalog->apps[app].file_name[index];
            if (character == ' ') break;
            labels[app][index] = (uint8_t)character;
        }
    }
}

static int32_t run_package_launcher(const PjsStorageCatalog *catalog,
                                    PjsInputState *input,
                                    PjsPowerTelemetry *power,
                                    bool cache_enabled
                                    , PjsUsbProtocol *observer, bool usb_started,
                                    uint32_t *observed_epoch, PjsLineageState *lineage
                                    )
{
    uint8_t labels[PJS_STORAGE_MAX_APPS][9];
    catalog_labels(catalog, labels);
    if (!qjs_runtime_set_launcher_catalog(
            &labels[0][0], catalog->count, sizeof(labels[0]))) return -1;

    PjsStorageFile launcher_file = {0};
    int32_t storage_result = pjs_storage_load_guest_named(
        &launcher_file, launcher_package);
    if (storage_result != PJS_STORAGE_OK) {
        qjs_runtime_set_launcher_catalog(0, 0u, 0u);
        return -1;
    }
    PjsGuestPackage launcher_guest = {0};
    if (pjs_package_open_ipod_photo(
            launcher_file.bytes, launcher_file.length, &launcher_guest) != 0 ||
        launcher_guest.package_hash_low != PJS_LAUNCHER_PACKAGE_HASH_LOW ||
        launcher_guest.package_hash_high != PJS_LAUNCHER_PACKAGE_HASH_HIGH ||
        !qjs_runtime_boot(&launcher_guest)) {
        pjs_storage_release(&launcher_file);
        qjs_runtime_set_launcher_catalog(0, 0u, 0u);
        reset_core_after_failed_guest();
        return -1;
    }

    PjsScheduler scheduler = {0};
    int32_t pending_wheel_delta = 0;
    uint32_t last_frame_us = 0u;
    uint32_t next_frame = timer_now_us();
    uint32_t shutdown_key_start = 0u;
    bool shutdown_key_fired = false;
    uint32_t exit_chord_start = 0u;
    uint32_t now = timer_now_us();
    PjsPowerLifecycle launcher_lifecycle;
    PjsStorageDiskPower launcher_disk_power = {0};
    PjsPowerSourceFilter launcher_source_filter;
    power_lifecycle_init(&launcher_lifecycle);
    power_source_filter_init(&launcher_source_filter);
    uint32_t launcher_last_activity = timer_now_us();
    uint32_t launcher_next_power_sample = launcher_last_activity + 250000u;
    uint32_t launcher_previous_buttons = input->buttons;
    /* A warm return can arrive with the system chord still held. Consume
     * that gesture before accepting navigation or another restart. */
    bool launcher_wait_release = true;
    bool launcher_power_initialized = false;
    uint32_t launcher_frames = 0u;
    for (;;) {
        if (usb_started) {
            pjs_usb_device_poll();
            uint32_t epoch = pjs_usb_cdc_epoch();
            if (epoch != *observed_epoch) {
                pjs_usb_protocol_reset_observer_transport(observer);
                *observed_epoch = epoch;
            }
            if (pjs_usb_cdc_configured()) {
                publish_usb_power(observer, power);
                pjs_usb_protocol_publish_performance(observer, &observed_performance);
                pjs_usb_protocol_publish_observation(observer, true,
                    launcher_frames, 0u, launcher_guest.package_hash_low,
                    launcher_guest.package_hash_high);
                /* Apps is a resident runtime too. Advertise the same
                 * observer-to-maintenance transition available to ordinary
                 * apps, but defer its preflight until the outer lifecycle has
                 * initialized audio and power state. */
                observer->maintenance_available = true;
                (void)pjs_usb_protocol_poll(observer, 1u);
                if (pjs_usb_cdc_tx_idle()) {
                    /* Protocol poll parses frames; service performs the
                     * actual CDC write that promotes ACK_PENDING to ACKED. */
                    pjs_usb_protocol_service(observer);
                }
                if (observer->maintenance_request &&
                    observer->maintenance_ack_written &&
                    pjs_usb_cdc_tx_idle()) {
                    qjs_runtime_shutdown();
                    pjs_storage_release(&launcher_file);
                    qjs_runtime_set_launcher_catalog(0, 0u, 0u);
                    reset_core_after_failed_guest();
                    observer->maintenance_available = false;
                    return -2; /* outer loop owns maintenance handoff */
                }
            }
        }
        poll_system_input(input);
        now = timer_now_us();
        if ((int32_t)(now - launcher_next_power_sample) >= 0) {
            if (!launcher_power_initialized) {
                /* The launcher runs before the ordinary app's telemetry
                 * initialization. Configure I2C and the charging input here
                 * too, so source qualification does not depend on inherited
                 * GPIO setup or remain permanently unstable on a cold boot. */
                power_telemetry_init();
                launcher_power_initialized = true;
            }
            PjsPowerSourceSample launcher_source_sample = {0};
            power_telemetry_sample(power);
            power_source_sample_read_only(&launcher_source_sample);
            uint8_t launcher_source = power_source_filter_update(
                &launcher_source_filter, launcher_source_sample.flags,
                launcher_source_sample.valid_mask);
            power->flags = (power->flags &
                ~(PJS_POWER_SOURCE_MASK | PJS_POWER_SOURCE_UNSTABLE)) |
                launcher_source;
            if ((launcher_source & PJS_POWER_SOURCE_UNSTABLE) == 0u) {
                launcher_lifecycle.charger_mode = power_charger_mode();
            }
            launcher_next_power_sample = now + 250000u;
        }
        bool launcher_button_edge = input->buttons != launcher_previous_buttons;
        launcher_previous_buttons = input->buttons;
        if (launcher_button_edge || input->wheel_delta != 0 ||
            input->wheel_touched) {
            launcher_last_activity = now;
        }
        bool launcher_external =
            (power->flags & (PJS_POWER_EXTERNAL_MASK |
                             PJS_POWER_SOURCE_UNSTABLE)) != 0u;
        launcher_external = launcher_external || pjs_usb_cdc_configured();
        if (pjs_audio_pcm_busy()) launcher_last_activity = now;
        if (launcher_lifecycle.suspended != 0u) {
            bool wake_gesture = launcher_external ||
                input->buttons != 0u || input->wheel_delta != 0 ||
                input->wheel_touched;
            if (wake_gesture &&
                kernel_resume(&launcher_lifecycle, &launcher_disk_power)) {
                /* A wake gesture belongs to lifecycle, not the launcher.
                 * Drop its wheel/button facts, reset the frame clock, and
                 * redraw before accepting new navigation. */
                pending_wheel_delta = 0;
                last_frame_us = 0u;
                next_frame = now;
                input->buttons = 0u;
                input->wheel_delta = 0;
                input->wheel_touched = false;
                launcher_wait_release = true;
                (void)render_and_present();
                launcher_last_activity = now;
            }
            idle_until_service();
            continue;
        }
        if (launcher_wait_release) {
            pending_wheel_delta = 0;
            if (input->buttons != 0u || input->wheel_touched) {
                idle_until_service();
                continue;
            }
            launcher_wait_release = false;
            input->wheel_delta = 0;
            next_frame = now;
        }
        if (!launcher_external
            && !pjs_audio_pcm_busy()
            &&
            (uint32_t)(now - launcher_last_activity) >=
                PJS_POWER_IDLE_SLEEP_US) {
            if (kernel_suspend(&launcher_lifecycle, &launcher_disk_power,
                               0)) {
                pending_wheel_delta = 0;
                last_frame_us = 0u;
                continue;
            }
            /* Do not retry a failed suspend on every launcher frame. */
            launcher_last_activity = now;
        }
        system_shutdown_key(input, now, &shutdown_key_start, &shutdown_key_fired,
                            lineage, &launcher_disk_power, 0, power->flags);
        pending_wheel_delta = clamp_wheel_delta(
            pending_wheel_delta + (int32_t)input->wheel_delta);
        now = timer_now_us();
        uint32_t chord = PJS_BUTTON_MENU | PJS_BUTTON_LEFT;
        if (!input->hold && (input->buttons & chord) == chord) {
            if (exit_chord_start == 0u) exit_chord_start = now;
            if ((uint32_t)(now - exit_chord_start) >= 2000000u) pp_reboot();
            /* System restart has the same chord as an ordinary app. Do not
             * deliver its held Left button as launcher navigation. */
            pending_wheel_delta = 0;
            continue;
        } else {
            exit_chord_start = 0u;
        }
        if ((int32_t)(now - next_frame) < 0) {
            idle_until_service();
            continue;
        }
        next_frame = now + PJS_RENDER_GAP_US;

        PjsCoreInput frame_input = core_input(
            input, power, &scheduler, pending_wheel_delta, cache_enabled,
            last_frame_us, PJS_RUNTIME_READY_DISK);
        pending_wheel_delta = 0;
        uint32_t frame_started = timer_now_us();
        if (!qjs_runtime_frame(&frame_input) || pjs_core_step(&frame_input) < 0) {
            qjs_runtime_shutdown();
            pjs_storage_release(&launcher_file);
            qjs_runtime_set_launcher_catalog(0, 0u, 0u);
            reset_core_after_failed_guest();
            return -1;
        }
        ++launcher_frames;
        observed_performance.last_frame_us = timer_now_us() - frame_started;
        if (observed_performance.last_frame_us > observed_performance.max_frame_us)
            observed_performance.max_frame_us = observed_performance.last_frame_us;
        int32_t selection = qjs_runtime_launcher_selection();
        if (pjs_core_needs_render() != 0u || status_hold != presented_hold) last_frame_us = render_and_present();
        /* Consume the launch gesture before the next guest installs handlers. */
        if (selection < 0 || input->hold || input->buttons != 0u || input->wheel_touched) continue;

        uint32_t teardown_started = timer_now_us();
        qjs_runtime_shutdown();
        pjs_storage_release(&launcher_file);
        qjs_runtime_set_launcher_catalog(0, 0u, 0u);
        reset_core_after_failed_guest();
        observed_performance.launcher_teardown_us = timer_now_us() - teardown_started;
        return selection;
    }
}

void kernel_main(void)
{
    disable_interrupt_sources();
    cpu_idle_init();
    /* Bootloader handoff inherits a 24 MHz source on the measured A1099.
     * Establish our CPU baseline before LCD/USB ownership. Hold inherited
     * USB engines in reset and stop DMA0 before the IRAM memory transition.
     * USB initialization later releases its two blocks in the usual order. */
    PP_DEV_RS |= PP_DEV_USB0 | PP_DEV_USB1;
    PP_DEV_EN &= ~(PP_DEV_USB0 | PP_DEV_USB1);
    if (pjs_audio_dma_stop() != PJS_AUDIO_DMA_RESULT_OK ||
        pjs_cpu_clock_init() != 0) panic_code(0x434c4b30u); /* CLK0 */
    cache_take_ownership_disabled();
    bool cache_enabled = false;
    initialize_heap();

    uint8_t *usb_staging = pjs_heap_alloc(PJS_USB_STAGING_BYTES, 32u);
    if (usb_staging == 0) panic_code(0x50315531u); /* P1U1 */
    PjsUsbProtocol usb_protocol = {0};
    PjsUsbProtocolConfig usb_protocol_config = {
        .io = {
            .transport = 0,
            .read = usb_protocol_read,
            .write = usb_protocol_write,
        },
        .rx_buffer = usb_protocol_rx,
        .rx_capacity = sizeof(usb_protocol_rx),
        .tx_buffer = usb_protocol_tx,
        .tx_capacity = sizeof(usb_protocol_tx),
        .package_buffer = usb_staging,
        .package_capacity = PJS_USB_STAGING_BYTES,
        .max_payload = PJS_USB_WIRE_PAYLOAD_BYTES,
        .observe_only = true,
    };
    if (!pjs_usb_protocol_init(&usb_protocol, &usb_protocol_config)) {
        panic_code(0x50315532u); /* P1U2 */
    }
    bool usb_takeover = false;
    bool usb_started = false;
    uint32_t observed_frames = 0u;
    uint32_t observed_usb_epoch = 0u;

    input_init();
    if (!lcd_init()) panic_code(0x50314c31u); /* P1L1 */
    backlight_init();

    /* Every development image must remain observable even if package
     * admission, FAT startup, or a later hardware gate stalls. */
    usb_started = pjs_usb_device_init() == PJS_USB_OK;
    if (usb_started) {
        uint32_t usb_wait_start = timer_now_us();
        while (!pjs_usb_cdc_configured() &&
               (uint32_t)(timer_now_us() - usb_wait_start) < 5000000u) {
            pjs_usb_device_poll();
        }
        /* PP5020 cache ownership before controller setup prevents this board
         * from enumerating.  Once CDC is configured, all controller DMA lives
         * in IRAM and can coexist with cached SDRAM.  This late path keeps the
         * proven uncached enumeration sequence while restoring enough CPU
         * throughput for generated QuickJS bundles. */
        /* The observer must remain fast with or without a configured host. */
        cache_enabled = cache_take_ownership_enabled();
    }
    /* Controller initialization failure must not strand standalone apps in
     * the slow uncached profile. Never reinitialize USB after this point. */
    if (!usb_started) cache_enabled = cache_take_ownership_enabled();

    /* A configured development host owns the boot transaction. Do not enter
     * synchronous ATA/FAT discovery before the CDC protocol can be serviced;
     * let the host start a guest and mount its store after USB takeover.
     * Cable-free standalone boot keeps the complete
     * lineage and persistence path. */
    bool usb_boot_takeover = false;

boot_apps:
    ; /* Re-enter only after runtime/core teardown; hardware remains owned. */
    PjsPowerTelemetry power = {
        .battery_mv = PJS_PLACEHOLDER_BATTERY_MV,
        .flags = (uint8_t)(power_source_flags_read() | PJS_POWER_TELEMETRY_DISABLED),
    };
    observed_frames = 0u;
    observed_performance = (PjsUsbPerformance){0};
    observed_performance.flags = cache_enabled ? 1u : 0u;
    if (pjs_core_backend_marker() != PJS_CORE_BACKEND_RUST_MAGIC) {
        panic_code(0x50314245u); /* P1BE */
    }
    if (pjs_core_init() != 0) panic_code(0x50314331u); /* P1C1 */

    uint32_t runtime_status = PJS_RUNTIME_DISABLED;
    bool runtime_active = false;
    bool runtime_from_disk = false;
    bool runtime_from_catalog = false;
    bool launcher_maintenance_pending = false;
    uint32_t catalog_selection = 0u;
    uint32_t catalog_count = 0u;
    uint32_t boot_source = PJS_BOOT_SOURCE_NONE;
    uint32_t boot_failure_stage = PJS_BOOT_FAILURE_NONE;
    uint32_t boot_failure_code = 0u;
    PjsStorageFile disk_package = {0};
    PjsGuestPackage guest = {0};
    PjsInputState input = {0};
    poll_system_input(&input);
    PjsScheduler initial_scheduler = {0};
    PjsCoreInput initial_input = core_input(
        &input, &power, &initial_scheduler, 0, cache_enabled, 0u,
        PJS_RUNTIME_PACKAGE_ADMITTED);
    pjs_storage_reset_diagnostics();
    PjsStorageCatalog catalog = {0};
    int32_t discovery_result = pjs_storage_discover_apps(&catalog);
    bool package_bank_ready = false;
    PjsLineageState lineage = {0};
    int32_t lineage_result = usb_boot_takeover ? PJS_STORAGE_ERR_BUSY :
        pjs_storage_lineage_load(&lineage);
    bool lineage_ready = lineage_result == PJS_STORAGE_OK;
    bool lineage_accept_pending = false;
    uint32_t lineage_event = 4u;
    PjsLineageRecord lineage_accept = {0};
    bool admission_rejected = false;
    bool embedded_recovery_attempted = false;

    if (lineage_ready) {
        PjsLineageRecord boot_record = lineage.record;
        bool queued_package = boot_record.phase == PJS_LINEAGE_PHASE_QUEUED &&
            boot_record.trial_source == PJS_BOOT_SOURCE_PACKAGE_BANK;
        bool rollback = boot_record.phase != PJS_LINEAGE_PHASE_ACTIVE &&
                        !queued_package;
        PjsGuestPackage embedded_identity = {0};
        if (pjs_package_open_ipod_photo(
                pjs_embedded_package, pjs_embedded_package_length,
                &embedded_identity) != 0) {
            lineage_ready = false;
            lineage_result = PJS_STORAGE_ERR_STATE;
        }
        BootCandidate candidates[8] = {0};
        uint32_t source_count = 0u;
        if (queued_package && package_bank_ready) {
            candidate_add(candidates, &source_count,
                          PJS_BOOT_SOURCE_PACKAGE_BANK,
                          boot_record.trial_hash_low,
                          boot_record.trial_hash_high, -1);
        }
        if (!queued_package && !rollback && discovery_result == PJS_STORAGE_OK) {
            PjsStorageCatalog launcher_catalog = catalog;
            uint32_t retained_low = 0u;
            uint32_t retained_high = 0u;
            int32_t retained_index = -1;
            if (catalog.count < PJS_STORAGE_MAX_APPS && package_bank_ready &&
                retained_bank_identity(&boot_record, &retained_low,
                                       &retained_high)) {
                static const char retained_name[11] =
                    {'U','S','B','A','P','P',' ',' ','P','K','T'};
                retained_index = (int32_t)launcher_catalog.count;
                for (uint32_t index = 0u; index < sizeof(retained_name); ++index)
                    launcher_catalog.apps[retained_index].file_name[index] =
                        retained_name[index];
                launcher_catalog.apps[retained_index].size = 0u;
                ++launcher_catalog.count;
            }
            int32_t selection = run_package_launcher(
                &launcher_catalog, &input, &power, cache_enabled
                , &usb_protocol, usb_started, &observed_usb_epoch, &lineage
                );
            if (selection == -2) {
                launcher_maintenance_pending = true;
            }
            if (selection >= 0 && (uint32_t)selection < launcher_catalog.count) {
                if (selection == retained_index) {
                    candidate_add(candidates, &source_count,
                                  PJS_BOOT_SOURCE_PACKAGE_BANK,
                                  retained_low, retained_high, -1);
                } else {
                    candidate_add(candidates, &source_count,
                                  PJS_BOOT_SOURCE_CATALOG, 0u, 0u, selection);
                }
            }
        }
        if (queued_package) {
            candidate_add(candidates, &source_count, boot_record.active_source,
                          boot_record.active_hash_low,
                          boot_record.active_hash_high, -1);
            candidate_add(candidates, &source_count, boot_record.last_good_source,
                          boot_record.last_good_hash_low,
                          boot_record.last_good_hash_high, -1);
        } else if (!rollback) {
            if (boot_record.active_source != PJS_BOOT_SOURCE_PENDING) {
                candidate_add(candidates, &source_count,
                              PJS_BOOT_SOURCE_PENDING, 0u, 0u, -1);
            }
            candidate_add(candidates, &source_count, boot_record.active_source,
                          boot_record.active_hash_low, boot_record.active_hash_high, -1);
        }
        if (!queued_package) {
            candidate_add(candidates, &source_count, boot_record.last_good_source,
                          boot_record.last_good_hash_low, boot_record.last_good_hash_high, -1);
        }
        if (!rollback && !queued_package) {
            candidate_add(candidates, &source_count,
                          PJS_BOOT_SOURCE_LEGACY_APP, 0u, 0u, -1);
        }
        candidate_add(candidates, &source_count,
                      PJS_BOOT_SOURCE_EMBEDDED, 0u, 0u, -1);

        poll_system_input(&input);
        initial_input = core_input(&input, &power, &initial_scheduler,
                                   0, cache_enabled, 0u,
                                   PJS_RUNTIME_PACKAGE_ADMITTED);

        for (uint32_t index = 0u; lineage_ready && !runtime_active &&
             !launcher_maintenance_pending &&
             index < source_count; ++index) {
            uint32_t load_started = timer_now_us();
            BootCandidate candidate = candidates[index];
            uint32_t source = candidate.source;
            guest = (PjsGuestPackage){0};
            int32_t package_result = 0;
            int32_t catalog_index = -1;
            if (source == PJS_BOOT_SOURCE_EMBEDDED) {
                package_result = pjs_package_open_ipod_photo(
                    pjs_embedded_package, pjs_embedded_package_length, &guest);
            } else if (source == PJS_BOOT_SOURCE_PACKAGE_BANK) {
                package_result = PJS_STORAGE_ERR_NOT_FOUND;
                for (uint32_t bank = 0u; bank < PJS_PACKAGE_STORE_BANK_COUNT; ++bank) {
                    PjsPackageStoreHeader header = {0};
                    if (pjs_storage_package_bank_inspect(bank, &header) !=
                            PJS_STORAGE_OK ||
                        header.hash_low != candidate.hash_low ||
                        header.hash_high != candidate.hash_high) continue;
                    package_result = pjs_storage_load_package_bank(
                        bank, candidate.hash_low, candidate.hash_high,
                        &disk_package, &header);
                    break;
                }
                if (package_result == 0) {
                    package_result = pjs_package_open_ipod_photo(
                        disk_package.bytes, disk_package.length, &guest);
                }
            } else {
                const char *name = package_name_for_source(source);
                if (source == PJS_BOOT_SOURCE_CATALOG) {
                    catalog_index = candidate.catalog_index;
                    if (catalog_index < 0) {
                        catalog_index = catalog_index_for_hash(
                            &catalog, candidate.hash_low, candidate.hash_high);
                    }
                    if (catalog_index >= 0) {
                        name = catalog.apps[catalog_index].file_name;
                    }
                }
                if (name == 0) continue;
                int32_t storage_result = source == PJS_BOOT_SOURCE_CATALOG
                    ? pjs_storage_load_app(&disk_package, name)
                    : pjs_storage_load_guest_named(&disk_package, name);
                if (storage_result == PJS_STORAGE_ERR_NOT_FOUND) continue;
                if (storage_result != PJS_STORAGE_OK) {
                    remember_boot_failure(PJS_BOOT_FAILURE_STORAGE,
                                          pjs_storage_last_error(),
                                          &boot_failure_stage,
                                          &boot_failure_code);
                    continue;
                }
                package_result = pjs_package_open_ipod_photo(
                    disk_package.bytes, disk_package.length, &guest);
            }
            if (package_result != 0) {
                if (source == PJS_BOOT_SOURCE_PENDING && package_result == -4) {
                    admission_rejected = true;
                }
                remember_boot_failure(PJS_BOOT_FAILURE_PACKAGE,
                                      error_magnitude(package_result),
                                      &boot_failure_stage, &boot_failure_code);
                pjs_storage_release(&disk_package);
                continue;
            }

            if (!runtime_memory_admitted()) {
                remember_boot_failure(PJS_BOOT_FAILURE_MEMORY, 1u,
                                      &boot_failure_stage, &boot_failure_code);
                if (source == PJS_BOOT_SOURCE_PENDING) admission_rejected = true;
                pjs_storage_release(&disk_package);
                continue;
            }

            bool is_catalog_candidate = source == PJS_BOOT_SOURCE_CATALOG;
            bool is_package_bank_candidate =
                source == PJS_BOOT_SOURCE_PACKAGE_BANK;
            bool matches_active = source == boot_record.active_source &&
                package_hash_matches(&guest, boot_record.active_hash_low,
                                      boot_record.active_hash_high);
            bool is_new_pending = source == PJS_BOOT_SOURCE_PENDING && !matches_active;
            bool identity_required = candidate.hash_low != 0u || candidate.hash_high != 0u;
            if ((identity_required && !package_hash_matches(
                    &guest, candidate.hash_low, candidate.hash_high)) ||
                (is_new_pending && package_hash_matches(
                    &guest, boot_record.active_hash_low, boot_record.active_hash_high)) ||
                ((is_new_pending || is_catalog_candidate ||
                  is_package_bank_candidate) && package_hash_matches(
                    &guest, lineage.record.rejected_hash_low,
                    lineage.record.rejected_hash_high))) {
                pjs_storage_release(&disk_package);
                continue;
            }

            observed_performance.app_load_us = timer_now_us() - load_started;
            PjsLineageRecord trial = lineage.record;
            trial.phase = PJS_LINEAGE_PHASE_TRIAL;
            trial.trial_source = source;
            trial.trial_hash_low = guest.package_hash_low;
            trial.trial_hash_high = guest.package_hash_high;
            trial.failure_stage = PJS_BOOT_FAILURE_NONE;
            trial.failure_code = 0u;
            uint32_t trial_started = timer_now_us();
            if (pjs_storage_lineage_write(&lineage, &trial) != PJS_STORAGE_OK) {
                lineage_ready = false;
                pjs_storage_release(&disk_package);
                break;
            }

            observed_performance.lineage_commit_us = timer_now_us() - trial_started;
            runtime_status = PJS_RUNTIME_PACKAGE_ADMITTED;
            uint32_t boot_started = timer_now_us();
            runtime_active = qjs_runtime_boot(&guest);
            bool first_guest_frame = runtime_active && qjs_runtime_frame(&initial_input);
            observed_performance.app_boot_us = timer_now_us() - boot_started;
            if (first_guest_frame) {
                runtime_from_disk = source != PJS_BOOT_SOURCE_EMBEDDED;
                boot_source = source;
                lineage_event = rollback ? 1u :
                    ((is_new_pending || queued_package) ? 0u : 2u);
                if (admission_rejected && boot_record.generation == 1u) {
                    lineage_event = 5u;
                }
                lineage_accept = lineage.record;
                lineage_accept.phase = PJS_LINEAGE_PHASE_RUNNING;
                if (!matches_active) {
                    if (rollback) {
                        lineage_accept.last_good_source = PJS_BOOT_SOURCE_EMBEDDED;
                        lineage_accept.last_good_hash_low =
                            embedded_identity.package_hash_low;
                        lineage_accept.last_good_hash_high =
                            embedded_identity.package_hash_high;
                    } else {
                        lineage_accept.last_good_source = boot_record.active_source;
                        lineage_accept.last_good_hash_low = boot_record.active_hash_low;
                        lineage_accept.last_good_hash_high = boot_record.active_hash_high;
                    }
                    lineage_accept.active_source = source;
                    lineage_accept.active_hash_low = guest.package_hash_low;
                    lineage_accept.active_hash_high = guest.package_hash_high;
                }
                lineage_accept_pending = true;
                break;
            }

            uint32_t failure_stage = runtime_active ? PJS_BOOT_FAILURE_FRAME :
                                                      PJS_BOOT_FAILURE_QUICKJS;
            uint32_t failure_code = qjs_runtime_error_code();
            PjsLineageRecord crashed = lineage.record;
            crashed.phase = PJS_LINEAGE_PHASE_CRASHED;
            crashed.rejected_source = source;
            crashed.rejected_hash_low = guest.package_hash_low;
            crashed.rejected_hash_high = guest.package_hash_high;
            crashed.failure_stage = failure_stage;
            crashed.failure_code = failure_code;
            (void)pjs_storage_lineage_write(&lineage, &crashed);
            remember_boot_failure(failure_stage, failure_code,
                                  &boot_failure_stage, &boot_failure_code);
            if (runtime_active) qjs_runtime_shutdown();
            runtime_active = false;
            pjs_storage_release(&disk_package);
            reset_core_after_failed_guest();
            rollback = true;
        }
    }
    /* A configured development connection owns guest startup. Do not run
     * embedded JavaScript before the host can reach HELLO and RUN. */
    if (!runtime_active && !usb_boot_takeover &&
        !launcher_maintenance_pending) {
        pjs_storage_release(&disk_package);
        guest = (PjsGuestPackage){0};
        int32_t embedded_result = pjs_package_open_ipod_photo(
            pjs_embedded_package, pjs_embedded_package_length, &guest);
        boot_source = PJS_BOOT_SOURCE_EMBEDDED;
        if (embedded_result == 0) {
            runtime_status = PJS_RUNTIME_PACKAGE_ADMITTED;
            runtime_active = qjs_runtime_boot(&guest);
            if (runtime_active && !qjs_runtime_frame(&initial_input)) {
                boot_failure_stage = PJS_BOOT_FAILURE_FRAME;
                boot_failure_code = qjs_runtime_error_code();
                qjs_runtime_shutdown();
                runtime_active = false;
            } else if (!runtime_active) {
                boot_failure_stage = PJS_BOOT_FAILURE_EMBEDDED_QUICKJS;
                boot_failure_code = qjs_runtime_error_code();
            }
        } else {
            boot_failure_stage = PJS_BOOT_FAILURE_EMBEDDED_PACKAGE;
            boot_failure_code = error_magnitude(embedded_result);
        }
    }
    runtime_status = runtime_active ?
        (runtime_from_disk ? PJS_RUNTIME_READY_DISK : PJS_RUNTIME_READY) :
        (usb_boot_takeover ? PJS_RUNTIME_DISABLED : PJS_RUNTIME_ERROR);

    initial_input.runtime_status = runtime_status;
    if (runtime_from_catalog) {
        pjs_core_set_app_diagnostic(
            catalog_selection, catalog_count, pjs_storage_sector_read_count());
    } else {
        pjs_core_set_boot_diagnostic(boot_source, boot_failure_stage,
                                     boot_failure_code,
                                     pjs_storage_sector_read_count());
    }
    if (!lineage_ready) {
        pjs_core_set_lineage_diagnostic(
            6u, 0u, 0u, error_magnitude(lineage_result));
    }
    uint32_t first_present_started = timer_now_us();
    if (pjs_core_step(&initial_input) < 0) panic_code(0x50314332u); /* P1C2 */
    uint32_t last_frame_us = render_and_present();
    observed_performance.first_present_us = timer_now_us() - first_present_started;
    observed_performance.max_frame_us = 0u;
    observed_performance.max_present_us = 0u;
    if (lineage_ready && lineage_accept_pending) {
        uint32_t accept_started = timer_now_us();
        if (last_presented_damage_area == 0u) {
            pjs_core_set_lineage_diagnostic(
                6u, boot_source, lineage.record.generation,
                (uint32_t)(-PJS_STORAGE_ERR_VERIFY));
        } else if (pjs_storage_lineage_write(
                       &lineage, &lineage_accept) == PJS_STORAGE_OK) {
            pjs_core_set_lineage_diagnostic(
                lineage_event, boot_source, lineage.record.generation, 0u);
        } else {
            pjs_core_set_lineage_diagnostic(
                6u, boot_source, lineage.record.generation, lineage.error);
        }
        observed_performance.lineage_commit_us += timer_now_us() - accept_started;
        last_frame_us = render_and_present();
    }
    pjs_core_set_kernel_diagnostic(0u, 0u);
    last_frame_us = render_and_present();

    /* Start the 60 Hz clock only after the expensive initial frame. */
    if (runtime_active) ++batch_diagnostics.app_sessions;
    scheduler_global_reset();
    timer_irq_init();
    irq_enable_global();


    PjsAudioState audio = {0};
    pjs_audio_state_init(&audio);

    /* Keep the already-qualified boot frame independent from I2C. Telemetry
     * starts only after the UI, cache, timer and input paths are alive. */
    power_telemetry_init();
    /* Seed the first lifecycle decision before the idle loop. A failed ADC
     * read remains a telemetry fault and never becomes permission to shut
     * down. */
    power_telemetry_sample(&power);


    PjsPowerLifecycle lifecycle;
    power_lifecycle_init(&lifecycle);
    /* Start with the LTC4066 suspended until the filtered source policy has
     * observed a stable cable. This also clears an inherited fast-charge
     * setting from the loader before the first lifecycle sample. */
    uint32_t last_battery_sample = power.samples;
    uint8_t filtered_power = PJS_POWER_SOURCE_UNSTABLE;
    uint32_t last_power_mode = UINT32_MAX;
    bool lcd_tested = false;
    bool shutdown_ready = false;
    uint32_t next_source_sample = timer_now_us();
    uint32_t last_i2c_recovery_count = power_i2c_recovery_count();
    uint32_t last_user_activity = timer_now_us();
    uint32_t restart_chord_start = 0u;
    uint32_t suspend_chord_start = 0u;
    bool restart_chord_fired = false;
    bool suspend_chord_fired = false;
    bool automatic_shutdown_attempted = false;
    PjsStorageDiskPower disk_power = {0};

    int32_t pending_wheel_delta = 0;
    uint32_t shutdown_key_start = 0u;
    bool shutdown_key_fired = false;
    uint32_t exit_chord_start = 0u;
    uint32_t now = timer_now_us();
    uint32_t next_power_sample = now + 250000u;
    uint32_t next_present = now;
    uint32_t next_memory_check = now + 1000000u;
    uint32_t previous_buttons = input.buttons;
    bool previous_hold = input.hold;

    for (;;) {
        uint8_t lifecycle_wake_events = 0u;
        bool lifecycle_source_changed = false;
        if (usb_started) {
            pjs_usb_device_poll();
            uint32_t usb_epoch = pjs_usb_cdc_epoch();
            if (usb_epoch != observed_usb_epoch) {
                pjs_usb_protocol_reset_observer_transport(&usb_protocol);
                observed_usb_epoch = usb_epoch;
            }
            if (pjs_usb_cdc_configured()) {
                publish_usb_power(&usb_protocol, &power);
                usb_protocol.maintenance_available = !usb_takeover &&
                    (runtime_active || launcher_maintenance_pending) &&
                    lineage_ready && lifecycle.suspended == 0u &&
                    !pjs_audio_pcm_busy();
                pjs_usb_protocol_publish_performance(&usb_protocol, &observed_performance);
                pjs_usb_protocol_publish_observation(&usb_protocol,
                    runtime_active, observed_frames, boot_failure_code,
                    guest.package_hash_low, guest.package_hash_high);
                /* FAT/ATA access is deferred until the host has completed
                 * enumeration. Uploaded guests then fail closed if their
                 * persistent store cannot be mounted. */
                qjs_runtime_enable_fs_mount();
                if (!usb_takeover && !PJS_PRODUCTION) {
                    if (pjs_audio_stream_gate_active()) {
                        (void)pjs_audio_stream_gate_cancel();
                    }
                    (void)pjs_audio_stop(&audio);
                    if (runtime_active) qjs_runtime_shutdown();
                    runtime_active = false;
                    runtime_from_disk = false;
                    pjs_storage_release(&disk_package);
                    reset_core_after_failed_guest();
                    runtime_status = PJS_RUNTIME_DISABLED;
                    usb_takeover = true;
                }
                (void)pjs_usb_protocol_poll(&usb_protocol, 1u);
                /* Protocol writes enqueue bytes in CDC RAM. Wait for the
                 * actual IN completion before entering synchronous boot. */
                if (pjs_usb_cdc_tx_idle()) {
                    pjs_usb_protocol_service(&usb_protocol);
                }
            }

            /* Explicit host request only. The resident app keeps ownership
             * until its ACK drains and reversible preflight has succeeded. */
            if (!usb_takeover && pjs_usb_cdc_tx_idle() &&
                pjs_usb_protocol_take_maintenance_request(&usb_protocol)) {
                uint8_t *maintenance_buffer = 0;
                uint32_t failure = 0u;
                bool pcm_suspended = false;
                PjsStorageDiskPower maintenance_disk = {0};
                if ((!runtime_active && !launcher_maintenance_pending) ||
                    !lineage_ready || lifecycle.suspended != 0u ||
                    pjs_audio_pcm_busy() || !memory_integrity_ok()) failure = 1u;
                if (failure == 0u) {
                    maintenance_buffer = pjs_heap_alloc(4u * 1024u * 1024u, 32u);
                    if (maintenance_buffer == 0) failure = 2u;
                }
                if (failure == 0u) {
                    pcm_suspended = pjs_audio_pcm_suspend() == 0;
                    if (!pcm_suspended) failure = 3u;
                }
                /* Keep ATA powered and exclusively native. RAM guests use the
                 * existing per-app FS; no USB mass-storage ownership is granted. */
                if (failure == 0u &&
                    pjs_storage_disk_flush(&maintenance_disk) != PJS_STORAGE_OK)
                    failure = 4u;
                if (failure == 0u && !kernel_commit_clean_lineage(&lineage))
                    failure = 5u;
                if (failure != 0u) {
                    /* Keep the launcher-less resident state explicitly
                     * maintenance-ready so the host can retry after a
                     * transient preflight failure. A physical reboot remains
                     * the recovery path for a persistent storage fault. */
                    if (pcm_suspended && pjs_audio_pcm_resume() != 0) failure = 6u;
                    if (maintenance_buffer != 0) pjs_heap_free(maintenance_buffer);
                    (void)pjs_usb_protocol_result(&usb_protocol, failure, -1);
                    if (launcher_maintenance_pending) {
                        /* No guest remains after launcher teardown. Resume
                         * Apps rather than leave a failed request on a blank
                         * runtime; its USB loop will drain the queued result. */
                        irq_disable_global();
                        timer_irq_stop();
                        pjs_core_shutdown();
                        usb_protocol.maintenance_available = false;
                        pjs_usb_protocol_publish_observation(&usb_protocol,
                            false, 0u, 0u, 0u, 0u);
                        goto boot_apps;
                    }
                } else {
                    if (runtime_active) {
                        qjs_runtime_shutdown();
                        runtime_active = false;
                    }
                    launcher_maintenance_pending = false;
                    runtime_from_disk = false;
                    pjs_storage_release(&disk_package);
                    reset_core_after_failed_guest();
                    pjs_usb_protocol_publish_observation(&usb_protocol,
                        false, observed_frames, 0u, 0u, 0u);
                    if (!pjs_usb_protocol_activate_maintenance(
                            &usb_protocol, maintenance_buffer, 4u * 1024u * 1024u)) {
                        pjs_heap_free(maintenance_buffer);
                        boot_failure_code = 7u;
                        runtime_status = PJS_RUNTIME_ERROR;
                        (void)pjs_usb_protocol_result(&usb_protocol, 7u, -1);
                    } else {
                        pjs_heap_free(usb_staging);
                        usb_staging = maintenance_buffer;
                        usb_takeover = true;
                        runtime_status = PJS_RUNTIME_DISABLED;
                        (void)pjs_usb_protocol_result(&usb_protocol, 0u, 0);
                    }
                }
            }

            if (usb_takeover) {
                runtime_active = pjs_usb_protocol_is_running(&usb_protocol);
                runtime_status = runtime_active ? PJS_RUNTIME_READY :
                    (pjs_usb_protocol_state(&usb_protocol) ==
                         PJS_USB_STATE_ERROR ? PJS_RUNTIME_ERROR :
                                                PJS_RUNTIME_DISABLED);
            }

            if (usb_takeover && !runtime_active && pjs_usb_cdc_tx_idle() &&
                pjs_usb_protocol_take_reboot_request(&usb_protocol)) {
                if (!kernel_shutdown_storage(&lineage, &disk_power, &audio)) {
                    (void)pjs_usb_protocol_result(&usb_protocol, 8u, -1);
                } else {
                    timer_irq_stop();
                    irq_disable_global();
                    pjs_usb_device_shutdown();
                    pp_reboot();
                }
            }

            const uint8_t *chainload_image = 0;
            uint32_t chainload_length = 0u;
            if (pjs_usb_cdc_tx_idle() && pjs_usb_protocol_take_chainload(
                    &usb_protocol, &chainload_image, &chainload_length)) {
                if (pjs_audio_stream_gate_active()) {
                    (void)pjs_audio_stream_gate_cancel();
                }
                (void)pjs_audio_stop(&audio);
                irq_disable_global();
                timer_irq_stop();
                if (runtime_active) qjs_runtime_shutdown();
                pjs_core_shutdown();
                pjs_usb_device_shutdown();
                pjs_native_image_chainload(chainload_image,
                                           chainload_length);
            }
            /* Upload/idle control traffic must not wait behind an uncached
             * 60 Hz render and telemetry pass. RUN flips runtime_active and
             * rejoins the normal guest frame loop on the next iteration. */
            if (usb_takeover && !runtime_active) {
                if (!usb_protocol.upload_active && !usb_protocol.runtime_boot_pending)
                    idle_until_service();
                continue;
            }
        }
        /* This path remains hot while no frame is required. It samples input
         * continuously and preserves wheel motion until the next fixed step. */
        poll_system_input(&input);
        bool buttons_changed = input.buttons != previous_buttons;
        uint32_t pressed = input.buttons & ~previous_buttons;
        previous_buttons = input.buttons;
        if (lifecycle.suspended == 0u)
            system_shutdown_key(&input, now, &shutdown_key_start, &shutdown_key_fired,
                                &lineage, &disk_power, &audio, power.flags);
        bool hold_changed = input.hold != previous_hold;
        previous_hold = input.hold;
        if (lifecycle.suspended == 0u &&
            (pressed & PJS_BUTTON_SELECT) != 0u &&
            !PJS_PRODUCTION &&
            (input.buttons & PJS_BUTTON_MENU) == 0u) {
            if (kernel_lcd_cycle()) {
                lcd_tested = true;
                shutdown_ready = false;
                pjs_core_set_kernel_diagnostic(4u, 0u);
            } else {
                pjs_core_set_kernel_diagnostic(8u, 20u);
            }
        } else if (lifecycle.suspended == 0u &&
            (pressed & PJS_BUTTON_PLAY) != 0u &&
            !PJS_PRODUCTION &&
            (input.buttons & PJS_BUTTON_MENU) == 0u) {
            uint32_t error = lcd_tested ? kernel_shutdown_preflight(
                runtime_active,
                lineage_ready &&
                    lineage.record.phase == PJS_LINEAGE_PHASE_RUNNING,
                filtered_power) : 4u;
            if (error == 0u) {
                shutdown_ready = true;
                pjs_core_set_kernel_diagnostic(5u, 0u);
            } else {
                shutdown_ready = false;
                pjs_core_set_kernel_diagnostic(8u, error);
            }
        }
        pending_wheel_delta = clamp_wheel_delta(
            pending_wheel_delta + (int32_t)input.wheel_delta);
        now = timer_now_us();

        if ((int32_t)(now - next_source_sample) >= 0) {
            PjsPowerSourceSample sample = {0};
            power_source_sample_read_only(&sample);
            bool fresh_battery_sample = power.samples != last_battery_sample;
            if (fresh_battery_sample) last_battery_sample = power.samples;
            uint32_t source_transitions_before = lifecycle.source_transitions;
            uint32_t lifecycle_flags = power_lifecycle_update(
                &lifecycle, power.battery_mv,
                fresh_battery_sample &&
                    (power.flags & PJS_POWER_ADC_VALID) != 0u,
                sample.flags, sample.valid_mask);
            uint32_t preserved_flags = power.flags &
                (PJS_POWER_ADC_VALID | PJS_POWER_I2C_FAULT |
                 PJS_POWER_ADC_RANGE_FAULT | PJS_POWER_TELEMETRY_DISABLED |
                 PJS_POWER_CHARGER_LIMITED);
            power.flags = preserved_flags | lifecycle_flags;
            lifecycle_source_changed =
                lifecycle.source_transitions != source_transitions_before;
            if (power_i2c_recovery_count() != last_i2c_recovery_count) {
                power.flags |= PJS_POWER_I2C_RECOVERED;
                last_i2c_recovery_count = power_i2c_recovery_count();
            }
            filtered_power = (uint8_t)(power.flags & 0xffu);
            lifecycle_wake_events =
                power_lifecycle_take_wake_events(&lifecycle);
            bool lifecycle_diagnostic = false;
            if ((lifecycle_wake_events & PJS_POWER_WAKE_USB) != 0u) {
                pjs_core_set_kernel_diagnostic(10u, 0u);
                last_power_mode = UINT32_MAX;
                lifecycle_diagnostic = true;
            } else if ((lifecycle_wake_events & PJS_POWER_WAKE_FIREWIRE) != 0u) {
                pjs_core_set_kernel_diagnostic(11u, 0u);
                last_power_mode = UINT32_MAX;
                lifecycle_diagnostic = true;
            } else if ((power.flags & PJS_POWER_CHARGE_ONLY) != 0u) {
                pjs_core_set_kernel_diagnostic(15u, 0u);
                lifecycle_diagnostic = true;
            } else if ((power.flags & PJS_POWER_BATTERY_CRITICAL) != 0u) {
                pjs_core_set_kernel_diagnostic(17u, 0u);
                lifecycle_diagnostic = true;
            } else if ((power.flags & PJS_POWER_BATTERY_LOW) != 0u) {
                pjs_core_set_kernel_diagnostic(16u, 0u);
                lifecycle_diagnostic = true;
            }
            if ((filtered_power & PJS_POWER_SOURCE_UNSTABLE) == 0u) {
                lifecycle.charger_mode = power_charger_mode();
                if (lifecycle.charger_mode == PJS_POWER_CHARGER_USB_100MA)
                    power.flags |= PJS_POWER_CHARGER_LIMITED;
                else
                    power.flags &= ~PJS_POWER_CHARGER_LIMITED;
                uint32_t mode = kernel_power_mode(filtered_power);
                if (mode != last_power_mode) {
                    last_power_mode = mode;
                    if (!lifecycle_diagnostic) {
                        pjs_core_set_kernel_diagnostic(mode, 0u);
                    }
                }
            }
            next_source_sample = now + 100000u;
        }

        if ((int32_t)(now - next_memory_check) >= 0) {
            if (!memory_integrity_ok()) panic_code(0x4d454d47u); /* MEMG */
            next_memory_check = now + 1000000u;
        }

        if (hold_changed || buttons_changed || input.wheel_delta != 0 ||
            lifecycle_source_changed || lifecycle_wake_events != 0u) {
            last_user_activity = now;
        }
        /* Reusable paused/ended handles do not keep the system awake. */
        if (pjs_audio_pcm_busy()) last_user_activity = now;

        /* A suspended runtime still polls input and source pins, but does not
         * execute guest frames. Any new button edge is the wake request. */
        if (lifecycle.suspended != 0u) {
            if (pressed != 0u || lifecycle_wake_events != 0u) {
                if (kernel_resume(&lifecycle, &disk_power)) {
                    lcd_tested = true;
                    shutdown_ready = false;
                    pjs_core_set_kernel_diagnostic(14u, 0u);
                    last_user_activity = now;
                } else {
                    pjs_core_set_kernel_diagnostic(8u, 70u);
                }
            }
        } else {
            bool suspend_chord = (input.buttons &
                (PJS_BUTTON_MENU | PJS_BUTTON_RIGHT)) ==
                (PJS_BUTTON_MENU | PJS_BUTTON_RIGHT);
            bool restart_chord = (input.buttons &
                (PJS_BUTTON_MENU | PJS_BUTTON_LEFT)) ==
                (PJS_BUTTON_MENU | PJS_BUTTON_LEFT);
            if (!suspend_chord) {
                suspend_chord_start = 0u;
                suspend_chord_fired = false;
            } else if (suspend_chord_start == 0u) {
                suspend_chord_start = now;
            } else if (!suspend_chord_fired &&
                       (uint32_t)(now - suspend_chord_start) >=
                           PJS_POWER_CHORD_HOLD_US) {
                suspend_chord_fired = true;
                bool battery_only = (filtered_power &
                    (PJS_POWER_EXTERNAL_MASK | PJS_POWER_SOURCE_UNSTABLE)) == 0u;
                if (!battery_only) {
                    /* Menu+Right is a battery-only lifecycle probe. Avoid
                     * dropping the disk rail while externally powered. */
                    pjs_core_set_kernel_diagnostic(21u, 0u);
                } else {
                    pjs_core_set_kernel_diagnostic(18u, 0u);
                    (void)render_and_present();
                    if (kernel_suspend(&lifecycle, &disk_power, &audio)) {
                        pjs_core_set_kernel_diagnostic(13u, 0u);
                    } else {
                        pjs_core_set_kernel_diagnostic(8u, 71u);
                    }
                }
            }

            if (!restart_chord) {
                restart_chord_start = 0u;
                restart_chord_fired = false;
            } else if (restart_chord_start == 0u) {
                restart_chord_start = now;
            } else if (!restart_chord_fired &&
                       (uint32_t)(now - restart_chord_start) >=
                           PJS_POWER_CHORD_HOLD_US) {
                restart_chord_fired = true;
                pjs_core_set_kernel_diagnostic(18u, 0u);
                (void)render_and_present();
                /* Ordinary apps return to Apps without resetting USB or the
                 * disk rail. A maintenance guest still uses the explicit
                 * reboot path below because the protocol owns its runtime. */
                if (
                    !usb_takeover &&
                    runtime_active) {
                    /* An acknowledged host ownership request takes priority
                     * over a simultaneous physical app-return gesture. */
                    if (usb_protocol.maintenance_request) continue;
                    bool return_ready = true;
                    return_ready = pjs_audio_pcm_reset() == 0;
                    if (return_ready)
                        return_ready = kernel_commit_clean_lineage(&lineage);
                    if (return_ready)
                        return_ready = pjs_storage_disk_flush(&disk_power) ==
                            PJS_STORAGE_OK;
                    if (!return_ready) {
                        ++batch_diagnostics.return_failures;
                        pjs_core_set_kernel_diagnostic(8u, 72u);
                        continue;
                    }
                    kernel_stop_runtime(&disk_package, &runtime_active);
                    irq_disable_global();
                    timer_irq_stop();
                    pjs_core_shutdown();
                    usb_protocol.maintenance_available = false;
                    ++batch_diagnostics.warm_returns;
                    pjs_usb_protocol_publish_observation(&usb_protocol,
                        false, 0u, 0u, 0u, 0u);
                    goto boot_apps;
                }
                if (!kernel_shutdown_storage(&lineage, &disk_power, &audio)) {
                    pjs_core_set_kernel_diagnostic(8u, 72u);
                    restart_chord_start = now;
                } else {
                    kernel_stop_runtime(&disk_package, &runtime_active);
                    timer_irq_stop();
                    irq_disable_global();
                    if (usb_started) pjs_usb_device_shutdown();
                    backlight_suspend();
                    (void)lcd_sleep();
                    pp_reboot();
                }
            }

            if (automatic_shutdown_attempted == false &&
                power_battery_shutdown_due(
                    &lifecycle.battery, filtered_power,
                    (filtered_power & PJS_POWER_SOURCE_UNSTABLE) == 0u)) {
                automatic_shutdown_attempted = true;
                pjs_core_set_kernel_diagnostic(19u, 0u);
                (void)render_and_present();
                if (kernel_shutdown_storage(&lineage, &disk_power, &audio)) {
                    kernel_stop_runtime(&disk_package, &runtime_active);
                    timer_irq_stop();
                    irq_disable_global();
                    if (usb_started) pjs_usb_device_shutdown();
                    backlight_suspend();
                    (void)lcd_sleep();
                    (void)power_charger_set_mode(
                        PJS_POWER_CHARGER_SUSPEND);
                    int standby_result = power_request_standby(
                        PJS_POWER_WAKE_EXTERNAL);
                    if (standby_result != PJS_POWER_RESULT_OK) {
                        /* The final ATA/lineage sequence succeeded. Keep the
                         * unit inert if PMU standby cannot be written; a
                         * reset here would be an unsafe reboot loop. */
                        pjs_core_set_kernel_diagnostic(
                            8u, 80u + (uint32_t)(-standby_result));
                    }
                    for (;;) __asm__ volatile("nop");
                }
                pjs_core_set_kernel_diagnostic(8u, 75u);
            }

            if (suspend_chord_start == 0u && restart_chord_start == 0u &&
                (filtered_power &
                    (PJS_POWER_EXTERNAL_MASK | PJS_POWER_SOURCE_UNSTABLE)) == 0u &&
                (uint32_t)(now - last_user_activity) >=
                    PJS_POWER_IDLE_SLEEP_US
                && !pjs_audio_pcm_busy()
                ) {
                if (kernel_suspend(&lifecycle, &disk_power, &audio)) {
                    pjs_core_set_kernel_diagnostic(13u, 0u);
                    last_user_activity = now;
                } else {
                    /* Avoid retrying a failed ATA/LCD sequence on every loop;
                     * a new input edge or a later source transition retries it. */
                    last_user_activity = now;
                    pjs_core_set_kernel_diagnostic(8u, 76u);
                }
            }
        }


        if (lifecycle.suspended == 0u) {
            pjs_audio_stream_gate_tick(timer_now_us());
            uint32_t stream_mode, stream_error;
            if (pjs_audio_stream_gate_status(&stream_mode, &stream_error))
                pjs_core_set_kernel_diagnostic(stream_mode, stream_error);
        }

        uint32_t chord = PJS_BUTTON_MENU | PJS_BUTTON_PLAY;
        if ((input.buttons & chord) == chord) {
            if (exit_chord_start == 0u) exit_chord_start = now;
            if ((uint32_t)(now - exit_chord_start) >= 2000000u) {
                bool usb_ready = (filtered_power &
                    (PJS_POWER_USB | PJS_POWER_SOURCE_UNSTABLE)) == PJS_POWER_USB;
                uint32_t production_preflight = kernel_shutdown_preflight(
                    runtime_active,
                    lineage_ready &&
                        lineage.record.phase == PJS_LINEAGE_PHASE_RUNNING,
                    filtered_power);
                if (production_preflight != 0u) {
                    pjs_core_set_kernel_diagnostic(8u, production_preflight);
                    exit_chord_start = now;
                    continue;
                }
                shutdown_ready = true;
                if (!shutdown_ready || !usb_ready) {
                    pjs_core_set_kernel_diagnostic(
                        8u, shutdown_ready ? 31u : 30u);
                    exit_chord_start = now;
                    continue;
                }
                pjs_core_set_kernel_diagnostic(6u, 0u);
                (void)render_and_present();
                PjsStorageDiskHandoff handoff = {0};
                if (pjs_storage_prepare_disk_handoff(&handoff) !=
                        PJS_STORAGE_OK ||
                    !pjs_storage_disk_handoff_armed()) {
                    pjs_storage_disk_handoff_clear();
                    pjs_core_set_kernel_diagnostic(
                        8u, 40u + handoff.state);
                    exit_chord_start = now;
                    continue;
                }
                pjs_core_set_kernel_diagnostic(18u, 0u);
                (void)render_and_present();
                if (!kernel_shutdown_storage(&lineage, &disk_power, &audio)) {
                    pjs_storage_disk_handoff_clear();
                    pjs_core_set_kernel_diagnostic(
                        8u, 41u + disk_power.state);
                    exit_chord_start = now;
                    continue;
                }
                pjs_core_set_kernel_diagnostic(20u, 0u);
                (void)render_and_present();
                timer_delay_us(2000000u);
                kernel_stop_runtime(&disk_package, &runtime_active);
                timer_irq_stop();
                irq_disable_global();
                if (usb_started) pjs_usb_device_shutdown();
                backlight_enable(false);
                (void)lcd_sleep();
                pp_reboot_disk_mode();
            }
        } else {
            exit_chord_start = 0u;
        }

        if ((int32_t)(now - next_power_sample) >= 0) {
            power_telemetry_sample(&power);
            next_power_sample = now + 1000000u;
        }

        /* A successful suspend puts the panel to sleep. Keep input, source,
         * and battery sampling alive, but do not step the guest or transfer a
         * dirty diagnostic frame until kernel_resume() wakes the panel. */
        if (lifecycle.suspended != 0u) {
            idle_until_service();
            continue;
        }

        /* An uncached guest can remain behind the timer indefinitely.
         * Give input and presentation a turn after each simulation step. */
        uint32_t steps = scheduler_take_fixed_batch(1u);
        if (steps != 0u) {
            if (lifecycle.suspended == 0u) {
            PjsScheduler scheduler;
            scheduler_snapshot(&scheduler);
            PjsCoreInput frame_input = core_input(&input, &power, &scheduler,
                                                  pending_wheel_delta,
                                                  cache_enabled,
                                                  last_frame_us,
                                                  runtime_status);
            for (uint32_t step = 0u; step < steps; ++step) {
                uint32_t frame_started = timer_now_us();
                /* Catch-up batches may contain 32 guest frames. Audio must
                 * not wait for the entire batch to finish. */
                pjs_audio_stream_gate_refill();
                if (usb_takeover) {
                    if (runtime_active &&
                        !pjs_usb_protocol_runtime_frame(&usb_protocol,
                                                        &frame_input)) {
                        runtime_active = false;
                        runtime_status = PJS_RUNTIME_ERROR;
                        frame_input.runtime_status = runtime_status;
                    }
                } else
                if (runtime_active && !qjs_runtime_frame(&frame_input)) {
                    if (pjs_audio_stream_gate_active())
                        (void)pjs_audio_stream_gate_cancel();
                    boot_failure_stage = PJS_BOOT_FAILURE_FRAME;
                    boot_failure_code = qjs_runtime_error_code();
                    qjs_runtime_shutdown();
                    runtime_active = false;
                    runtime_status = runtime_failure_status();
                    frame_input.runtime_status = runtime_status;
                    if (lineage_ready) {
                        PjsLineageRecord crashed = lineage.record;
                        crashed.phase = PJS_LINEAGE_PHASE_CRASHED;
                        crashed.rejected_source = crashed.active_source;
                        crashed.rejected_hash_low = crashed.active_hash_low;
                        crashed.rejected_hash_high = crashed.active_hash_high;
                        crashed.failure_stage = boot_failure_stage;
                        crashed.failure_code = boot_failure_code;
                        if (pjs_storage_lineage_write(
                                &lineage, &crashed) == PJS_STORAGE_OK) {
                            pjs_core_set_lineage_diagnostic(
                                3u, crashed.active_source,
                                lineage.record.generation, 0u);
                        } else {
                            pjs_core_set_lineage_diagnostic(
                                6u, crashed.active_source,
                                lineage.record.generation, lineage.error);
                        }
                    }
                    /* A resident app fault must leave the user at Apps.  The
                     * embedded package is a last-resort recovery guest, but
                     * it is not an app selector and therefore strands a
                     * healthy installation behind the diagnostic surface.
                     * Close the guest store and restart the normal launcher
                     * loop so the user can choose another package. */
                    if (!usb_takeover) {
                        pjs_storage_release(&disk_package);
                        irq_disable_global();
                        timer_irq_stop();
                        pjs_core_shutdown();
                        usb_protocol.maintenance_available = false;
                        pjs_usb_protocol_publish_observation(&usb_protocol,
                            false, 0u, boot_failure_code, 0u, 0u);
                        goto boot_apps;
                    }
                    if (!embedded_recovery_attempted) {
                        embedded_recovery_attempted = true;
                        pjs_storage_release(&disk_package);
                        if (boot_embedded_recovery(&guest, &frame_input)) {
                            runtime_active = true;
                            runtime_from_disk = false;
                            runtime_status = PJS_RUNTIME_READY;
                            boot_source = PJS_BOOT_SOURCE_EMBEDDED;
                            frame_input.runtime_status = runtime_status;
                            pjs_core_set_lineage_diagnostic(
                                4u, PJS_BOOT_SOURCE_EMBEDDED,
                                lineage.record.generation, 0u);
                        } else {
                            pjs_core_set_lineage_diagnostic(
                                6u, PJS_BOOT_SOURCE_EMBEDDED,
                                lineage.record.generation,
                                qjs_runtime_error_code());
                        }
                    }
                }
                if (pjs_core_step(&frame_input) < 0) {
                    panic_code(0x50314332u); /* P1C2 */
                }
                pjs_audio_stream_gate_refill();
                /* Wheel motion is an edge-like event. Apply the accumulated
                 * delta once, never once per catch-up simulation step. */
                frame_input.wheel_delta = 0;
                pending_wheel_delta = 0;
                if (runtime_active) ++observed_frames;
                observed_performance.last_frame_us = timer_now_us() - frame_started;
                if (observed_performance.last_frame_us > observed_performance.max_frame_us)
                    observed_performance.max_frame_us = observed_performance.last_frame_us;
            }

            /* Drain bounded catch-up work before starting another expensive
             * render. Input is polled again at the top of every batch. */
            scheduler_snapshot(&scheduler);
            } else {
                /* Keep the scheduler bounded while the guest and disk are
                 * suspended; no deferred wheel motion is replayed on wake. */
                pending_wheel_delta = 0;
            }
        }

        now = timer_now_us();
        if ((pjs_core_needs_render() == 0u && status_hold == presented_hold) ||
            (int32_t)(now - next_present) < 0) {
            if (steps == 0u) idle_until_service();
            continue;
        }

        next_present = now + PJS_RENDER_GAP_US;
        last_frame_us = render_and_present();
    }
}
