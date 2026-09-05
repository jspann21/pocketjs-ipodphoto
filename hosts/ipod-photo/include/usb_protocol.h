#ifndef POCKETJS_IPOD_PHOTO_USB_PROTOCOL_H
#define POCKETJS_IPOD_PHOTO_USB_PROTOCOL_H

/*
 * Device-side control protocol for the USB test runner.
 *
 * This file intentionally knows nothing about the PP5020 USB controller.  The
 * board code supplies a non-blocking byte read/write pair and calls
 * pjs_usb_protocol_poll() from its main loop.  All storage is supplied by the
 * caller; the protocol never calls malloc and never takes ownership of a
 * caller-provided buffer.
 */

#include <stdbool.h>
#include <stdint.h>

#include "core_bridge.h"

#define PJS_USB_PROTOCOL_VERSION 1u
#define PJS_USB_PROTOCOL_MAGIC_0 ((uint8_t)'P')
#define PJS_USB_PROTOCOL_MAGIC_1 ((uint8_t)'J')
#define PJS_USB_PROTOCOL_MAGIC_2 ((uint8_t)'S')
#define PJS_USB_PROTOCOL_MAGIC_3 ((uint8_t)'U')

/* Fixed wire header: magic[4], version[1], type[1], flags[2],
 * sequence[4], payload_length[4], crc32[4].  Integer fields are little
 * endian.  CRC32 covers version through the final payload byte, excluding
 * magic and the CRC field itself. */
#define PJS_USB_PROTOCOL_HEADER_BYTES 20u
#define PJS_USB_PROTOCOL_MAX_PAYLOAD 4096u
#define PJS_USB_RUNTIME_ERROR_TEXT_MAX 128u
#define PJS_USB_PERFORMANCE_TRAILER_MARKER 0x31465250u /* PRF1 */
#define PJS_USB_BOOT_PROFILE_TRAILER_MARKER 0x31504251u /* QBP1 */
#define PJS_USB_POWER_TRAILER_MARKER 0x31525750u /* PWR1 */
#define PJS_USB_PACKAGE_STATUS_TRAILER_MARKER 0x31545350u /* PST1 */
#define PJS_USB_COMMIT_STATUS_UNAVAILABLE INT32_MIN
#define PJS_USB_DIAGNOSTICS_TRAILER_MARKER 0x314e4744u /* DGN1 */
#define PJS_USB_CPU_TRAILER_MARKER 0x31555043u /* CPU1 */

typedef struct {
    uint32_t heap_free, heap_largest_free, heap_allocated;
    uint32_t audio_error, audio_busy;
    uint32_t warm_returns, sleeps, wakes, return_failures, app_sessions;
    uint32_t kernel_mode, first_kernel_error, lineage_error;
    uint32_t lineage_generation, lineage_source, audio_underruns, audio_dma_fault;
} PjsUsbDiagnostics;

typedef struct {
    uint32_t flags;
    uint32_t app_load_us;
    uint32_t app_boot_us;
    uint32_t launcher_teardown_us;
    uint32_t first_present_us;
    uint32_t last_frame_us;
    uint32_t max_frame_us;
    uint32_t last_present_us;
    uint32_t max_present_us;
    uint32_t lineage_commit_us;
    uint32_t render_us, lcd_us, present_count;
} PjsUsbPerformance;

typedef struct {
    uint32_t battery_raw;
    uint32_t battery_mv;
    uint32_t flags;
    uint32_t sample_count;
    uint32_t failure_count;
    uint32_t charger_requested_mode;
    uint32_t gpo_enable;
    uint32_t gpo_value;
    uint32_t gpo_input;
    uint32_t usb_configured;
    uint32_t usb_suspended;
} PjsUsbPowerTelemetry;

typedef int (*PjsUsbPackageCommitFn)(void *context, const uint8_t *bytes,
                                     uint32_t length, uint32_t crc,
                                     const PjsGuestPackage *guest,
                                     uint32_t *generation);

typedef enum {
    PJS_USB_MSG_HELLO = 0x01u,
    PJS_USB_MSG_HELLO_REPLY = 0x81u,
    PJS_USB_MSG_INFO = 0x02u,
    PJS_USB_MSG_INFO_REPLY = 0x82u,

    PJS_USB_MSG_UPLOAD_BEGIN = 0x10u,
    PJS_USB_MSG_UPLOAD_DATA = 0x11u,
    PJS_USB_MSG_UPLOAD_END = 0x12u,
    PJS_USB_MSG_UPLOAD_REPLY = 0x90u,

    PJS_USB_MSG_IMAGE_BEGIN = 0x13u,
    PJS_USB_MSG_IMAGE_DATA = 0x14u,
    PJS_USB_MSG_IMAGE_END = 0x15u,
    PJS_USB_MSG_IMAGE_REPLY = 0x91u,

    PJS_USB_MSG_RUN = 0x20u,
    PJS_USB_MSG_STOP = 0x21u,
    PJS_USB_MSG_STATUS = 0x22u,
    PJS_USB_MSG_CHAINLOAD = 0x23u,
    PJS_USB_MSG_ENTER_MAINTENANCE = 0x24u,
    PJS_USB_MSG_COMMIT_PACKAGE = 0x25u,
    PJS_USB_MSG_REBOOT = 0x26u,
    PJS_USB_MSG_RUN_REPLY = 0xa0u,
    PJS_USB_MSG_STOP_REPLY = 0xa1u,
    PJS_USB_MSG_STATUS_REPLY = 0xa2u,
    PJS_USB_MSG_CHAINLOAD_REPLY = 0xa3u,
    PJS_USB_MSG_ENTER_MAINTENANCE_REPLY = 0xa4u,
    PJS_USB_MSG_COMMIT_PACKAGE_REPLY = 0xa5u,
    PJS_USB_MSG_REBOOT_REPLY = 0xa6u,

    PJS_USB_MSG_LOG = 0x30u,
    PJS_USB_MSG_RESULT = 0x31u,
    PJS_USB_MSG_PROMPT = 0x32u,
    PJS_USB_MSG_ERROR = 0x7fu,
} PjsUsbMessageType;

typedef enum {
    PJS_USB_STATUS_OK = 0u,
    PJS_USB_STATUS_BAD_REQUEST = 1u,
    PJS_USB_STATUS_BAD_VERSION = 2u,
    PJS_USB_STATUS_BAD_SEQUENCE = 3u,
    PJS_USB_STATUS_BUSY = 4u,
    PJS_USB_STATUS_TOO_LARGE = 5u,
    PJS_USB_STATUS_OUT_OF_ORDER = 6u,
    PJS_USB_STATUS_BAD_CRC = 7u,
    PJS_USB_STATUS_BAD_PACKAGE = 8u,
    PJS_USB_STATUS_NOT_READY = 9u,
    PJS_USB_STATUS_RUNTIME = 10u,
    PJS_USB_STATUS_NO_MEMORY = 11u,
    PJS_USB_STATUS_INTERNAL = 12u,
} PjsUsbStatus;

typedef enum {
    PJS_USB_STATE_IDLE = 0u,
    PJS_USB_STATE_UPLOADING = 1u,
    PJS_USB_STATE_READY = 2u,
    PJS_USB_STATE_RUNNING = 3u,
    PJS_USB_STATE_ERROR = 4u,
    PJS_USB_STATE_BOOTING = 5u,
} PjsUsbState;

/* Payload contracts (all integer fields little endian):
 *
 *   HELLO/INFO: empty request.  Their replies begin with status:u32; HELLO
 *     then has version, max_payload, package_capacity, state; INFO has
 *     version, state, package_length, package_crc, package_hash_low/high,
 *     runtime_error, frame_count, image_valid, image_length, image_crc,
 *     chainload_ready.
 *   UPLOAD_BEGIN: total_length:u32, package_crc:u32.
 *   UPLOAD_DATA: offset:u32 followed by package bytes.  Offsets must be
 *     contiguous; the reply's optional u32 is the next expected offset.
 *   UPLOAD_END/RUN/STOP/STATUS: empty request.  Every command reply begins
 *     status:u32.  STATUS adds state, runtime_active, package_valid,
 *     package_length, image_valid, image_length, runtime_error,
 *     package_error, image_crc.
 *   IMAGE_BEGIN/IMAGE_DATA/IMAGE_END: same transaction shape as package
 *     upload, but the bytes are validated by pjs_native_image_admissible()
 *     and remain in the caller-provided staging buffer.  CHAINLOAD is empty;
 *     its acknowledgement must finish writing before main may take the
 *     image with pjs_usb_protocol_take_chainload().
 *   LOG/PROMPT: level-or-kind:u8 followed by UTF-8/opaque text bytes.
 *   RESULT: result_code:u32, value:i32, frame_count:u32.
 */

/* read() returns a positive number of bytes, zero when no byte is currently
 * available, or a negative transport error.  write() may short-write and
 * must return zero for a temporary would-block condition. */
typedef int32_t (*PjsUsbReadFn)(void *transport, uint8_t *bytes,
                                uint32_t capacity);
typedef int32_t (*PjsUsbWriteFn)(void *transport, const uint8_t *bytes,
                                 uint32_t length);

typedef struct {
    void *transport;
    PjsUsbReadFn read;
    PjsUsbWriteFn write;
} PjsUsbIo;

typedef struct {
    PjsUsbIo io;
    uint8_t *rx_buffer;
    uint32_t rx_capacity;
    uint8_t *tx_buffer;
    uint32_t tx_capacity;
    uint8_t *package_buffer;
    uint32_t package_capacity;
    uint32_t max_payload;
    /* Read-only resident-app observer. HELLO/INFO/STATUS remain available;
     * every upload/run/stop/chainload command is rejected before mutation. */
    bool observe_only;
    void *commit_context;
    PjsUsbPackageCommitFn commit_package;
} PjsUsbProtocolConfig;

typedef struct {
    PjsUsbProtocolConfig config;
    uint32_t rx_used;
    uint32_t tx_length;
    uint32_t tx_offset;
    uint32_t tx_sequence;
    bool tx_pending;
    bool upload_active;
    bool image_upload_active;
    bool package_valid;
    bool image_valid;
    bool runtime_active;
    bool runtime_boot_pending;
    bool runtime_boot_ack_pending;
    bool runtime_boot_ack_written;
    bool chainload_ack_pending;
    bool chainload_ack_written;
    bool chainload_ready;
    bool maintenance_available;
    bool maintenance_request;
    bool maintenance_ack_pending;
    bool maintenance_ack_written;
    bool maintenance_active;
    bool reboot_request;
    bool reboot_ack_pending;
    bool reboot_ack_written;
    int32_t commit_status;
    uint32_t commit_generation;
    uint32_t commit_hash_low;
    uint32_t commit_hash_high;
    bool have_rx_sequence;
    uint32_t last_rx_sequence;
    uint32_t expected_package_length;
    uint32_t expected_package_crc;
    uint32_t uploaded_package_length;
    uint32_t expected_image_length;
    uint32_t expected_image_crc;
    uint32_t uploaded_image_length;
    uint32_t image_crc;
    uint32_t package_crc;
    uint32_t package_error;
    uint32_t runtime_error;
    uint32_t runtime_error_text_length;
    uint8_t runtime_error_text[PJS_USB_RUNTIME_ERROR_TEXT_MAX];
    uint32_t frame_count;
    PjsUsbPerformance performance;
    PjsUsbPowerTelemetry power;
    bool power_valid;
    PjsUsbDiagnostics diagnostics;
    bool diagnostics_valid;
    PjsUsbState state;
    PjsGuestPackage guest;
} PjsUsbProtocol;

/* Returns false when the supplied buffers/callbacks cannot support the
 * configured protocol.  The buffers must remain valid for the lifetime of p. */
bool pjs_usb_protocol_init(PjsUsbProtocol *p,
                           const PjsUsbProtocolConfig *config);

/* Drain available transport bytes, parse complete frames, and flush pending
 * output.  max_frames bounds work performed in one main-loop iteration.  A
 * zero limit means one frame; the function returns false only for a transport
 * error or invalid context. */
bool pjs_usb_protocol_poll(PjsUsbProtocol *p, uint32_t max_frames);

/* Start an admitted RUN after its acknowledgement has had an intervening
 * transport poll. QuickJS boot remains synchronous, but no longer delays the
 * RUN reply. State becomes RUNNING or ERROR before this function returns. */
void pjs_usb_protocol_service(PjsUsbProtocol *p);

/* Run one JavaScript frame after poll().  The caller remains responsible for
 * pjs_core_step/render and for constructing the input facts. */
bool pjs_usb_protocol_runtime_frame(PjsUsbProtocol *p,
                                    const PjsCoreInput *input);

/* Publish telemetry for a resident guest without changing ownership or
 * invoking any runtime/core lifecycle operation. */
void pjs_usb_protocol_publish_observation(PjsUsbProtocol *p,
                                           bool active,
                                           uint32_t frame_count,
                                           uint32_t runtime_error,
                                           uint32_t package_hash_low,
                                           uint32_t package_hash_high);

/* Permit one explicit observer-to-RAM-maintenance transition while the
 * resident app is active and safe to quiesce. */
void pjs_usb_protocol_set_maintenance_available(PjsUsbProtocol *p,
                                                 bool available);
bool pjs_usb_protocol_take_maintenance_request(PjsUsbProtocol *p);
bool pjs_usb_protocol_activate_maintenance(PjsUsbProtocol *p,
                                            uint8_t *package_buffer,
                                            uint32_t package_capacity);

/* Configure the synchronous, bounded package persistence callback. */
void pjs_usb_protocol_set_commit_callback(PjsUsbProtocol *p, void *context,
                                           PjsUsbPackageCommitFn callback);
bool pjs_usb_protocol_take_reboot_request(PjsUsbProtocol *p);

/* Publish startup/frame timing for resident and RAM-maintenance runtimes. */
void pjs_usb_protocol_publish_performance(PjsUsbProtocol *p,
                                          const PjsUsbPerformance *performance);

/* Publish a caller-sampled power snapshot. This function performs no
 * hardware reads; INFO appends PWR1 only after a valid snapshot is supplied. */
void pjs_usb_protocol_publish_power(PjsUsbProtocol *p,
                                    const PjsUsbPowerTelemetry *power);

/* Drop only observer transport fragments after a USB reset/reconfiguration. */
void pjs_usb_protocol_reset_observer_transport(PjsUsbProtocol *p);

/* Once CHAINLOAD's reply has completely drained through io.write(), return
 * the staged image exactly once.  Main must stop the runtime and USB service
 * before calling pjs_native_image_chainload(image, length). */
bool pjs_usb_protocol_take_chainload(PjsUsbProtocol *p,
                                     const uint8_t **image,
                                     uint32_t *length);

bool pjs_usb_protocol_is_running(const PjsUsbProtocol *p);
PjsUsbState pjs_usb_protocol_state(const PjsUsbProtocol *p);
const PjsGuestPackage *pjs_usb_protocol_guest(const PjsUsbProtocol *p);

/* Best-effort asynchronous device events.  They return false if a frame is
 * already queued or the supplied event exceeds the configured TX capacity. */
bool pjs_usb_protocol_log(PjsUsbProtocol *p, uint8_t level,
                          const uint8_t *text, uint32_t length);
bool pjs_usb_protocol_result(PjsUsbProtocol *p, uint32_t result_code,
                             int32_t value);
bool pjs_usb_protocol_prompt(PjsUsbProtocol *p, uint8_t kind,
                             const uint8_t *text, uint32_t length);

/* Standard little-endian CRC helper, also useful to a host implementation
 * when constructing UPLOAD_BEGIN. */
uint32_t pjs_usb_protocol_crc32(const uint8_t *bytes, uint32_t length);

#endif
