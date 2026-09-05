#ifndef POCKETJS_IPOD_PHOTO_USB_DEVICE_H
#define POCKETJS_IPOD_PHOTO_USB_DEVICE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Polling-first USB device service for the PP5020 ARC controller.
 *
 * The service presents one CDC ACM function.  EP1 IN is the optional CDC
 * serial-state notification pipe; EP2 IN/OUT carry the byte stream.  The
 * controller itself is still packet/descriptor driven, but no CPU interrupt
 * is required: pjs_usb_device_poll() drains all pending controller events.
 */
enum pjs_usb_status {
    PJS_USB_OK = 0,
    PJS_USB_EINVAL = -1,
    PJS_USB_EBUSY = -2,
    PJS_USB_ENOTREADY = -3,
    PJS_USB_ETIMEOUT = -4,
    PJS_USB_EDISCONNECT = -5,
    PJS_USB_EIO = -6
};

/* A maximum-size 4 KiB protocol frame includes its 20-byte header.  Keep
 * enough ring headroom to accept the whole frame while the main loop drains
 * the preceding 512-byte DMA completion. */
#define PJS_USB_CDC_RX_CAPACITY 8192u
#define PJS_USB_CDC_TX_CAPACITY 8192u

/* Enable the PP5020 USB device controller and start the full-speed device. */
int pjs_usb_device_init(void);

/* Stop the controller and release its bus pull-up. */
void pjs_usb_device_shutdown(void);

/* Service reset, setup, transfer-complete, and disconnect events. */
void pjs_usb_device_poll(void);
/* Main-thread yield for long native/guest work. Services only the controller,
 * at most once per millisecond; never dispatches protocol or guest commands. */
void pjs_usb_device_poll_cooperative(void);

/* A VBUS/port connection is present and the controller is running. */
int pjs_usb_device_connected(void);

/* The host has selected the CDC configuration (configuration value 1). */
int pjs_usb_cdc_configured(void);
/* The host has put the USB bus into suspend while the device remains attached. */
int pjs_usb_device_suspended(void);
/* Changes whenever configuration resets transport rings, even if both
 * disconnect and reconnect occur inside cooperative polling. */
uint32_t pjs_usb_cdc_epoch(void);

/*
 * Copy bytes from/to the CDC stream rings.  read() returns the number copied,
 * zero when no byte is available, or a negative pjs_usb_status.  write()
 * returns the number accepted, which may be smaller than length when the
 * bounded TX ring is full.
 */
int pjs_usb_cdc_read(uint8_t *destination, size_t capacity);
int pjs_usb_cdc_write(const uint8_t *source, size_t length);
size_t pjs_usb_cdc_rx_available(void);
size_t pjs_usb_cdc_tx_space(void);

/* True only when the TX ring is empty and EP2 IN has no transfer in flight. */
int pjs_usb_cdc_tx_idle(void);

/* Current host line state from SET_CONTROL_LINE_STATE (DTR bit 0, RTS bit 1). */
uint16_t pjs_usb_cdc_line_state(void);

/* CDC line coding in USB little-endian form (baud, stop, parity, data bits). */
struct pjs_usb_cdc_line_coding {
    uint32_t baud_rate;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
};

void pjs_usb_cdc_get_line_coding(struct pjs_usb_cdc_line_coding *coding);

/* Compact bring-up snapshot for the main diagnostic screen.  The counters
 * are monotonic uint32_t values.  last_setup_lo/hi contain the raw eight-byte
 * SETUP packet as little-endian words; last_prime packs endpoint[3:0],
 * direction[8], and length[30:16]; last_error packs a local code in [31:24]
 * and controller status in [23:0]. */
typedef struct PjsUsbDebugSnapshot {
    uint32_t init_count;
    uint32_t reset_count;
    uint32_t setup_count;
    uint32_t descriptor_count;
    uint32_t prime_count;
    uint32_t completion_count;
    uint32_t error_count;
    uint32_t last_setup_lo;
    uint32_t last_setup_hi;
    uint32_t last_prime;
    uint32_t last_complete;
    uint32_t last_error;
} PjsUsbDebugSnapshot;

void pjs_usb_debug_snapshot(PjsUsbDebugSnapshot *out);

/*
 * Optional cache hooks.  The default no-op definitions in usb_device.c are
 * suitable while the PP5020 data cache is disabled.  A cache-owning runtime
 * may provide strong definitions with the same names.
 */
void pjs_usb_dma_clean(const void *address, size_t length);
void pjs_usb_dma_invalidate(const void *address, size_t length);

#endif
