#ifndef POCKETJS_IPOD_PHOTO_POWER_H
#define POCKETJS_IPOD_PHOTO_POWER_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PJS_POWER_FIREWIRE = 1u << 0,
    PJS_POWER_USB = 1u << 1,
    PJS_POWER_CHARGING = 1u << 2,
    PJS_POWER_ADC_VALID = 1u << 3,
    PJS_POWER_I2C_FAULT = 1u << 4,
    PJS_POWER_TELEMETRY_DISABLED = 1u << 5,
    PJS_POWER_ADC_RANGE_FAULT = 1u << 6,
    /* Source pins have not produced the same value for the filter's
     * qualification window, or one of the source pins is not observable. */
    PJS_POWER_SOURCE_UNSTABLE = 1u << 7,
    /* Battery and lifecycle state. These bits intentionally live above the
     * original telemetry bits so existing core consumers remain compatible. */
    PJS_POWER_BATTERY_LOW = 1u << 8,
    PJS_POWER_BATTERY_CRITICAL = 1u << 9,
    PJS_POWER_CHARGE_ONLY = 1u << 10,
    PJS_POWER_SUSPENDED = 1u << 11,
    PJS_POWER_WAKE_USB_SEEN = 1u << 12,
    PJS_POWER_WAKE_FIREWIRE_SEEN = 1u << 13,
    PJS_POWER_CHARGER_LIMITED = 1u << 14,
    PJS_POWER_I2C_RECOVERED = 1u << 15,
};

#define PJS_POWER_SOURCE_MASK \
    (PJS_POWER_FIREWIRE | PJS_POWER_USB | PJS_POWER_CHARGING)
#define PJS_POWER_EXTERNAL_MASK (PJS_POWER_FIREWIRE | PJS_POWER_USB)
#define PJS_POWER_SOURCE_FILTER_SAMPLES 3u

/* These are the measured iPod Photo discharge points used by Rockbox. They
 * are deliberately part of the target contract instead of an unqualified
 * percentage derived from a straight line. */
#define PJS_POWER_BATTERY_DISKSAFE_MV 3300u
#define PJS_POWER_BATTERY_SHUTOFF_MV 3300u
#define PJS_POWER_BATTERY_LOW_MV 3450u
#define PJS_POWER_BATTERY_RECOVER_MV 3550u
#define PJS_POWER_BATTERY_LOW_SAMPLES 3u
#define PJS_POWER_BATTERY_CRITICAL_SAMPLES 5u
#define PJS_POWER_BATTERY_GOOD_SAMPLES 3u

enum {
    PJS_POWER_BATTERY_UNKNOWN = 0u,
    PJS_POWER_BATTERY_NORMAL = 1u,
    PJS_POWER_BATTERY_LOW_STATE = 2u,
    PJS_POWER_BATTERY_CRITICAL_STATE = 3u,
};

enum {
    PJS_POWER_WAKE_USB = 1u << 0,
    PJS_POWER_WAKE_FIREWIRE = 1u << 1,
    PJS_POWER_WAKE_EXTERNAL = PJS_POWER_WAKE_USB | PJS_POWER_WAKE_FIREWIRE,
};

enum {
    PJS_POWER_CHARGER_SUSPEND = 0u,
    PJS_POWER_CHARGER_USB_100MA = 1u,
    PJS_POWER_CHARGER_USB_500MA = 2u,
};

enum {
    PJS_POWER_RESULT_OK = 0,
    PJS_POWER_RESULT_ARGUMENT = -1,
    PJS_POWER_RESULT_I2C = -2,
};

typedef struct {
    uint8_t flags;
    uint8_t valid_mask;
} PjsPowerSourceSample;

typedef struct {
    uint8_t stable_flags;
    uint8_t candidate_flags;
    uint8_t candidate_samples;
    uint8_t valid_mask;
    uint8_t qualified;
    uint32_t samples;
    uint32_t transitions;
} PjsPowerSourceFilter;

typedef struct {
    uint8_t state;
    uint8_t low_samples;
    uint8_t critical_samples;
    uint8_t good_samples;
    uint16_t last_mv;
    uint32_t samples;
} PjsPowerBatteryDebounce;

typedef struct {
    PjsPowerSourceFilter source;
    PjsPowerBatteryDebounce battery;
    uint8_t stable_source;
    uint8_t charger_mode;
    uint8_t wake_events;
    uint8_t wake_seen;
    uint8_t suspended;
    uint32_t wake_count;
    uint32_t wake_usb_count;
    uint32_t wake_firewire_count;
    uint32_t source_transitions;
    uint32_t samples;
} PjsPowerLifecycle;

typedef struct {
    uint16_t battery_raw;
    uint16_t battery_mv;
    uint32_t flags;
    uint8_t consecutive_failures;
    uint32_t samples;
} PjsPowerTelemetry;

uint8_t power_source_flags_read(void);
void power_telemetry_init(void);
void power_telemetry_sample(PjsPowerTelemetry *telemetry);
uint16_t power_battery_mv_from_raw(uint16_t raw);
uint8_t power_battery_percent_from_mv(uint16_t mv, bool charging);
uint8_t power_decode_source_flags(uint32_t gpioc, uint32_t gpiod,
                                  uint32_t gpo32_input);

void power_battery_debounce_init(PjsPowerBatteryDebounce *battery);
uint8_t power_battery_debounce_update(PjsPowerBatteryDebounce *battery,
                                      uint16_t mv, bool valid);
bool power_battery_shutdown_due(const PjsPowerBatteryDebounce *battery,
                                uint8_t stable_source,
                                bool source_stable);
void power_lifecycle_init(PjsPowerLifecycle *lifecycle);
uint32_t power_lifecycle_update(PjsPowerLifecycle *lifecycle,
                                uint16_t battery_mv, bool battery_valid,
                                uint8_t raw_source_flags,
                                uint8_t source_valid_mask);
uint8_t power_lifecycle_take_wake_events(PjsPowerLifecycle *lifecycle);
void power_lifecycle_set_suspended(PjsPowerLifecycle *lifecycle,
                                   bool suspended);
uint8_t power_lifecycle_battery_state(const PjsPowerLifecycle *lifecycle);
uint8_t power_lifecycle_source(const PjsPowerLifecycle *lifecycle);
/* Passing USB or FIREWIRE returns the per-source count. Any other source
 * value returns the total count. Counts saturate instead of wrapping. */
uint32_t power_lifecycle_wake_count(const PjsPowerLifecycle *lifecycle,
                                    uint8_t wake_source);

int power_charger_set_mode(uint8_t mode);
uint8_t power_charger_mode(void);
int power_request_standby(uint8_t wake_sources);
uint8_t power_last_standby_sources(void);
uint32_t power_i2c_recovery_count(void);

/* Read source pins without configuring a peripheral or writing a PMU,
 * charger, or power-rail register. The valid mask is deliberately separate
 * from flags: an inherited loader may not have enabled the LTC4066 input. */
void power_source_sample_read_only(PjsPowerSourceSample *sample);
uint8_t power_source_flags_read_only(void);

/* Debounce only source bits. The returned value contains the last qualified
 * source state and PJS_POWER_SOURCE_UNSTABLE until every source pin is both
 * observable and stable for PJS_POWER_SOURCE_FILTER_SAMPLES samples. */
void power_source_filter_init(PjsPowerSourceFilter *filter);
uint8_t power_source_filter_update(PjsPowerSourceFilter *filter,
                                   uint8_t raw_flags, uint8_t valid_mask);

#endif
