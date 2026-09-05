#include "power.h"

#include "pp5020.h"
#include "timer.h"

#define PCF50605_ADDRESS 0x08u
#define PCF50605_ADCC1 0x2fu
#define PCF50605_ADCS1 0x30u
#define PCF50605_BATTERY_CHANNEL 0x02u
#define PCF50605_OOCC1 0x08u
#define PCF50605_GOSTDBY 0x01u
#define PCF50605_CHGWAK 0x20u
#define PCF50605_EXTONWAK 0x40u
#define I2C_SEND 0x80u
#define I2C_READ 0x20u
#define I2C_BUSY 0x40u
#define I2C_TRIES 3u
#define I2C_WAIT_US 5000u
#define BATTERY_MIN_MV 2500u
#define BATTERY_MAX_MV 5000u

/* The LTC4066 charger is controlled by two PP5020 GPO32 pins on Color/Photo:
 * SUSP is active high and HPWR selects the 100/500 mA USB limit. */
#define CHARGER_SUSPEND_MASK 0x08000000u
#define CHARGER_FAST_MASK 0x00000040u

static uint32_t i2c_recoveries;
static uint8_t charger_mode = PJS_POWER_CHARGER_SUSPEND;
static uint8_t standby_sources;

static void recover_bus(void);

static bool wait_idle(void)
{
    uint32_t started = timer_now_us();
    while ((PP_I2C_STATUS & I2C_BUSY) != 0u) {
        if ((uint32_t)(timer_now_us() - started) >= I2C_WAIT_US) return false;
    }
    return true;
}

static void recover_bus(void)
{
    PP_DEV_EN |= PP_DEV_I2C;
    PP_DEV_RS |= PP_DEV_I2C;
    pp_nop3();
    PP_DEV_RS &= ~PP_DEV_I2C;
    /* Re-apply the known PP5020 clock after a reset. A stale inherited clock
     * must never turn an I2C timeout into an unbounded retry loop. */
    PP_I2C_CLOCK = 0u;
    PP_I2C_CLOCK = 0x80u;
    if (i2c_recoveries != UINT32_MAX) ++i2c_recoveries;
}

static bool send_bytes(uint8_t address, const uint8_t *data, uint32_t length)
{
    if (data == 0 || length == 0u || length > 4u || !wait_idle()) return false;
    PP_I2C_ADDR = (uint8_t)(address << 1);
    PP_I2C_CTRL &= (uint8_t)~I2C_READ;
    for (uint32_t index = 0u; index < length; ++index) PP_I2C_DATA(index) = data[index];
    PP_I2C_CTRL = (uint8_t)((PP_I2C_CTRL & (uint8_t)~0x06u) | ((length - 1u) << 1));
    PP_I2C_CTRL |= I2C_SEND;
    return wait_idle();
}

static bool read_bytes(uint8_t address, uint8_t *data, uint32_t length)
{
    if (data == 0 || length == 0u || length > 4u || !wait_idle()) return false;
    PP_I2C_ADDR = (uint8_t)((address << 1) | 1u);
    PP_I2C_CTRL |= I2C_READ;
    PP_I2C_CTRL = (uint8_t)((PP_I2C_CTRL & (uint8_t)~0x06u) | ((length - 1u) << 1));
    PP_I2C_CTRL |= I2C_SEND;
    if (!wait_idle()) return false;
    for (uint32_t index = 0u; index < length; ++index) data[index] = PP_I2C_DATA(index);
    return true;
}

static bool pcf_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    for (uint32_t attempt = 0u; attempt < I2C_TRIES; ++attempt) {
        if (send_bytes(PCF50605_ADDRESS, data, 2u)) return true;
        recover_bus();
    }
    return false;
}

static bool pcf_read_multiple(uint8_t reg, uint8_t *data, uint32_t length)
{
    for (uint32_t attempt = 0u; attempt < I2C_TRIES; ++attempt) {
        if (send_bytes(PCF50605_ADDRESS, &reg, 1u) &&
            read_bytes(PCF50605_ADDRESS, data, length)) return true;
        recover_bus();
    }
    return false;
}

uint16_t power_battery_mv_from_raw(uint16_t raw)
{
    if (raw > 1023u) raw = 1023u;
    return (uint16_t)(((uint32_t)raw * 6000u) >> 10);
}

static const uint16_t photo_discharge_curve[11] = {
    3450u, 3660u, 3700u, 3730u, 3770u, 3820u,
    3870u, 3920u, 4040u, 4100u, 4170u,
};

uint8_t power_battery_percent_from_mv(uint16_t mv, bool charging)
{
    /* The Photo's charge curve is the same conservative table for this
     * campaign. Keep the argument in the API because a later calibrated
     * pack may provide distinct charge/discharge points. */
    (void)charging;
    if (mv <= photo_discharge_curve[0]) return 0u;
    if (mv >= photo_discharge_curve[10]) return 100u;
    for (uint32_t index = 1u; index < 11u; ++index) {
        uint16_t high = photo_discharge_curve[index];
        if (mv > high) continue;
        uint16_t low = photo_discharge_curve[index - 1u];
        uint16_t span = (uint16_t)(high - low);
        uint16_t offset = (uint16_t)(mv - low);
        uint32_t tenths = ((uint32_t)offset * 10u + span / 2u) / span;
        uint32_t percent = (index - 1u) * 10u + tenths;
        return (uint8_t)(percent > 100u ? 100u : percent);
    }
    return 100u;
}

void power_battery_debounce_init(PjsPowerBatteryDebounce *battery)
{
    if (battery == 0) return;
    *battery = (PjsPowerBatteryDebounce){
        .state = PJS_POWER_BATTERY_UNKNOWN,
    };
}

uint8_t power_battery_debounce_update(PjsPowerBatteryDebounce *battery,
                                      uint16_t mv, bool valid)
{
    if (battery == 0) return PJS_POWER_BATTERY_UNKNOWN;
    if (battery->samples != UINT32_MAX) ++battery->samples;
    if (!valid) return battery->state;

    battery->last_mv = mv;
    if (mv <= PJS_POWER_BATTERY_SHUTOFF_MV) {
        battery->low_samples = 0u;
        if (battery->critical_samples != 0xffu) ++battery->critical_samples;
        battery->good_samples = 0u;
        if (battery->critical_samples >= PJS_POWER_BATTERY_CRITICAL_SAMPLES) {
            battery->state = PJS_POWER_BATTERY_CRITICAL_STATE;
        }
        return battery->state;
    }

    battery->critical_samples = 0u;
    if (mv <= PJS_POWER_BATTERY_LOW_MV) {
        if (battery->low_samples != 0xffu) ++battery->low_samples;
        battery->good_samples = 0u;
        if (battery->low_samples >= PJS_POWER_BATTERY_LOW_SAMPLES &&
            battery->state != PJS_POWER_BATTERY_CRITICAL_STATE) {
            battery->state = PJS_POWER_BATTERY_LOW_STATE;
        }
        return battery->state;
    }

    battery->low_samples = 0u;
    if (mv >= PJS_POWER_BATTERY_RECOVER_MV) {
        if (battery->good_samples != 0xffu) ++battery->good_samples;
        if (battery->good_samples >= PJS_POWER_BATTERY_GOOD_SAMPLES) {
            battery->state = PJS_POWER_BATTERY_NORMAL;
        }
    } else {
        battery->good_samples = 0u;
    }
    return battery->state;
}

bool power_battery_shutdown_due(const PjsPowerBatteryDebounce *battery,
                                uint8_t stable_source,
                                bool source_stable)
{
    if (battery == 0 || !source_stable ||
        battery->state != PJS_POWER_BATTERY_CRITICAL_STATE) return false;
    return (stable_source & PJS_POWER_SOURCE_MASK) == 0u &&
           battery->critical_samples >= PJS_POWER_BATTERY_CRITICAL_SAMPLES;
}

void power_lifecycle_init(PjsPowerLifecycle *lifecycle)
{
    if (lifecycle == 0) return;
    *lifecycle = (PjsPowerLifecycle){0};
    power_source_filter_init(&lifecycle->source);
    power_battery_debounce_init(&lifecycle->battery);
    lifecycle->charger_mode = PJS_POWER_CHARGER_SUSPEND;
}

uint32_t power_lifecycle_update(PjsPowerLifecycle *lifecycle,
                                uint16_t battery_mv, bool battery_valid,
                                uint8_t raw_source_flags,
                                uint8_t source_valid_mask)
{
    if (lifecycle == 0) {
        return PJS_POWER_SOURCE_UNSTABLE;
    }
    if (lifecycle->samples != UINT32_MAX) ++lifecycle->samples;
    uint8_t filtered = power_source_filter_update(
        &lifecycle->source, raw_source_flags, source_valid_mask);
    bool source_stable = (filtered & PJS_POWER_SOURCE_UNSTABLE) == 0u;
    uint8_t source = (uint8_t)(filtered & PJS_POWER_SOURCE_MASK);
    uint8_t previous_source = lifecycle->stable_source;
    if (source_stable && source != previous_source) {
        if (lifecycle->source_transitions != UINT32_MAX) {
            ++lifecycle->source_transitions;
        }
        if (previous_source == 0u && source != 0u) {
            if ((source & PJS_POWER_USB) != 0u) {
                lifecycle->wake_events |= PJS_POWER_WAKE_USB;
                lifecycle->wake_seen |= PJS_POWER_WAKE_USB;
                if (lifecycle->wake_usb_count != UINT32_MAX) {
                    ++lifecycle->wake_usb_count;
                }
            }
            if ((source & PJS_POWER_FIREWIRE) != 0u) {
                lifecycle->wake_events |= PJS_POWER_WAKE_FIREWIRE;
                lifecycle->wake_seen |= PJS_POWER_WAKE_FIREWIRE;
                if (lifecycle->wake_firewire_count != UINT32_MAX) {
                    ++lifecycle->wake_firewire_count;
                }
            }
            if (lifecycle->wake_count != UINT32_MAX) ++lifecycle->wake_count;
        }
        lifecycle->stable_source = source;
    }

    uint8_t battery_state = power_battery_debounce_update(
        &lifecycle->battery, battery_mv, battery_valid);
    uint32_t flags = filtered;
    if (battery_valid) flags |= PJS_POWER_ADC_VALID;
    if (battery_state == PJS_POWER_BATTERY_LOW_STATE) {
        flags |= PJS_POWER_BATTERY_LOW;
    } else if (battery_state == PJS_POWER_BATTERY_CRITICAL_STATE) {
        flags |= PJS_POWER_BATTERY_CRITICAL | PJS_POWER_BATTERY_LOW;
    }
    if (source_stable && source != 0u &&
        (battery_state == PJS_POWER_BATTERY_LOW_STATE ||
         battery_state == PJS_POWER_BATTERY_CRITICAL_STATE)) {
        /* External power keeps a low battery in charge-only policy; it must
         * never be mistaken for permission to run the automatic shutoff. */
        flags |= PJS_POWER_CHARGE_ONLY;
    }
    if (lifecycle->suspended != 0u) flags |= PJS_POWER_SUSPENDED;
    if ((lifecycle->wake_seen & PJS_POWER_WAKE_USB) != 0u) {
        flags |= PJS_POWER_WAKE_USB_SEEN;
    }
    if ((lifecycle->wake_seen & PJS_POWER_WAKE_FIREWIRE) != 0u) {
        flags |= PJS_POWER_WAKE_FIREWIRE_SEEN;
    }
    return flags;
}

uint8_t power_lifecycle_take_wake_events(PjsPowerLifecycle *lifecycle)
{
    if (lifecycle == 0) return 0u;
    uint8_t events = lifecycle->wake_events;
    lifecycle->wake_events = 0u;
    return events;
}

void power_lifecycle_set_suspended(PjsPowerLifecycle *lifecycle,
                                   bool suspended)
{
    if (lifecycle == 0) return;
    lifecycle->suspended = suspended ? 1u : 0u;
}

uint8_t power_lifecycle_battery_state(const PjsPowerLifecycle *lifecycle)
{
    return lifecycle == 0 ? PJS_POWER_BATTERY_UNKNOWN : lifecycle->battery.state;
}

uint8_t power_lifecycle_source(const PjsPowerLifecycle *lifecycle)
{
    return lifecycle == 0 ? 0u : lifecycle->stable_source;
}

uint32_t power_lifecycle_wake_count(const PjsPowerLifecycle *lifecycle,
                                    uint8_t wake_source)
{
    if (lifecycle == 0) return 0u;
    if (wake_source == PJS_POWER_WAKE_USB) return lifecycle->wake_usb_count;
    if (wake_source == PJS_POWER_WAKE_FIREWIRE) {
        return lifecycle->wake_firewire_count;
    }
    return lifecycle->wake_count;
}

uint8_t power_decode_source_flags(uint32_t gpioc, uint32_t gpiod,
                                  uint32_t gpo32_input)
{
    uint8_t flags = 0u;
    if ((gpioc & 0x04u) == 0u) flags |= PJS_POWER_FIREWIRE;
    if ((gpiod & 0x08u) != 0u) flags |= PJS_POWER_USB;
    if ((gpo32_input & 0x01u) == 0u) flags |= PJS_POWER_CHARGING;
    return flags;
}

uint8_t power_source_flags_read(void)
{
    uint8_t flags = 0u;
    if ((PP_GPIOC_INPUT_VAL & 0x04u) == 0u) flags |= PJS_POWER_FIREWIRE;
    if ((PP_GPIOD_INPUT_VAL & 0x08u) != 0u) flags |= PJS_POWER_USB;
    return flags;
}

void power_source_sample_read_only(PjsPowerSourceSample *sample)
{
    if (sample == 0) return;

    uint32_t gpioc = PP_GPIOC_INPUT_VAL;
    uint32_t gpiod = PP_GPIOD_INPUT_VAL;
    uint32_t gpo32_input = PP_GPO32_INPUT_VAL;
    uint8_t valid = (uint8_t)(PJS_POWER_FIREWIRE | PJS_POWER_USB);
    /* Do not claim a charging state when the inherited firmware left the
     * LTC4066 indication pin unconfigured. This read is observational; the
     * read-only campaign must not enable the pin as a side effect. */
    if ((PP_GPO32_INPUT_ENABLE & 0x01u) != 0u) {
        valid = (uint8_t)(valid | PJS_POWER_CHARGING);
    }
    sample->flags = (uint8_t)(power_decode_source_flags(
        gpioc, gpiod, gpo32_input) & valid);
    sample->valid_mask = valid;
}

uint8_t power_source_flags_read_only(void)
{
    PjsPowerSourceSample sample = {0};
    power_source_sample_read_only(&sample);
    if (sample.valid_mask != PJS_POWER_SOURCE_MASK) {
        return (uint8_t)(sample.flags | PJS_POWER_SOURCE_UNSTABLE);
    }
    return sample.flags;
}

void power_source_filter_init(PjsPowerSourceFilter *filter)
{
    if (filter == 0) return;
    *filter = (PjsPowerSourceFilter){0};
}

uint8_t power_source_filter_update(PjsPowerSourceFilter *filter,
                                   uint8_t raw_flags, uint8_t valid_mask)
{
    if (filter == 0) return PJS_POWER_SOURCE_UNSTABLE;

    const uint8_t source_mask = (uint8_t)PJS_POWER_SOURCE_MASK;
    uint8_t valid = (uint8_t)(valid_mask & source_mask);
    uint8_t sample = (uint8_t)(raw_flags & valid);
    if (filter->samples != UINT32_MAX) ++filter->samples;

    /* A source that becomes unobservable must not remain latched as present.
     * Keep the other pins' qualified values while reporting uncertainty. */
    filter->stable_flags &= valid;
    if (filter->valid_mask != valid) {
        filter->valid_mask = valid;
        filter->candidate_flags = sample;
        filter->candidate_samples = 1u;
        filter->qualified = 0u;
    } else if (filter->qualified != 0u &&
               (filter->stable_flags & valid) == sample) {
        filter->candidate_flags = sample;
        filter->candidate_samples = 0u;
    } else {
        if (filter->candidate_flags != sample ||
            filter->candidate_samples == 0u) {
            filter->candidate_flags = sample;
            filter->candidate_samples = 1u;
        } else if (filter->candidate_samples < 0xffu) {
            ++filter->candidate_samples;
        }
        if (filter->candidate_samples >= PJS_POWER_SOURCE_FILTER_SAMPLES) {
            if ((filter->stable_flags & valid) != sample &&
                filter->transitions != UINT32_MAX) {
                ++filter->transitions;
            }
            filter->stable_flags = (uint8_t)(
                (filter->stable_flags & (uint8_t)~valid) | sample);
            filter->candidate_samples = 0u;
            filter->qualified = 1u;
        }
    }

    uint8_t result = (uint8_t)(filter->stable_flags & source_mask);
    if (valid != source_mask || filter->qualified == 0u ||
        filter->candidate_samples != 0u) {
        result = (uint8_t)(result | PJS_POWER_SOURCE_UNSTABLE);
    }
    return result;
}

void power_telemetry_init(void)
{
    recover_bus();
    /* PP5020 iPod targets select the 24 MHz source with the controller's
     * standard divider value. Resetting to zero first mirrors the proven
     * controller initialization sequence and avoids inheriting a loader's
     * stale clock selection. */
    PP_I2C_CLOCK = 0u;
    PP_I2C_CLOCK = 0x80u;
    /* Make GPO32 bit 0 observable as the LTC4066 charging indication. */
    PP_GPO32_INPUT_ENABLE |= 0x01u;
    PP_GPO32_ENABLE &= ~0x01u;
}

void power_telemetry_sample(PjsPowerTelemetry *telemetry)
{
    if (telemetry == 0) return;
    uint8_t flags = power_decode_source_flags(PP_GPIOC_INPUT_VAL, PP_GPIOD_INPUT_VAL,
                                              PP_GPO32_INPUT_VAL);
    uint8_t adc[2] = {0u, 0u};
    bool ok = pcf_write(PCF50605_ADCC1,
                        (uint8_t)((PCF50605_BATTERY_CHANNEL << 1) | 1u)) &&
              pcf_read_multiple(PCF50605_ADCS1, adc, 2u);
    if (ok) {
        uint16_t raw = (uint16_t)(((uint16_t)adc[0] << 2) | (adc[1] & 0x03u));
        uint16_t mv = power_battery_mv_from_raw(raw);
        telemetry->battery_raw = raw;
        if (mv >= BATTERY_MIN_MV && mv <= BATTERY_MAX_MV) {
            telemetry->battery_mv = mv;
            telemetry->consecutive_failures = 0u;
            flags |= PJS_POWER_ADC_VALID;
        } else {
            if (telemetry->consecutive_failures != 0xffu) ++telemetry->consecutive_failures;
            flags |= PJS_POWER_ADC_RANGE_FAULT;
        }
    } else {
        if (telemetry->consecutive_failures != 0xffu) ++telemetry->consecutive_failures;
        flags |= PJS_POWER_I2C_FAULT;
    }
    telemetry->flags = flags;
    ++telemetry->samples;
}

int power_charger_set_mode(uint8_t mode)
{
    if (mode > PJS_POWER_CHARGER_USB_500MA) {
        return PJS_POWER_RESULT_ARGUMENT;
    }

    PP_GPO32_ENABLE |= CHARGER_SUSPEND_MASK | CHARGER_FAST_MASK;
    if (mode == PJS_POWER_CHARGER_SUSPEND) {
        PP_GPO32_VAL |= CHARGER_SUSPEND_MASK;
    } else {
        PP_GPO32_VAL &= ~CHARGER_SUSPEND_MASK;
    }
    if (mode == PJS_POWER_CHARGER_USB_500MA) {
        PP_GPO32_VAL |= CHARGER_FAST_MASK;
    } else {
        PP_GPO32_VAL &= ~CHARGER_FAST_MASK;
    }
    charger_mode = mode;
    return PJS_POWER_RESULT_OK;
}

uint8_t power_charger_mode(void)
{
    return charger_mode;
}

int power_request_standby(uint8_t wake_sources)
{
    uint8_t known = (uint8_t)PJS_POWER_WAKE_EXTERNAL;
    if ((wake_sources & known) == 0u || (wake_sources & (uint8_t)~known) != 0u) {
        return PJS_POWER_RESULT_ARGUMENT;
    }

    uint8_t value = PCF50605_GOSTDBY;
    if ((wake_sources & PJS_POWER_WAKE_USB) != 0u) {
        value = (uint8_t)(value | PCF50605_CHGWAK);
    }
    if ((wake_sources & PJS_POWER_WAKE_FIREWIRE) != 0u) {
        value = (uint8_t)(value | PCF50605_EXTONWAK);
    }
    if (!pcf_write(PCF50605_OOCC1, value)) {
        return PJS_POWER_RESULT_I2C;
    }
    standby_sources = wake_sources;
    return PJS_POWER_RESULT_OK;
}

uint8_t power_last_standby_sources(void)
{
    return standby_sources;
}

uint32_t power_i2c_recovery_count(void)
{
    return i2c_recoveries;
}
