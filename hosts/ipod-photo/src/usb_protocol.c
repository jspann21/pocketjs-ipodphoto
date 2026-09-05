#include "usb_protocol.h"

#include "panic.h"

#include <stddef.h>

#include "native_loader.h"
#include "qjs_runtime.h"
#include "storage.h"
#include "usb_device.h"
#include "fs_store.h"

/* The protocol is deliberately freestanding.  These small helpers avoid a
 * dependency on a particular libc implementation in the phase-1 image. */
static void pjs_copy(uint8_t *dst, const uint8_t *src, uint32_t length)
{
    for (uint32_t i = 0u; i < length; ++i) dst[i] = src[i];
}

static void pjs_move(uint8_t *dst, const uint8_t *src, uint32_t length)
{
    if (dst == src || length == 0u) return;
    if (dst < src) {
        for (uint32_t i = 0u; i < length; ++i) dst[i] = src[i];
    } else {
        for (uint32_t i = length; i != 0u; --i) dst[i - 1u] = src[i - 1u];
    }
}

static uint32_t get_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t reset_core(void)
{
    pjs_core_shutdown();
    int32_t result = pjs_core_init();
    if (result == 0) return 0u;
    uint32_t magnitude = result < 0 ?
        (uint32_t)(-(int64_t)result) : (uint32_t)result;
    return 0x80000000u | (magnitude & 0x7fffffffu);
}

static void clear_runtime_error_text(PjsUsbProtocol *p)
{
    p->runtime_error_text_length = 0u;
    p->runtime_error_text[0] = 0u;
}

static void capture_runtime_error_text(PjsUsbProtocol *p)
{
    uint32_t length = 0u;
    const char *text = qjs_runtime_error_text(&length);
    if (length > PJS_USB_RUNTIME_ERROR_TEXT_MAX) {
        length = PJS_USB_RUNTIME_ERROR_TEXT_MAX;
    }
    if (text != 0 && length != 0u) {
        pjs_copy(p->runtime_error_text, (const uint8_t *)text, length);
    }
    p->runtime_error_text_length = length;
}

uint32_t pjs_usb_protocol_crc32(const uint8_t *bytes, uint32_t length)
{
    uint32_t crc = 0xffffffffu;
    if (bytes == 0 && length != 0u) return 0u;
    for (uint32_t i = 0u; i < length; ++i) {
        if ((i & 1023u) == 0u) pjs_usb_device_poll_cooperative();
        crc ^= bytes[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t frame_crc(const uint8_t *frame, uint32_t payload_length)
{
    uint32_t crc = 0xffffffffu;
    for (uint32_t i = 4u; i < 16u; ++i) {
        crc ^= frame[i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    for (uint32_t i = 0u; i < payload_length; ++i) {
        crc ^= frame[PJS_USB_PROTOCOL_HEADER_BYTES + i];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool transport_ready(const PjsUsbProtocol *p)
{
    return p != 0 && p->config.io.read != 0 && p->config.io.write != 0 &&
           p->config.rx_buffer != 0 && p->config.tx_buffer != 0 &&
           p->config.package_buffer != 0 &&
           p->config.rx_capacity >= PJS_USB_PROTOCOL_HEADER_BYTES &&
           p->config.tx_capacity >= PJS_USB_PROTOCOL_HEADER_BYTES;
}

bool pjs_usb_protocol_init(PjsUsbProtocol *p,
                           const PjsUsbProtocolConfig *config)
{
    if (p == 0 || config == 0 || config->io.read == 0 ||
        config->io.write == 0 || config->rx_buffer == 0 ||
        config->tx_buffer == 0 || config->package_buffer == 0 ||
        config->rx_capacity < PJS_USB_PROTOCOL_HEADER_BYTES ||
        config->tx_capacity < PJS_USB_PROTOCOL_HEADER_BYTES ||
        config->package_capacity == 0u) {
        return false;
    }
    *p = (PjsUsbProtocol){0};
    p->config = *config;
    if (p->config.max_payload == 0u) p->config.max_payload = 1024u;
    if (p->config.max_payload < 64u) return false;
    if (p->config.max_payload > PJS_USB_PROTOCOL_MAX_PAYLOAD) {
        p->config.max_payload = PJS_USB_PROTOCOL_MAX_PAYLOAD;
    }
    if (p->config.rx_capacity < PJS_USB_PROTOCOL_HEADER_BYTES +
                                  p->config.max_payload ||
        p->config.tx_capacity < PJS_USB_PROTOCOL_HEADER_BYTES +
                                  p->config.max_payload) {
        return false;
    }
    p->state = PJS_USB_STATE_IDLE;
    p->tx_sequence = 1u;
    p->commit_status = PJS_USB_COMMIT_STATUS_UNAVAILABLE;
    return true;
}

static bool queue_frame(PjsUsbProtocol *p, uint8_t type, uint32_t sequence,
                        const uint8_t *payload, uint32_t payload_length)
{
    if (!transport_ready(p) || p->tx_pending ||
        payload_length > p->config.max_payload || payload_length >
        p->config.tx_capacity - PJS_USB_PROTOCOL_HEADER_BYTES) {
        return false;
    }
    uint8_t *frame = p->config.tx_buffer;
    frame[0] = PJS_USB_PROTOCOL_MAGIC_0;
    frame[1] = PJS_USB_PROTOCOL_MAGIC_1;
    frame[2] = PJS_USB_PROTOCOL_MAGIC_2;
    frame[3] = PJS_USB_PROTOCOL_MAGIC_3;
    frame[4] = PJS_USB_PROTOCOL_VERSION;
    frame[5] = type;
    frame[6] = 0u;
    frame[7] = 0u;
    put_u32(frame + 8u, sequence);
    put_u32(frame + 12u, payload_length);
    if (payload_length != 0u) pjs_copy(frame + PJS_USB_PROTOCOL_HEADER_BYTES,
                                       payload, payload_length);
    put_u32(frame + 16u, frame_crc(frame, payload_length));
    p->tx_length = PJS_USB_PROTOCOL_HEADER_BYTES + payload_length;
    p->tx_offset = 0u;
    p->tx_pending = true;
    return true;
}

static bool queue_status(PjsUsbProtocol *p, uint8_t request_type,
                         uint32_t sequence, PjsUsbStatus status,
                         const uint8_t *extra, uint32_t extra_length)
{
    /* INFO carries retained-crash, storage, and bounded QuickJS diagnostics.
     * Leave four leading bytes for status. */
    uint8_t payload[484];
    if (extra_length > sizeof(payload) - 4u) return false;
    put_u32(payload, (uint32_t)status);
    if (extra_length != 0u) pjs_copy(payload + 4u, extra, extra_length);
    uint8_t response_type;
    switch (request_type) {
    case PJS_USB_MSG_HELLO: response_type = PJS_USB_MSG_HELLO_REPLY; break;
    case PJS_USB_MSG_INFO: response_type = PJS_USB_MSG_INFO_REPLY; break;
    case PJS_USB_MSG_UPLOAD_BEGIN:
    case PJS_USB_MSG_UPLOAD_DATA:
    case PJS_USB_MSG_UPLOAD_END: response_type = PJS_USB_MSG_UPLOAD_REPLY; break;
    case PJS_USB_MSG_IMAGE_BEGIN:
    case PJS_USB_MSG_IMAGE_DATA:
    case PJS_USB_MSG_IMAGE_END: response_type = PJS_USB_MSG_IMAGE_REPLY; break;
    case PJS_USB_MSG_RUN: response_type = PJS_USB_MSG_RUN_REPLY; break;
    case PJS_USB_MSG_STOP: response_type = PJS_USB_MSG_STOP_REPLY; break;
    case PJS_USB_MSG_STATUS: response_type = PJS_USB_MSG_STATUS_REPLY; break;
    case PJS_USB_MSG_CHAINLOAD: response_type = PJS_USB_MSG_CHAINLOAD_REPLY; break;
    case PJS_USB_MSG_ENTER_MAINTENANCE:
        response_type = PJS_USB_MSG_ENTER_MAINTENANCE_REPLY; break;
    case PJS_USB_MSG_COMMIT_PACKAGE:
        response_type = PJS_USB_MSG_COMMIT_PACKAGE_REPLY; break;
    case PJS_USB_MSG_REBOOT:
        response_type = PJS_USB_MSG_REBOOT_REPLY; break;
    default: response_type = PJS_USB_MSG_ERROR; break;
    }
    return queue_frame(p, response_type, sequence, payload, 4u + extra_length);
}

static bool queue_error(PjsUsbProtocol *p, uint32_t sequence,
                        PjsUsbStatus status, uint32_t detail)
{
    uint8_t extra[4];
    put_u32(extra, detail);
    return queue_status(p, PJS_USB_MSG_ERROR, sequence, status, extra, 4u);
}

static bool flush_tx(PjsUsbProtocol *p)
{
    while (p->tx_pending) {
        int32_t result = p->config.io.write(
            p->config.io.transport, p->config.tx_buffer + p->tx_offset,
            p->tx_length - p->tx_offset);
        if (result < 0) return false;
        if (result == 0) return true;
        if ((uint32_t)result > p->tx_length - p->tx_offset) return false;
        p->tx_offset += (uint32_t)result;
        if (p->tx_offset == p->tx_length) {
            p->tx_pending = false;
            p->tx_length = 0u;
            p->tx_offset = 0u;
            if (p->chainload_ack_pending) {
                p->chainload_ack_pending = false;
                /* Defer promotion until the next protocol poll.  The
                 * low-level CDC service gets one intervening poll to hand
                 * the queued ACK to its USB IN DMA, so main cannot tear the
                 * controller down while the acknowledgement is still in the
                 * transport ring. */
                p->chainload_ack_written = true;
            }
            if (p->maintenance_ack_pending) {
                p->maintenance_ack_pending = false;
                p->maintenance_ack_written = true;
            }
            if (p->reboot_ack_pending) {
                p->reboot_ack_pending = false;
                p->reboot_ack_written = true;
            }
            if (p->runtime_boot_ack_pending) {
                p->runtime_boot_ack_pending = false;
                /* The following main-loop iteration services USB once before
                 * the synchronous QuickJS boot consumes the pending launch. */
                p->runtime_boot_ack_written = true;
            }
        }
    }
    return true;
}

static bool event_frame(PjsUsbProtocol *p, uint8_t type,
                        const uint8_t *payload, uint32_t length)
{
    uint32_t sequence = p->tx_sequence++;
    if (p->tx_sequence == 0u) p->tx_sequence = 1u;
    return queue_frame(p, type, sequence, payload, length);
}

static bool queue_text_event(PjsUsbProtocol *p, uint8_t type, uint8_t prefix,
                             const uint8_t *text, uint32_t length)
{
    if (!transport_ready(p) || p->tx_pending ||
        (text == 0 && length != 0u) || length > p->config.max_payload - 1u ||
        length + PJS_USB_PROTOCOL_HEADER_BYTES + 1u > p->config.tx_capacity) {
        return false;
    }
    uint8_t *frame = p->config.tx_buffer;
    frame[0] = PJS_USB_PROTOCOL_MAGIC_0;
    frame[1] = PJS_USB_PROTOCOL_MAGIC_1;
    frame[2] = PJS_USB_PROTOCOL_MAGIC_2;
    frame[3] = PJS_USB_PROTOCOL_MAGIC_3;
    frame[4] = PJS_USB_PROTOCOL_VERSION;
    frame[5] = type;
    frame[6] = 0u;
    frame[7] = 0u;
    put_u32(frame + 8u, p->tx_sequence++);
    if (p->tx_sequence == 0u) p->tx_sequence = 1u;
    put_u32(frame + 12u, length + 1u);
    frame[PJS_USB_PROTOCOL_HEADER_BYTES] = prefix;
    if (length != 0u) pjs_copy(frame + PJS_USB_PROTOCOL_HEADER_BYTES + 1u,
                                text, length);
    put_u32(frame + 16u, frame_crc(frame, length + 1u));
    p->tx_length = PJS_USB_PROTOCOL_HEADER_BYTES + length + 1u;
    p->tx_offset = 0u;
    p->tx_pending = true;
    return true;
}

bool pjs_usb_protocol_log(PjsUsbProtocol *p, uint8_t level,
                          const uint8_t *text, uint32_t length)
{
    if (p == 0 || (text == 0 && length != 0u) ||
        length > p->config.max_payload - 1u) {
        return false;
    }
    return queue_text_event(p, PJS_USB_MSG_LOG, level, text, length);
}

bool pjs_usb_protocol_result(PjsUsbProtocol *p, uint32_t result_code,
                             int32_t value)
{
    if (p == 0) return false;
    uint8_t payload[12];
    put_u32(payload, result_code);
    put_u32(payload + 4u, (uint32_t)value);
    put_u32(payload + 8u, p->frame_count);
    return event_frame(p, PJS_USB_MSG_RESULT, payload, sizeof(payload));
}

bool pjs_usb_protocol_prompt(PjsUsbProtocol *p, uint8_t kind,
                             const uint8_t *text, uint32_t length)
{
    if (p == 0 || (text == 0 && length != 0u) ||
        length > p->config.max_payload - 1u) {
        return false;
    }
    return queue_text_event(p, PJS_USB_MSG_PROMPT, kind, text, length);
}

static void reset_upload(PjsUsbProtocol *p)
{
    p->upload_active = false;
    p->expected_package_length = 0u;
    p->expected_package_crc = 0u;
    p->uploaded_package_length = 0u;
}

static void reset_image_upload(PjsUsbProtocol *p)
{
    p->image_upload_active = false;
    p->expected_image_length = 0u;
    p->expected_image_crc = 0u;
    p->uploaded_image_length = 0u;
}

static bool handle_frame(PjsUsbProtocol *p, const uint8_t *frame,
                         uint32_t payload_length)
{
    uint8_t type = frame[5];
    uint32_t sequence = get_u32(frame + 8u);
    const uint8_t *payload = frame + PJS_USB_PROTOCOL_HEADER_BYTES;

    if (type != PJS_USB_MSG_HELLO && p->have_rx_sequence &&
        sequence == p->last_rx_sequence) {
        return queue_error(p, sequence, PJS_USB_STATUS_BAD_SEQUENCE, sequence);
    }
    p->have_rx_sequence = true;
    p->last_rx_sequence = sequence;

    if (p->config.observe_only && type != PJS_USB_MSG_HELLO &&
        type != PJS_USB_MSG_INFO && type != PJS_USB_MSG_STATUS &&
        type != PJS_USB_MSG_ENTER_MAINTENANCE) {
        /* This gate precedes every command-specific mutation, including
         * upload buffer writes and STOP/RUN lifecycle calls. */
        return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
    }
    if (p->maintenance_active &&
        (type == PJS_USB_MSG_IMAGE_BEGIN || type == PJS_USB_MSG_IMAGE_DATA ||
         type == PJS_USB_MSG_IMAGE_END || type == PJS_USB_MSG_CHAINLOAD)) {
        return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
    }
    if (p->maintenance_active &&
        (p->reboot_request || p->reboot_ack_pending || p->reboot_ack_written) &&
        type != PJS_USB_MSG_HELLO && type != PJS_USB_MSG_INFO &&
        type != PJS_USB_MSG_STATUS && type != PJS_USB_MSG_REBOOT) {
        return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
    }

    switch (type) {
    case PJS_USB_MSG_HELLO: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        /* HELLO starts a fresh host epoch. Abandon only an incomplete
         * transaction so a reconnect can restart it from offset zero; a
         * fully admitted package or image remains available. */
        if (p->upload_active) reset_upload(p);
        if (p->image_upload_active) reset_image_upload(p);
        if (!p->runtime_active && !p->runtime_boot_pending) {
            p->state = p->package_valid || p->image_valid ?
                PJS_USB_STATE_READY : PJS_USB_STATE_IDLE;
        }
        uint8_t extra[16];
        put_u32(extra, PJS_USB_PROTOCOL_VERSION);
        put_u32(extra + 4u, p->config.max_payload);
        put_u32(extra + 8u, p->config.package_capacity);
        put_u32(extra + 12u, (uint32_t)p->state);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_INFO: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        /* Base + maximum error text + all optional diagnostic trailers. */
        uint8_t extra[480];
        put_u32(extra, PJS_USB_PROTOCOL_VERSION);
        put_u32(extra + 4u, (uint32_t)p->state);
        put_u32(extra + 8u, p->package_valid ? p->uploaded_package_length : 0u);
        put_u32(extra + 12u, p->package_crc);
        put_u32(extra + 16u, p->guest.package_hash_low);
        put_u32(extra + 20u, p->guest.package_hash_high);
        put_u32(extra + 24u, p->runtime_error);
        put_u32(extra + 28u, p->frame_count);
        put_u32(extra + 32u, p->image_valid ? 1u : 0u);
        put_u32(extra + 36u, p->image_valid ? p->uploaded_image_length : 0u);
        put_u32(extra + 40u, p->image_crc);
        put_u32(extra + 44u, p->chainload_ready ? 1u : 0u);
        volatile const PjsCrashRecord *crash =
            (volatile const PjsCrashRecord *)(uintptr_t)
                PJS_RETAINED_CRASH_ADDRESS;
        uint32_t crash_magic = crash->magic;
        put_u32(extra + 48u, crash_magic == PJS_CRASH_MAGIC ? crash_magic : 0u);
        put_u32(extra + 52u,
                crash_magic == PJS_CRASH_MAGIC ? crash->reason : 0u);
        put_u32(extra + 56u,
                crash_magic == PJS_CRASH_MAGIC ? crash->pc : 0u);
        put_u32(extra + 60u,
                crash_magic == PJS_CRASH_MAGIC ? crash->spsr : 0u);
        put_u32(extra + 64u, pjs_storage_last_error());
        put_u32(extra + 68u, pjs_storage_sector_read_count());
        put_u32(extra + 72u, pjs_storage_first_failed_lba());
        put_u32(extra + 76u, pjs_storage_sector_write_count());
        put_u32(extra + 80u, pjs_storage_sector_flush_count());
        put_u32(extra + 84u, pjs_storage_failed_operation());
        put_u32(extra + 88u, pjs_storage_failed_status());
        put_u32(extra + 92u, pjs_storage_failed_error());
        put_u32(extra + 96u, (uint32_t)pjs_fs_store_last_error_code());
        put_u32(extra + 100u, p->runtime_error_text_length);
        if (p->runtime_error_text_length != 0u) {
            pjs_copy(extra + 104u, p->runtime_error_text,
                     p->runtime_error_text_length);
        }
        uint32_t extra_length = 104u + p->runtime_error_text_length;
        if (p->config.observe_only || p->maintenance_active) {
            put_u32(extra + extra_length,
                    PJS_USB_PERFORMANCE_TRAILER_MARKER);
            const uint32_t values[10] = {
                p->performance.flags, p->performance.app_load_us,
                p->performance.app_boot_us,
                p->performance.launcher_teardown_us,
                p->performance.first_present_us, p->performance.last_frame_us,
                p->performance.max_frame_us, p->performance.last_present_us,
                p->performance.max_present_us,
                p->performance.lineage_commit_us,
            };
            for (uint32_t index = 0u; index < 10u; ++index) {
                put_u32(extra + extra_length + 4u + index * 4u, values[index]);
            }
            extra_length += 44u;
        }
        if (p->config.observe_only || p->maintenance_active) {
            const PjsQjsBootProfile *profile = qjs_runtime_boot_profile();
            const uint32_t values[12] = {
                profile->runtime_us, profile->context_us, profile->host_us,
                profile->native_pak_us, profile->eval_us, profile->jobs_us,
                profile->allocation_calls, profile->usb_service_us,
                profile->clock_source, profile->pll_control,
                profile->memory_timing, profile->device_init,
            };
            put_u32(extra + extra_length, PJS_USB_BOOT_PROFILE_TRAILER_MARKER);
            for (uint32_t index = 0u; index < 12u; ++index) {
                put_u32(extra + extra_length + 4u + index * 4u, values[index]);
            }
            extra_length += 52u;
        }
        if (p->power_valid) {
            const uint32_t values[11] = {
                p->power.battery_raw, p->power.battery_mv, p->power.flags,
                p->power.sample_count, p->power.failure_count,
                p->power.charger_requested_mode, p->power.gpo_enable,
                p->power.gpo_value, p->power.gpo_input,
                p->power.usb_configured, p->power.usb_suspended,
            };
            put_u32(extra + extra_length, PJS_USB_POWER_TRAILER_MARKER);
            for (uint32_t index = 0u; index < 11u; ++index)
                put_u32(extra + extra_length + 4u + index * 4u, values[index]);
            extra_length += 48u;
        }
        if (p->maintenance_active) {
            put_u32(extra + extra_length,
                    PJS_USB_PACKAGE_STATUS_TRAILER_MARKER);
            put_u32(extra + extra_length + 4u, (uint32_t)p->commit_status);
            put_u32(extra + extra_length + 8u, p->commit_generation);
            put_u32(extra + extra_length + 12u, p->commit_hash_low);
            put_u32(extra + extra_length + 16u, p->commit_hash_high);
            extra_length += 20u;
        }
        if (p->diagnostics_valid) {
            const PjsUsbDiagnostics *d = &p->diagnostics;
            const uint32_t values[17] = {
                d->heap_free, d->heap_largest_free, d->heap_allocated,
                d->audio_error, d->audio_busy, d->warm_returns, d->sleeps,
                d->wakes, d->return_failures, d->app_sessions,
                d->kernel_mode, d->first_kernel_error, d->lineage_error,
                d->lineage_generation, d->lineage_source,
                d->audio_underruns, d->audio_dma_fault,
            };
            put_u32(extra + extra_length, PJS_USB_DIAGNOSTICS_TRAILER_MARKER);
            for (uint32_t index = 0u; index < 17u; ++index)
                put_u32(extra + extra_length + 4u + index * 4u, values[index]);
            extra_length += 72u;
        }
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, extra_length);
    }
    case PJS_USB_MSG_UPLOAD_BEGIN: {
        if (payload_length != 8u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->runtime_active || p->runtime_boot_pending ||
            p->image_upload_active ||
            p->chainload_ack_pending || p->chainload_ready) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        uint32_t length = get_u32(payload);
        uint32_t crc = get_u32(payload + 4u);
        if (p->upload_active &&
            (length != p->expected_package_length ||
             crc != p->expected_package_crc)) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        if (length == 0u || length > p->config.package_capacity) {
            uint8_t extra[4];
            put_u32(extra, p->config.package_capacity);
            return queue_status(p, type, sequence, PJS_USB_STATUS_TOO_LARGE,
                                extra, sizeof(extra));
        }
        p->package_valid = false;
        p->image_valid = false;
        p->guest = (PjsGuestPackage){0};
        p->package_error = 0u;
        p->package_crc = 0u;
        p->image_crc = 0u;
        p->expected_package_length = length;
        p->expected_package_crc = crc;
        p->uploaded_package_length = 0u;
        p->upload_active = true;
        p->state = PJS_USB_STATE_UPLOADING;
        uint8_t extra[8];
        put_u32(extra, length);
        put_u32(extra + 4u, p->config.max_payload - 4u);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_ENTER_MAINTENANCE: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->maintenance_active || p->maintenance_request ||
            p->maintenance_ack_pending || p->maintenance_ack_written) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u);
        }
        if (!p->config.observe_only || !p->maintenance_available) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        if (!queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u)) {
            return false;
        }
        p->maintenance_request = true;
        p->maintenance_ack_pending = true;
        return true;
    }
    case PJS_USB_MSG_COMMIT_PACKAGE: {
        if (payload_length != 0u || !p->maintenance_active ||
            !p->package_valid || p->runtime_active || p->runtime_boot_pending ||
            p->upload_active || p->image_upload_active ||
            p->reboot_ack_pending || p->reboot_request ||
            p->config.commit_package == 0) {
            return queue_status(p, type, sequence,
                p->maintenance_active && p->config.commit_package != 0 ?
                    PJS_USB_STATUS_BUSY : PJS_USB_STATUS_NOT_READY, 0, 0u);
        }
        uint32_t generation = 0u;
        p->commit_generation = 0u;
        p->commit_hash_low = p->guest.package_hash_low;
        p->commit_hash_high = p->guest.package_hash_high;
        int result = p->config.commit_package(
            p->config.commit_context, p->config.package_buffer,
            p->uploaded_package_length, p->package_crc, &p->guest,
            &generation);
        p->commit_status = result;
        if (result != 0) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_INTERNAL,
                                0, 0u);
        }
        p->commit_generation = generation;
        uint8_t extra[4];
        put_u32(extra, generation);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_REBOOT: {
        if (payload_length != 0u || !p->maintenance_active ||
            p->runtime_active || p->runtime_boot_pending || p->upload_active ||
            p->image_upload_active || p->reboot_request ||
            p->reboot_ack_pending || p->reboot_ack_written) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        if (!queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u)) {
            return false;
        }
        p->reboot_request = true;
        p->reboot_ack_pending = true;
        return true;
    }
    case PJS_USB_MSG_UPLOAD_DATA: {
        if (payload_length < 5u || !p->upload_active) {
            return queue_status(p, type, sequence,
                                p->upload_active ? PJS_USB_STATUS_BAD_REQUEST :
                                                   PJS_USB_STATUS_NOT_READY,
                                0, 0u);
        }
        uint32_t offset = get_u32(payload);
        uint32_t data_length = payload_length - 4u;
        if (offset != p->uploaded_package_length || offset >
            p->expected_package_length ||
            data_length > p->expected_package_length - offset) {
            uint8_t extra[4];
            put_u32(extra, p->uploaded_package_length);
            return queue_status(p, type, sequence, PJS_USB_STATUS_OUT_OF_ORDER,
                                extra, sizeof(extra));
        }
        pjs_copy(p->config.package_buffer + offset, payload + 4u, data_length);
        p->uploaded_package_length += data_length;
        uint8_t extra[4];
        put_u32(extra, p->uploaded_package_length);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_UPLOAD_END: {
        if (payload_length != 0u || !p->upload_active) {
            return queue_status(p, type, sequence,
                                p->upload_active ? PJS_USB_STATUS_BAD_REQUEST :
                                                   PJS_USB_STATUS_NOT_READY,
                                0, 0u);
        }
        if (p->uploaded_package_length != p->expected_package_length) {
            reset_upload(p);
            p->state = PJS_USB_STATE_IDLE;
            return queue_status(p, type, sequence, PJS_USB_STATUS_OUT_OF_ORDER,
                                0, 0u);
        }
        p->package_crc = pjs_usb_protocol_crc32(
            p->config.package_buffer, p->uploaded_package_length);
        if (p->package_crc != p->expected_package_crc) {
            reset_upload(p);
            p->state = PJS_USB_STATE_IDLE;
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_CRC,
                                0, 0u);
        }
        p->guest = (PjsGuestPackage){0};
        int32_t package_result = pjs_package_open_ipod_photo(
            p->config.package_buffer, p->uploaded_package_length, &p->guest);
        p->upload_active = false;
        if (package_result != 0) {
            p->package_error = (uint32_t)package_result;
            p->state = PJS_USB_STATE_ERROR;
            uint8_t extra[4];
            put_u32(extra, p->package_error);
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_PACKAGE,
                                extra, sizeof(extra));
        }
        p->package_valid = true;
        p->state = PJS_USB_STATE_READY;
        uint8_t extra[4];
        put_u32(extra, p->package_crc);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_IMAGE_BEGIN: {
        if (payload_length != 8u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->runtime_active || p->runtime_boot_pending || p->upload_active ||
            p->chainload_ack_pending || p->chainload_ready) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        uint32_t length = get_u32(payload);
        uint32_t crc = get_u32(payload + 4u);
        if (p->image_upload_active &&
            (length != p->expected_image_length ||
             crc != p->expected_image_crc)) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        if (length == 0u || length > p->config.package_capacity) {
            uint8_t extra[4];
            put_u32(extra, p->config.package_capacity);
            return queue_status(p, type, sequence, PJS_USB_STATUS_TOO_LARGE,
                                extra, sizeof(extra));
        }
        p->package_valid = false;
        p->image_valid = false;
        p->guest = (PjsGuestPackage){0};
        p->package_error = 0u;
        p->image_crc = 0u;
        p->expected_image_length = length;
        p->expected_image_crc = crc;
        p->uploaded_image_length = 0u;
        p->image_upload_active = true;
        p->state = PJS_USB_STATE_UPLOADING;
        uint8_t extra[8];
        put_u32(extra, length);
        put_u32(extra + 4u, p->config.max_payload - 4u);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_IMAGE_DATA: {
        if (payload_length < 5u || !p->image_upload_active) {
            return queue_status(p, type, sequence,
                                p->image_upload_active ? PJS_USB_STATUS_BAD_REQUEST :
                                                         PJS_USB_STATUS_NOT_READY,
                                0, 0u);
        }
        uint32_t offset = get_u32(payload);
        uint32_t data_length = payload_length - 4u;
        if (offset != p->uploaded_image_length || offset >
            p->expected_image_length || data_length >
            p->expected_image_length - offset) {
            uint8_t extra[4];
            put_u32(extra, p->uploaded_image_length);
            return queue_status(p, type, sequence, PJS_USB_STATUS_OUT_OF_ORDER,
                                extra, sizeof(extra));
        }
        pjs_copy(p->config.package_buffer + offset, payload + 4u, data_length);
        p->uploaded_image_length += data_length;
        uint8_t extra[4];
        put_u32(extra, p->uploaded_image_length);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_IMAGE_END: {
        if (payload_length != 0u || !p->image_upload_active) {
            return queue_status(p, type, sequence,
                                p->image_upload_active ? PJS_USB_STATUS_BAD_REQUEST :
                                                         PJS_USB_STATUS_NOT_READY,
                                0, 0u);
        }
        if (p->uploaded_image_length != p->expected_image_length) {
            reset_image_upload(p);
            p->state = PJS_USB_STATE_IDLE;
            return queue_status(p, type, sequence, PJS_USB_STATUS_OUT_OF_ORDER,
                                0, 0u);
        }
        p->image_crc = pjs_usb_protocol_crc32(
            p->config.package_buffer, p->uploaded_image_length);
        if (p->image_crc != p->expected_image_crc) {
            reset_image_upload(p);
            p->state = PJS_USB_STATE_IDLE;
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_CRC,
                                0, 0u);
        }
        p->image_upload_active = false;
        if (!pjs_native_image_admissible(p->config.package_buffer,
                                         p->uploaded_image_length)) {
            p->package_error = PJS_USB_STATUS_BAD_PACKAGE;
            p->state = PJS_USB_STATE_ERROR;
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_PACKAGE,
                                0, 0u);
        }
        p->image_valid = true;
        p->state = PJS_USB_STATE_READY;
        uint8_t extra[4];
        put_u32(extra, p->image_crc);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    case PJS_USB_MSG_RUN: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->runtime_active) return queue_status(p, type, sequence,
                                                   PJS_USB_STATUS_OK, 0, 0u);
        if (p->runtime_boot_pending) {
            if (!queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u)) {
                return false;
            }
            p->runtime_boot_ack_pending = true;
            return true;
        }
        if (!p->package_valid) return queue_status(p, type, sequence,
                                                    PJS_USB_STATUS_NOT_READY,
                                                    0, 0u);
        p->runtime_error = 0u;
        clear_runtime_error_text(p);
        p->frame_count = 0u;
        if (!queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u)) {
            return false;
        }
        p->runtime_boot_pending = true;
        p->runtime_boot_ack_pending = true;
        p->state = PJS_USB_STATE_BOOTING;
        return true;
    }
    case PJS_USB_MSG_STOP:
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->runtime_active) {
            qjs_runtime_shutdown();
        }
        p->runtime_active = false;
        p->runtime_boot_pending = false;
        p->runtime_boot_ack_pending = false;
        p->runtime_boot_ack_written = false;
        p->runtime_error = reset_core();
        if (p->maintenance_active && p->upload_active) {
            /* STOP also abandons an incomplete RAM transfer, allowing the
             * next upload to use a different package after a disconnect. */
            p->upload_active = false;
            p->expected_package_length = 0u;
            p->expected_package_crc = 0u;
            p->uploaded_package_length = 0u;
        }
        clear_runtime_error_text(p);
        if (p->runtime_error != 0u) {
            p->state = PJS_USB_STATE_ERROR;
            uint8_t extra[4];
            put_u32(extra, p->runtime_error);
            return queue_status(p, type, sequence, PJS_USB_STATUS_RUNTIME,
                                extra, sizeof(extra));
        }
        p->state = p->package_valid ? PJS_USB_STATE_READY : PJS_USB_STATE_IDLE;
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u);
    case PJS_USB_MSG_CHAINLOAD: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        if (p->runtime_active || p->runtime_boot_pending || p->upload_active ||
            p->image_upload_active ||
            p->chainload_ack_pending || p->chainload_ready) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BUSY, 0, 0u);
        }
        if (!p->image_valid) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_NOT_READY,
                                0, 0u);
        }
        if (!queue_status(p, type, sequence, PJS_USB_STATUS_OK, 0, 0u)) {
            return false;
        }
        /* flush_tx() promotes this to chainload_ready only after the complete
         * acknowledgement has been accepted by the transport. */
        p->chainload_ack_pending = true;
        return true;
    }
    case PJS_USB_MSG_STATUS: {
        if (payload_length != 0u) {
            return queue_status(p, type, sequence, PJS_USB_STATUS_BAD_REQUEST,
                                0, 0u);
        }
        uint8_t extra[36];
        put_u32(extra, (uint32_t)p->state);
        put_u32(extra + 4u, p->runtime_active ? 1u : 0u);
        put_u32(extra + 8u, p->package_valid ? 1u : 0u);
        put_u32(extra + 12u, p->package_valid ? p->uploaded_package_length : 0u);
        put_u32(extra + 16u, p->image_valid ? 1u : 0u);
        put_u32(extra + 20u, p->image_valid ? p->uploaded_image_length : 0u);
        put_u32(extra + 24u, p->runtime_error);
        put_u32(extra + 28u, p->package_error);
        put_u32(extra + 32u, p->image_crc);
        return queue_status(p, type, sequence, PJS_USB_STATUS_OK,
                            extra, sizeof(extra));
    }
    default:
        return queue_error(p, sequence, PJS_USB_STATUS_BAD_REQUEST, type);
    }
}

static bool consume_frames(PjsUsbProtocol *p, uint32_t max_frames)
{
    uint32_t processed = 0u;
    if (max_frames == 0u) max_frames = 1u;
    while (processed < max_frames && p->rx_used >= 4u) {
        uint8_t *rx = p->config.rx_buffer;
        uint32_t magic_index = 0u;
        while (magic_index + 4u <= p->rx_used &&
               (rx[magic_index] != PJS_USB_PROTOCOL_MAGIC_0 ||
                rx[magic_index + 1u] != PJS_USB_PROTOCOL_MAGIC_1 ||
                rx[magic_index + 2u] != PJS_USB_PROTOCOL_MAGIC_2 ||
                rx[magic_index + 3u] != PJS_USB_PROTOCOL_MAGIC_3)) {
            ++magic_index;
        }
        if (magic_index != 0u) {
            if (magic_index + 4u > p->rx_used) magic_index = p->rx_used - 3u;
            pjs_move(rx, rx + magic_index, p->rx_used - magic_index);
            p->rx_used -= magic_index;
            if (p->rx_used < 4u) break;
        }
        if (p->rx_used < PJS_USB_PROTOCOL_HEADER_BYTES) break;
        if (rx[4] != PJS_USB_PROTOCOL_VERSION) {
            uint32_t sequence = get_u32(rx + 8u);
            uint8_t version = rx[4];
            pjs_move(rx, rx + 1u, p->rx_used - 1u);
            --p->rx_used;
            (void)queue_error(p, sequence, PJS_USB_STATUS_BAD_VERSION, version);
            ++processed;
            continue;
        }
        uint32_t payload_length = get_u32(rx + 12u);
        uint32_t frame_length = PJS_USB_PROTOCOL_HEADER_BYTES + payload_length;
        if (payload_length > p->config.max_payload ||
            frame_length > p->config.rx_capacity) {
            uint32_t sequence = get_u32(rx + 8u);
            pjs_move(rx, rx + 1u, p->rx_used - 1u);
            --p->rx_used;
            (void)queue_error(p, sequence, PJS_USB_STATUS_TOO_LARGE, payload_length);
            ++processed;
            continue;
        }
        if (p->rx_used < frame_length) break;
        if (get_u32(rx + 16u) != frame_crc(rx, payload_length)) {
            uint32_t sequence = get_u32(rx + 8u);
            pjs_move(rx, rx + 1u, p->rx_used - 1u);
            --p->rx_used;
            (void)queue_error(p, sequence, PJS_USB_STATUS_BAD_CRC, 0u);
            ++processed;
            continue;
        }
        /* A command cannot be safely acknowledged while another response is
         * queued. Leave the complete frame in place for the next poll. */
        if (p->tx_pending) break;
        if (!handle_frame(p, rx, payload_length)) return false;
        pjs_move(rx, rx + frame_length, p->rx_used - frame_length);
        p->rx_used -= frame_length;
        ++processed;
    }
    return true;
}

bool pjs_usb_protocol_poll(PjsUsbProtocol *p, uint32_t max_frames)
{
    if (!transport_ready(p)) return false;
    bool boot_ack_was_written = p->runtime_boot_ack_written;
    if (!flush_tx(p)) return false;
    if (boot_ack_was_written) {
        p->runtime_boot_ack_written = false;
    }
    if (p->chainload_ack_written) {
        p->chainload_ack_written = false;
        p->chainload_ready = true;
    }
    if (p->rx_used < p->config.rx_capacity) {
        int32_t result = p->config.io.read(
            p->config.io.transport, p->config.rx_buffer + p->rx_used,
            p->config.rx_capacity - p->rx_used);
        if (result < 0) return false;
        if ((uint32_t)result > p->config.rx_capacity - p->rx_used) return false;
        p->rx_used += (uint32_t)result;
    }
    if (!consume_frames(p, max_frames)) return false;
    return flush_tx(p);
}

void pjs_usb_protocol_service(PjsUsbProtocol *p)
{
    if (!transport_ready(p) || !p->runtime_boot_pending ||
        p->runtime_boot_ack_pending || p->runtime_boot_ack_written) {
        return;
    }

    p->runtime_boot_pending = false;
    /* A hot reload discards retained native state before QuickJS receives
     * the replacement guest. The boot remains synchronous on the main loop,
     * after the RUN acknowledgement has been handed to USB. */
    p->runtime_error = reset_core();
    if (p->runtime_error != 0u) {
        p->state = PJS_USB_STATE_ERROR;
        (void)pjs_usb_protocol_result(p, p->runtime_error, -1);
        return;
    }
    if (!qjs_runtime_boot(&p->guest)) {
        p->runtime_error = qjs_runtime_error_code();
        capture_runtime_error_text(p);
        uint32_t core_error = reset_core();
        if (p->runtime_error == 0u) p->runtime_error = core_error;
        p->state = PJS_USB_STATE_ERROR;
        (void)pjs_usb_protocol_result(p, p->runtime_error, -1);
        return;
    }
    p->runtime_active = true;
    p->frame_count = 0u;
    p->state = PJS_USB_STATE_RUNNING;
}

bool pjs_usb_protocol_runtime_frame(PjsUsbProtocol *p,
                                    const PjsCoreInput *input)
{
    if (!transport_ready(p) || input == 0) return false;
    if (!p->runtime_active) return true;
    if (p->state != PJS_USB_STATE_RUNNING || !qjs_runtime_frame(input)) {
        p->runtime_error = qjs_runtime_error_code();
        capture_runtime_error_text(p);
        qjs_runtime_shutdown();
        uint32_t core_error = reset_core();
        if (p->runtime_error == 0u) p->runtime_error = core_error;
        p->runtime_active = false;
        p->state = PJS_USB_STATE_ERROR;
        (void)pjs_usb_protocol_result(p, p->runtime_error, -1);
        return false;
    }
    ++p->frame_count;
    return true;
}

void pjs_usb_protocol_reset_observer_transport(PjsUsbProtocol *p)
{
    if (p == 0 || (!p->config.observe_only && !p->maintenance_active)) return;
    bool boot_pending = p->runtime_boot_pending;
    p->rx_used = 0u;
    p->tx_length = 0u;
    p->tx_offset = 0u;
    p->tx_sequence = 0u;
    p->tx_pending = false;
    p->have_rx_sequence = false;
    p->last_rx_sequence = 0u;
    p->maintenance_request = false;
    p->maintenance_ack_pending = false;
    p->maintenance_ack_written = false;
    p->reboot_request = false;
    p->reboot_ack_pending = false;
    p->reboot_ack_written = false;
    /* A lost RUN acknowledgement must not cause a later epoch to boot a
     * request that the host never observed.  Keep an already-running guest,
     * package/image validity, and their staging identity unchanged. */
    p->runtime_boot_pending = false;
    p->runtime_boot_ack_pending = false;
    p->runtime_boot_ack_written = false;
    p->chainload_ack_pending = false;
    p->chainload_ack_written = false;
    if (boot_pending && !p->runtime_active) {
        p->state = p->package_valid ? PJS_USB_STATE_READY : PJS_USB_STATE_IDLE;
    }
}

void pjs_usb_protocol_set_commit_callback(PjsUsbProtocol *p, void *context,
                                           PjsUsbPackageCommitFn callback)
{
    if (p == 0) return;
    p->config.commit_context = context;
    p->config.commit_package = callback;
}

bool pjs_usb_protocol_take_reboot_request(PjsUsbProtocol *p)
{
    if (p == 0 || !p->reboot_request || !p->reboot_ack_written ||
        p->tx_pending) return false;
    p->reboot_request = false;
    p->reboot_ack_written = false;
    return true;
}

void pjs_usb_protocol_set_maintenance_available(PjsUsbProtocol *p,
                                                 bool available)
{
    if (p == 0 || !p->config.observe_only) return;
    p->maintenance_available = available;
}

bool pjs_usb_protocol_take_maintenance_request(PjsUsbProtocol *p)
{
    if (p == 0 || !p->maintenance_request ||
        !p->maintenance_ack_written || p->tx_pending) return false;
    p->maintenance_request = false;
    p->maintenance_ack_written = false;
    return true;
}

bool pjs_usb_protocol_activate_maintenance(PjsUsbProtocol *p,
                                            uint8_t *package_buffer,
                                            uint32_t package_capacity)
{
    if (p == 0 || package_buffer == 0 || package_capacity < p->config.max_payload ||
        p->maintenance_active || p->runtime_active || p->runtime_boot_pending) {
        return false;
    }
    p->config.package_buffer = package_buffer;
    p->config.package_capacity = package_capacity;
    p->config.observe_only = false;
    p->maintenance_active = true;
    p->frame_count = 0u;
    p->maintenance_available = false;
    p->upload_active = false;
    p->image_upload_active = false;
    p->package_valid = false;
    p->image_valid = false;
    p->runtime_active = false;
    p->runtime_boot_pending = false;
    p->runtime_boot_ack_pending = false;
    p->runtime_boot_ack_written = false;
    p->chainload_ack_pending = false;
    p->chainload_ack_written = false;
    p->chainload_ready = false;
    p->expected_package_length = 0u;
    p->expected_package_crc = 0u;
    p->uploaded_package_length = 0u;
    p->expected_image_length = 0u;
    p->expected_image_crc = 0u;
    p->uploaded_image_length = 0u;
    p->image_crc = 0u;
    p->package_crc = 0u;
    p->package_error = 0u;
    p->runtime_error = 0u;
    p->runtime_error_text_length = 0u;
    p->state = PJS_USB_STATE_IDLE;
    p->guest = (PjsGuestPackage){0};
    return true;
}

void pjs_usb_protocol_publish_observation(PjsUsbProtocol *p,
                                           bool active,
                                           uint32_t frame_count,
                                           uint32_t runtime_error,
                                           uint32_t package_hash_low,
                                           uint32_t package_hash_high)
{
    if (p == 0 || !p->config.observe_only) return;
    p->runtime_active = active;
    p->frame_count = frame_count;
    p->runtime_error = runtime_error;
    if (runtime_error != 0u) capture_runtime_error_text(p);
    else clear_runtime_error_text(p);
    p->guest.package_hash_low = package_hash_low;
    p->guest.package_hash_high = package_hash_high;
    p->state = active ? PJS_USB_STATE_RUNNING : PJS_USB_STATE_READY;
}

void pjs_usb_protocol_publish_performance(PjsUsbProtocol *p,
                                          const PjsUsbPerformance *performance)
{
    if (p == 0 || performance == 0 ||
        (!p->config.observe_only && !p->maintenance_active)) return;
    p->performance = *performance;
}

void pjs_usb_protocol_publish_power(PjsUsbProtocol *p,
                                    const PjsUsbPowerTelemetry *power)
{
    if (p == 0 || power == 0) return;
    p->power = *power;
    p->power_valid = true;
}

bool pjs_usb_protocol_take_chainload(PjsUsbProtocol *p,
                                     const uint8_t **image,
                                     uint32_t *length)
{
    if (p == 0 || image == 0 || length == 0 ||
        !p->chainload_ready || !p->image_valid) {
        return false;
    }
    *image = p->config.package_buffer;
    *length = p->uploaded_image_length;
    p->chainload_ready = false;
    p->image_valid = false;
    p->state = PJS_USB_STATE_IDLE;
    return true;
}

bool pjs_usb_protocol_is_running(const PjsUsbProtocol *p)
{
    return p != 0 && p->runtime_active && p->state == PJS_USB_STATE_RUNNING;
}

PjsUsbState pjs_usb_protocol_state(const PjsUsbProtocol *p)
{
    return p == 0 ? PJS_USB_STATE_ERROR : p->state;
}

const PjsGuestPackage *pjs_usb_protocol_guest(const PjsUsbProtocol *p)
{
    return p != 0 && p->package_valid ? &p->guest : 0;
}
