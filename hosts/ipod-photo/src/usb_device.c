#include "usb_device.h"

#include "cache.h"
#include "pp5020.h"
#include "power.h"
#include "timer.h"

#include <stddef.h>
#include <stdint.h>


/*
 * PP5020 contains the ARC USBOTG device controller.  The controller is an
 * EHCI-shaped endpoint engine: a 2048-byte-aligned queue-head table points at
 * 32-byte transfer descriptors.  The register names below intentionally use
 * the USB programming model rather than importing a driver implementation.
 */
#define PJS_USB_BASE             0xc5000000u
#define PJS_USB_USBCMD           MMIO32(PJS_USB_BASE + 0x140u)
#define PJS_USB_USBSTS           MMIO32(PJS_USB_BASE + 0x144u)
#define PJS_USB_USBINTR          MMIO32(PJS_USB_BASE + 0x148u)
#define PJS_USB_DEVICEADDR       MMIO32(PJS_USB_BASE + 0x154u)
#define PJS_USB_ENDPOINTLISTADDR MMIO32(PJS_USB_BASE + 0x158u)
#define PJS_USB_PORTSC1          MMIO32(PJS_USB_BASE + 0x184u)
#define PJS_USB_OTGSC            MMIO32(PJS_USB_BASE + 0x1a4u)
#define PJS_USB_USBMODE          MMIO32(PJS_USB_BASE + 0x1a8u)
#define PJS_USB_ENDPTSETUPSTAT   MMIO32(PJS_USB_BASE + 0x1acu)
#define PJS_USB_ENDPTPRIME       MMIO32(PJS_USB_BASE + 0x1b0u)
#define PJS_USB_ENDPTFLUSH       MMIO32(PJS_USB_BASE + 0x1b4u)
#define PJS_USB_ENDPTSTATUS      MMIO32(PJS_USB_BASE + 0x1b8u)
#define PJS_USB_ENDPTCOMPLETE    MMIO32(PJS_USB_BASE + 0x1bcu)
#define PJS_USB_ENDPTCTRL(n)     MMIO32(PJS_USB_BASE + 0x1c0u + 4u * (n))

/* Controller command/status bits. */
#define PJS_USB_CMD_RUN          0x00000001u
#define PJS_USB_CMD_RESET        0x00000002u
#define PJS_USB_STS_INT          0x00000001u
#define PJS_USB_STS_ERR          0x00000002u
#define PJS_USB_STS_PORT_CHANGE  0x00000004u
#define PJS_USB_STS_RESET        0x00000040u
#define PJS_USB_STS_SUSPEND      0x00000100u
#define PJS_USB_INTR_ENABLE      (PJS_USB_STS_INT | PJS_USB_STS_ERR | \
                                  PJS_USB_STS_PORT_CHANGE | PJS_USB_STS_RESET)
#define PJS_USB_MODE_DEVICE      0x00000002u
#define PJS_USB_MODE_STREAM_OFF  0x00000010u
#define PJS_USB_ADDR_SHIFT       25u
#define PJS_USB_FORCE_FULL_SPEED 0x01000000u
#define PJS_USB_PORT_CONNECTED   0x00000001u
#define PJS_USB_PORT_SUSPEND     0x00000080u
#define PJS_USB_VBUS_VALID       0x00000200u

/* Endpoint control fields. */
#define PJS_USB_EP_TX_ENABLE     0x00800000u
#define PJS_USB_EP_TX_TOGGLE_RST 0x00400000u
#define PJS_USB_EP_TX_STALL      0x00010000u
#define PJS_USB_EP_TX_TYPE_SHIFT 18u
#define PJS_USB_EP_RX_ENABLE     0x00000080u
#define PJS_USB_EP_RX_TOGGLE_RST 0x00000040u
#define PJS_USB_EP_RX_STALL      0x00000001u
#define PJS_USB_EP_RX_TYPE_SHIFT 2u
#define PJS_USB_EP_TYPE_BULK     2u
#define PJS_USB_EP_TYPE_INTERRUPT 3u

/* Queue/dTD fields. */
#define PJS_USB_QH_ZLT           0x20000000u
#define PJS_USB_QH_IOS           0x00008000u
#define PJS_USB_QH_MPS_SHIFT     16u
#define PJS_USB_DTD_TERMINATE    0x00000001u
#define PJS_USB_DTD_IOC          0x00008000u
#define PJS_USB_DTD_ACTIVE       0x00000080u
#define PJS_USB_DTD_HALTED       0x00000040u
#define PJS_USB_DTD_ERROR        (PJS_USB_DTD_HALTED | 0x00000028u)
#define PJS_USB_DTD_REMAIN_MASK  0x7fff0000u
#define PJS_USB_DTD_MAX_LENGTH   0x7fffu

#define PJS_USB_EP0              0u
#define PJS_USB_EP_NOTIFY        1u
#define PJS_USB_EP_DATA          2u
#define PJS_USB_DIR_OUT          0u
#define PJS_USB_DIR_IN           1u
#define PJS_USB_PIPE(ep, dir)    ((ep) * 2u + (dir))
#define PJS_USB_PIPE_BIT(ep, dir) \
    (1u << ((ep) + ((dir) == PJS_USB_DIR_IN ? 16u : 0u)))
#define PJS_USB_PIPE_HW_BIT(pipe) \
    PJS_USB_PIPE_BIT((pipe) / 2u, (pipe) & 1u)

#define PJS_USB_HW_WAIT_ITERS    1000000u
#define PJS_USB_ATTACH_SETTLE_US  500000u
#define PJS_USB_DETACH_US         250000u
#define PJS_USB_RX_DMA_SIZE      512u
#define PJS_USB_TX_DMA_SIZE      512u
#define PJS_USB_CTRL_DMA_SIZE    128u
#define PJS_USB_NOTIFY_SIZE      10u
#define PJS_USB_SETUP_SIZE       8u

struct pjs_usb_dtd {
    uint32_t next;
    uint32_t size_status;
    uint32_t buffer[5];
    uint32_t software;
} __attribute__((packed));

struct pjs_usb_qh {
    uint32_t cap;
    uint32_t current;
    struct pjs_usb_dtd overlay;
    uint32_t setup[2];
    uint32_t reserved[4];
} __attribute__((packed));

_Static_assert(sizeof(struct pjs_usb_dtd) == 32u,
               "PP5020 dTD layout must remain 32 bytes");
_Static_assert(sizeof(struct pjs_usb_qh) == 64u,
               "PP5020 queue-head layout must remain 64 bytes");

/* The ARC controller consumes queue heads and dTDs from IRAM on PP5020. The
 * native Rockbox layout reserves the first 2 KiB-aligned IRAM page for the
 * queue-head table and keeps the remaining DMA workspace in that same
 * uncached window. Do not move these objects to SDRAM: ORing 0x10000000 onto
 * an IRAM address would turn it into an invalid bus address. This standalone
 * image reserves 0x4000c000, a 2 KiB-aligned region above the exception
 * stacks and below the audio-clock scratch slot at 0x40010000. */
#define PJS_USB_IRAM_BASE       0x4000c000u
#define PJS_USB_WORKSPACE_BYTES 0x0e00u
#define PJS_USB_IRAM_LIMIT      (PJS_USB_IRAM_BASE + PJS_USB_WORKSPACE_BYTES)
#define PJS_USB_QH_BASE         (PJS_USB_IRAM_BASE + 0x0000u)
#define PJS_USB_TD_BASE         (PJS_USB_IRAM_BASE + 0x0800u)
#define PJS_USB_RX_DMA_BASE     (PJS_USB_IRAM_BASE + 0x0900u)
#define PJS_USB_TX_DMA_BASE     (PJS_USB_IRAM_BASE + 0x0b00u)
#define PJS_USB_CTRL_DMA_BASE   (PJS_USB_IRAM_BASE + 0x0d00u)
#define PJS_USB_NOTIFY_BASE     (PJS_USB_IRAM_BASE + 0x0d80u)
#define PJS_USB_QH_COUNT        6u
#define PJS_USB_TD_COUNT        6u
#define PJS_USB_QH_BYTES        (PJS_USB_QH_COUNT * sizeof(struct pjs_usb_qh))
#define PJS_USB_TD_BYTES        (PJS_USB_TD_COUNT * sizeof(struct pjs_usb_dtd))

#define pjs_usb_qhs \
    ((struct pjs_usb_qh *)(uintptr_t)PJS_USB_QH_BASE)
#define pjs_usb_td \
    ((struct pjs_usb_dtd *)(uintptr_t)PJS_USB_TD_BASE)
#define pjs_usb_rx_dma \
    ((uint8_t *)(uintptr_t)PJS_USB_RX_DMA_BASE)
#define pjs_usb_tx_dma \
    ((uint8_t *)(uintptr_t)PJS_USB_TX_DMA_BASE)
#define pjs_usb_ctrl_dma \
    ((uint8_t *)(uintptr_t)PJS_USB_CTRL_DMA_BASE)
#define pjs_usb_notification \
    ((uint8_t *)(uintptr_t)PJS_USB_NOTIFY_BASE)

static uint8_t pjs_usb_rx_ring[PJS_USB_CDC_RX_CAPACITY]
    __attribute__((section(".bss.usb_ring"), aligned(32)));
static uint8_t pjs_usb_tx_ring[PJS_USB_CDC_TX_CAPACITY]
    __attribute__((section(".bss.usb_ring"), aligned(32)));

struct pjs_usb_ctrl_state {
    uint8_t setup[PJS_USB_SETUP_SIZE];
    uint8_t stage;
    uint8_t descriptor_kind;
    uint16_t descriptor_index;
    uint16_t expected;
    uint16_t actual;
    uint8_t pending_address;
    uint8_t pending_configuration;
};

enum {
    PJS_USB_CTRL_IDLE = 0,
    PJS_USB_CTRL_DATA_IN,
    PJS_USB_CTRL_DATA_OUT,
    PJS_USB_CTRL_STATUS_IN,
    PJS_USB_CTRL_STATUS_OUT
};

static struct pjs_usb_ctrl_state pjs_usb_ctrl;
static struct pjs_usb_cdc_line_coding pjs_usb_line = { 115200u, 0u, 0u, 8u };
static uint16_t pjs_usb_line_state_value;
static uint16_t pjs_usb_rx_head;
static uint16_t pjs_usb_rx_tail;
static uint16_t pjs_usb_tx_head;
static uint16_t pjs_usb_tx_tail;
static uint16_t pjs_usb_tx_active;
static uint8_t pjs_usb_rx_active;
static uint8_t pjs_usb_tx_active_flag;
static uint8_t pjs_usb_notify_active;
static uint8_t pjs_usb_notify_pending;
static uint8_t pjs_usb_running;
static uint8_t pjs_usb_configured;
/* USB permits the configured current only after the SET_CONFIGURATION
 * status stage has completed successfully. */
static uint8_t pjs_usb_charging_authorized;
static uint32_t pjs_usb_epoch;
static PjsUsbDebugSnapshot pjs_usb_debug;

static void pjs_usb_apply_charger_policy(void)
{
    uint8_t desired;
    /* VBUS is sampled directly from the same board pin used by power.c. The
     * controller's PORTSC connected bit alone is not sufficient during
     * detach/reset transitions. */
    if (!pjs_usb_running || (PP_GPIOD_INPUT_VAL & 0x08u) == 0u ||
        (PJS_USB_PORTSC1 & PJS_USB_PORT_SUSPEND) != 0u) {
        desired = PJS_POWER_CHARGER_SUSPEND;
    } else if (pjs_usb_configured && pjs_usb_charging_authorized) {
        desired = PJS_POWER_CHARGER_USB_500MA;
    } else {
        desired = PJS_POWER_CHARGER_USB_100MA;
    }
    if (power_charger_mode() != desired) {
        (void)power_charger_set_mode(desired);
    }
}

/* Cache-disabled operation is the normal phase-one mode. */
__attribute__((weak)) void pjs_usb_dma_clean(const void *address, size_t length)
{
    (void)address;
    (void)length;
}

__attribute__((weak)) void pjs_usb_dma_invalidate(const void *address, size_t length)
{
    (void)address;
    (void)length;
}

static void pjs_usb_barrier(void)
{
    /* ARM7TDMI has no architectural DMB instruction.  The volatile compiler
     * barrier is sufficient with the PP5020 cache disabled; a cache-owning
     * integration must also supply the DMA hooks declared in the header. */
    __asm__ volatile("" ::: "memory");
}

static int pjs_usb_wait_clear(volatile uint32_t *reg, uint32_t mask)
{
    uint32_t count;
    for (count = 0u; count < PJS_USB_HW_WAIT_ITERS; ++count) {
        if ((*reg & mask) == 0u) {
            return PJS_USB_OK;
        }
    }
    return PJS_USB_ETIMEOUT;
}

static void pjs_usb_debug_error(uint32_t code, uint32_t raw)
{
    pjs_usb_debug.error_count++;
    pjs_usb_debug.last_error = ((code & 0xffu) << 24u) |
                               (raw & 0x00ffffffu);
}

static int pjs_usb_connected_raw(void)
{
    uint32_t otg = PJS_USB_OTGSC;
    /* PORTSC suspend is an idle bus state, not cable removal. */
    return (PJS_USB_PORTSC1 & PJS_USB_PORT_CONNECTED) != 0u ||
           (otg & PJS_USB_VBUS_VALID) != 0u;
}

static uint32_t pjs_usb_ptr(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    if (value >= PJS_USB_IRAM_BASE && value < PJS_USB_IRAM_LIMIT) {
        return (uint32_t)value;
    }
    return (uint32_t)(value | (uintptr_t)PP_NOCACHE_BASE);
}

static void pjs_usb_set_buffer(struct pjs_usb_dtd *td, const void *address)
{
    uintptr_t start = (uintptr_t)address;
    td->buffer[0] = (uint32_t)start;
    td->buffer[1] = (uint32_t)((start & ~(uintptr_t)0xfffu) + 0x1000u);
    td->buffer[2] = (uint32_t)((start & ~(uintptr_t)0xfffu) + 0x2000u);
    td->buffer[3] = (uint32_t)((start & ~(uintptr_t)0xfffu) + 0x3000u);
    td->buffer[4] = (uint32_t)((start & ~(uintptr_t)0xfffu) + 0x4000u);
}

static void pjs_usb_flush_pipe(uint32_t pipe)
{
    uint32_t bit = PJS_USB_PIPE_HW_BIT(pipe);
    PJS_USB_ENDPTFLUSH = bit;
    if (pjs_usb_wait_clear(&PJS_USB_ENDPTFLUSH, bit) != PJS_USB_OK) {
        pjs_usb_debug_error(3u, bit);
    }
    /* A flushed dTD may already have raised IOC.  Clear it before a new
     * SETUP can be dispatched, otherwise the completion can be attributed to
     * the replacement control transfer. */
    PJS_USB_ENDPTCOMPLETE = bit;
    pjs_usb_qhs[pipe].overlay.next = PJS_USB_DTD_TERMINATE;
    pjs_usb_qhs[pipe].overlay.size_status = 0u;
    pjs_usb_td[pipe].next = PJS_USB_DTD_TERMINATE;
    pjs_usb_td[pipe].size_status = 0u;
}

static int pjs_usb_prime(uint32_t endpoint, uint32_t direction,
                         void *buffer, uint32_t length)
{
    uint32_t pipe;
    uint32_t bit;
    struct pjs_usb_dtd *td;
    struct pjs_usb_qh *qh;

    pjs_usb_debug.prime_count++;
    pjs_usb_debug.last_prime = (endpoint & 0x0fu) |
                               ((direction & 1u) << 8u) |
                               ((length & 0x7fffu) << 16u);
    if (endpoint > PJS_USB_EP_DATA || direction > PJS_USB_DIR_IN ||
        length > PJS_USB_DTD_MAX_LENGTH ||
        (length != 0u && buffer == NULL)) {
        return PJS_USB_EINVAL;
    }
    pipe = PJS_USB_PIPE(endpoint, direction);
    bit = PJS_USB_PIPE_BIT(endpoint, direction);
    td = &pjs_usb_td[pipe];
    qh = &pjs_usb_qhs[pipe];
    if ((PJS_USB_ENDPTSTATUS & bit) != 0u ||
        (PJS_USB_ENDPTPRIME & bit) != 0u) {
        return PJS_USB_EBUSY;
    }
    td->next = PJS_USB_DTD_TERMINATE;
    td->size_status = (length << 16u) | PJS_USB_DTD_ACTIVE | PJS_USB_DTD_IOC;
    pjs_usb_set_buffer(td, buffer != NULL ? buffer : pjs_usb_ctrl_dma);
    pjs_usb_dma_clean(buffer != NULL ? buffer : pjs_usb_ctrl_dma, length);
    pjs_usb_dma_clean(td, sizeof(*td));
    qh->current = 0u;
    qh->overlay.next = pjs_usb_ptr(td);
    qh->overlay.size_status = 0u;
    pjs_usb_dma_clean(qh, sizeof(*qh));
    pjs_usb_barrier();
    PJS_USB_ENDPTPRIME |= bit;
    if (pjs_usb_wait_clear(&PJS_USB_ENDPTPRIME, bit) != PJS_USB_OK) {
        pjs_usb_flush_pipe(pipe);
        pjs_usb_debug_error(4u, bit);
        return PJS_USB_ETIMEOUT;
    }
    /* ENDPTPRIME clearing only says that the controller accepted the dTD.
     * ENDPTSTATUS may remain clear until the next USB transaction, while the
     * dTD is correctly ACTIVE. Treating that normal interval as EIO aborts
     * every EP0 descriptor transfer during enumeration. Completion/error is
     * handled from ENDPTCOMPLETE in pjs_usb_transfer_complete(). */
    return PJS_USB_OK;
}

static uint32_t pjs_usb_transfer_count(uint32_t pipe, uint32_t requested)
{
    uint32_t remaining;
    pjs_usb_dma_invalidate(&pjs_usb_td[pipe], sizeof(pjs_usb_td[pipe]));
    remaining = (pjs_usb_td[pipe].size_status & PJS_USB_DTD_REMAIN_MASK) >> 16u;
    if (remaining > requested) {
        return 0u;
    }
    return requested - remaining;
}

static void pjs_usb_config_endpoint(uint32_t endpoint, uint32_t direction,
                                    uint32_t type, uint32_t mps)
{
    uint32_t control = PJS_USB_ENDPTCTRL(endpoint);
    if (direction == PJS_USB_DIR_IN) {
        control &= ~0x000c0000u;
        control |= PJS_USB_EP_TX_ENABLE | PJS_USB_EP_TX_TOGGLE_RST |
                   (type << PJS_USB_EP_TX_TYPE_SHIFT);
        /* CDC replies can end at an exact max-packet boundary (INFO is
         * 128 bytes). Enable hardware ZLP termination on bulk IN so the
         * host completes its read without waiting for a later reply. */
        pjs_usb_qhs[PJS_USB_PIPE(endpoint, direction)].cap =
            (mps << PJS_USB_QH_MPS_SHIFT) |
            (endpoint == PJS_USB_EP_DATA ? 0u : PJS_USB_QH_ZLT);
    } else {
        control &= ~0x0000000cu;
        control |= PJS_USB_EP_RX_ENABLE | PJS_USB_EP_RX_TOGGLE_RST |
                   (type << PJS_USB_EP_RX_TYPE_SHIFT);
        pjs_usb_qhs[PJS_USB_PIPE(endpoint, direction)].cap =
            (mps << PJS_USB_QH_MPS_SHIFT) | PJS_USB_QH_ZLT;
    }
    PJS_USB_ENDPTCTRL(endpoint) = control;
}

static void pjs_usb_stall_control(void)
{
    PJS_USB_ENDPTCTRL(0) |= PJS_USB_EP_TX_STALL | PJS_USB_EP_RX_STALL;
    pjs_usb_ctrl.stage = PJS_USB_CTRL_IDLE;
}

static void pjs_usb_clear_control_stall(void)
{
    PJS_USB_ENDPTCTRL(0) &= ~(PJS_USB_EP_TX_STALL | PJS_USB_EP_RX_STALL);
}

/* USB standard descriptors; all values are little endian on the wire. */
static const uint8_t pjs_usb_device_descriptor[18] = {
    18u, 1u, 0x10u, 0x01u, 0x02u, 0u, 0u, 64u,
    0x09u, 0x12u, 0x4au, 0x50u, 0x00u, 0x01u, 1u, 2u, 3u, 1u
};

static const uint8_t pjs_usb_config_descriptor[67] = {
    9u, 2u, 67u, 0u, 2u, 1u, 0u, 0x80u,
    250u, /* 500 mA once configured; policy holds 100 mA before that. */
    9u, 4u, 0u, 0u, 1u, 2u, 2u, 1u, 0u,
    5u, 0x24u, 0u, 0x10u, 0x01u,
    5u, 0x24u, 1u, 0u, 1u,
    4u, 0x24u, 2u, 2u,
    5u, 0x24u, 6u, 0u, 1u,
    7u, 5u, 0x81u, 3u, 8u, 0u, 16u,
    9u, 4u, 1u, 0u, 2u, 0x0au, 0u, 0u, 0u,
    7u, 5u, 0x82u, 2u, 64u, 0u, 0u,
    7u, 5u, 0x02u, 2u, 64u, 0u, 0u
};

static const uint8_t pjs_usb_string0[4] = { 4u, 3u, 0x09u, 0x04u };
static const uint8_t pjs_usb_string1[18] = {
    18u, 3u, 'P', 0u, 'o', 0u, 'c', 0u, 'k', 0u, 'e', 0u, 't', 0u,
    'J', 0u, 'S', 0u
};
static const uint8_t pjs_usb_string2[30] = {
    30u, 3u, 'P', 0u, 'o', 0u, 'c', 0u, 'k', 0u, 'e', 0u, 't', 0u,
    'J', 0u, 'S', 0u, ' ', 0u, 'A', 0u, '1', 0u, '0', 0u, '9', 0u, '9', 0u
};
static const uint8_t pjs_usb_string3[18] = {
    18u, 3u, 'A', 0u, '1', 0u, '0', 0u, '9', 0u, '9', 0u, '-', 0u, '0', 0u, '1', 0u
};

static const uint8_t *pjs_usb_descriptor(uint8_t kind, uint8_t index,
                                          uint16_t *length)
{
    if (kind == 1u && index == 0u) {
        *length = sizeof(pjs_usb_device_descriptor);
        return pjs_usb_device_descriptor;
    }
    if (kind == 2u && index == 0u) {
        *length = sizeof(pjs_usb_config_descriptor);
        return pjs_usb_config_descriptor;
    }
    if (kind == 3u) {
        if (index == 0u) {
            *length = sizeof(pjs_usb_string0);
            return pjs_usb_string0;
        }
        if (index == 1u) {
            *length = sizeof(pjs_usb_string1);
            return pjs_usb_string1;
        }
        if (index == 2u) {
            *length = sizeof(pjs_usb_string2);
            return pjs_usb_string2;
        }
        if (index == 3u) {
            *length = sizeof(pjs_usb_string3);
            return pjs_usb_string3;
        }
    }
    return NULL;
}

static int pjs_usb_control_in(const uint8_t *data, uint16_t length,
                              uint16_t requested)
{
    uint16_t transfer = length < requested ? length : requested;
    if (transfer > PJS_USB_CTRL_DMA_SIZE) {
        transfer = PJS_USB_CTRL_DMA_SIZE;
    }
    for (uint16_t index = 0u; index < transfer; ++index) {
        pjs_usb_ctrl_dma[index] = data[index];
    }
    pjs_usb_ctrl.expected = transfer;
    pjs_usb_ctrl.actual = 0u;
    pjs_usb_ctrl.stage = PJS_USB_CTRL_DATA_IN;
    return pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN,
                         pjs_usb_ctrl_dma, transfer);
}

static void pjs_usb_notify_queue(void)
{
    if (!pjs_usb_configured || pjs_usb_notify_active || !pjs_usb_notify_pending) {
        return;
    }
    pjs_usb_notification[0] = 0xa1u;
    pjs_usb_notification[1] = 0x20u;
    pjs_usb_notification[2] = 0u;
    pjs_usb_notification[3] = 0u;
    pjs_usb_notification[4] = 0u;
    pjs_usb_notification[5] = 0u;
    pjs_usb_notification[6] = 2u;
    pjs_usb_notification[7] = 0u;
    pjs_usb_notification[8] = (uint8_t)pjs_usb_line_state_value;
    pjs_usb_notification[9] = (uint8_t)(pjs_usb_line_state_value >> 8u);
    if (pjs_usb_prime(PJS_USB_EP_NOTIFY, PJS_USB_DIR_IN,
                     pjs_usb_notification, PJS_USB_NOTIFY_SIZE) == PJS_USB_OK) {
        pjs_usb_notify_active = 1u;
        pjs_usb_notify_pending = 0u;
    }
}

static void pjs_usb_configure_cdc(uint8_t enabled)
{
    pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP_NOTIFY, PJS_USB_DIR_IN));
    pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP_DATA, PJS_USB_DIR_IN));
    pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP_DATA, PJS_USB_DIR_OUT));
    pjs_usb_rx_head = 0u;
    pjs_usb_rx_tail = 0u;
    pjs_usb_tx_head = 0u;
    pjs_usb_tx_tail = 0u;
    pjs_usb_tx_active = 0u;
    pjs_usb_rx_active = 0u;
    pjs_usb_tx_active_flag = 0u;
    pjs_usb_notify_active = 0u;
    pjs_usb_notify_pending = 0u;
    pjs_usb_charging_authorized = 0u;
    ++pjs_usb_epoch;
    pjs_usb_configured = enabled;
    if (enabled) {
        pjs_usb_config_endpoint(PJS_USB_EP_NOTIFY, PJS_USB_DIR_IN,
                                PJS_USB_EP_TYPE_INTERRUPT, 8u);
        pjs_usb_config_endpoint(PJS_USB_EP_DATA, PJS_USB_DIR_IN,
                                PJS_USB_EP_TYPE_BULK, 64u);
        pjs_usb_config_endpoint(PJS_USB_EP_DATA, PJS_USB_DIR_OUT,
                                PJS_USB_EP_TYPE_BULK, 64u);
        if (pjs_usb_prime(PJS_USB_EP_DATA, PJS_USB_DIR_OUT,
                          pjs_usb_rx_dma, PJS_USB_RX_DMA_SIZE) == PJS_USB_OK) {
            pjs_usb_rx_active = 1u;
        }
    }
}

static void pjs_usb_control_setup(void)
{
    uint8_t request_type = pjs_usb_ctrl.setup[0];
    uint8_t request = pjs_usb_ctrl.setup[1];
    uint16_t value = (uint16_t)pjs_usb_ctrl.setup[2] |
                     ((uint16_t)pjs_usb_ctrl.setup[3] << 8u);
    uint16_t index = (uint16_t)pjs_usb_ctrl.setup[4] |
                     ((uint16_t)pjs_usb_ctrl.setup[5] << 8u);
    uint16_t length = (uint16_t)pjs_usb_ctrl.setup[6] |
                      ((uint16_t)pjs_usb_ctrl.setup[7] << 8u);
    uint16_t descriptor_length;
    const uint8_t *descriptor;

    pjs_usb_clear_control_stall();
    pjs_usb_ctrl.pending_address = 0u;
    pjs_usb_ctrl.pending_configuration = 0xffu;
    if ((request_type & 0x60u) == 0u) {
        switch (request) {
        case 0x00u: /* GET_STATUS */
            pjs_usb_ctrl_dma[0] = 0u;
            pjs_usb_ctrl_dma[1] = 0u;
            if ((request_type & 0x1fu) == 2u && index == 0x81u &&
                (PJS_USB_ENDPTCTRL(1) & PJS_USB_EP_TX_STALL) != 0u) {
                pjs_usb_ctrl_dma[0] = 1u;
            }
            if (pjs_usb_control_in(pjs_usb_ctrl_dma, 2u, length) != PJS_USB_OK) {
                pjs_usb_stall_control();
            }
            return;
        case 0x01u: /* CLEAR_FEATURE */
            if ((request_type & 0x1fu) == 2u && value == 0u && index < 0x83u) {
                if ((index & 0x80u) != 0u) {
                    PJS_USB_ENDPTCTRL(index & 0x0fu) &= ~PJS_USB_EP_TX_STALL;
                } else {
                    PJS_USB_ENDPTCTRL(index & 0x0fu) &= ~PJS_USB_EP_RX_STALL;
                }
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
                return;
            }
            break;
        case 0x03u: /* SET_FEATURE */
            if ((request_type & 0x1fu) == 2u && value == 0u && index < 0x83u) {
                if ((index & 0x80u) != 0u) {
                    PJS_USB_ENDPTCTRL(index & 0x0fu) |= PJS_USB_EP_TX_STALL;
                } else {
                    PJS_USB_ENDPTCTRL(index & 0x0fu) |= PJS_USB_EP_RX_STALL;
                }
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
                return;
            }
            break;
        case 0x05u: /* SET_ADDRESS */
            if ((request_type & 0x7fu) == 0u && value < 128u && length == 0u) {
                pjs_usb_ctrl.pending_address = (uint8_t)value;
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
                return;
            }
            break;
        case 0x06u: /* GET_DESCRIPTOR */
            pjs_usb_debug.descriptor_count++;
            descriptor = pjs_usb_descriptor((uint8_t)(value >> 8u),
                                            (uint8_t)value, &descriptor_length);
            if ((request_type & 0x80u) != 0u && descriptor != NULL &&
                pjs_usb_control_in(descriptor, descriptor_length, length) == PJS_USB_OK) {
                return;
            }
            break;
        case 0x08u: /* GET_CONFIGURATION */
            if ((request_type & 0x7fu) == 0u && length != 0u) {
                pjs_usb_ctrl_dma[0] = pjs_usb_configured ? 1u : 0u;
                if (pjs_usb_control_in(pjs_usb_ctrl_dma, 1u, length) !=
                    PJS_USB_OK) {
                    pjs_usb_stall_control();
                }
                return;
            }
            break;
        case 0x09u: /* SET_CONFIGURATION */
            if ((request_type & 0x7fu) == 0u && value <= 1u && length == 0u) {
                /* Configure the non-control endpoints before acknowledging
                 * the request.  In polling mode the host can finish this
                 * status stage and post its next SETUP before software sees
                 * ENDPTCOMPLETE; deferring this action would let that new
                 * SETUP overwrite pending_configuration and leave EP2 OUT
                 * permanently unprimed even though Windows created COMx. */
                pjs_usb_configure_cdc(value == 1u);
                pjs_usb_ctrl.pending_configuration = (uint8_t)value;
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
                return;
            }
            break;
        case 0x0au: /* GET_INTERFACE */
            if ((request_type & 0x7fu) == 1u && length != 0u) {
                pjs_usb_ctrl_dma[0] = 0u;
                if (pjs_usb_control_in(pjs_usb_ctrl_dma, 1u, length) != PJS_USB_OK) {
                    pjs_usb_stall_control();
                }
                return;
            }
            break;
        case 0x0bu: /* SET_INTERFACE */
            if ((request_type & 0x7fu) == 1u && value == 0u && length == 0u) {
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
                return;
            }
            break;
        default:
            break;
        }
    } else if ((request_type & 0x7fu) == 0x21u) {
        /* CDC ACM requests on the communication interface. */
        if (request == 0x21u && length >= 7u) { /* GET_LINE_CODING */
            pjs_usb_ctrl_dma[0] = (uint8_t)pjs_usb_line.baud_rate;
            pjs_usb_ctrl_dma[1] = (uint8_t)(pjs_usb_line.baud_rate >> 8u);
            pjs_usb_ctrl_dma[2] = (uint8_t)(pjs_usb_line.baud_rate >> 16u);
            pjs_usb_ctrl_dma[3] = (uint8_t)(pjs_usb_line.baud_rate >> 24u);
            pjs_usb_ctrl_dma[4] = pjs_usb_line.stop_bits;
            pjs_usb_ctrl_dma[5] = pjs_usb_line.parity;
            pjs_usb_ctrl_dma[6] = pjs_usb_line.data_bits;
            if (pjs_usb_control_in(pjs_usb_ctrl_dma, 7u, length) != PJS_USB_OK) {
                pjs_usb_stall_control();
            }
            return;
        }
        if (request == 0x20u && length == 7u) { /* SET_LINE_CODING */
            pjs_usb_ctrl.expected = 7u;
            pjs_usb_ctrl.stage = PJS_USB_CTRL_DATA_OUT;
            if (pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_OUT,
                              pjs_usb_ctrl_dma, 7u) != PJS_USB_OK) {
                pjs_usb_stall_control();
            }
            return;
        }
        if (request == 0x22u && length == 0u) { /* SET_CONTROL_LINE_STATE */
            pjs_usb_line_state_value = value;
            pjs_usb_notify_pending = 1u;
            pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
            (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
            return;
        }
        if (request == 0x23u && length == 0u) { /* SEND_BREAK */
            pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
            (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
            return;
        }
    }
    pjs_usb_stall_control();
}

static void pjs_usb_apply_control_status(void)
{
    if (pjs_usb_ctrl.pending_address != 0u ||
        ((pjs_usb_ctrl.setup[1] == 5u) &&
         (pjs_usb_ctrl.setup[2] | pjs_usb_ctrl.setup[3]) == 0u)) {
        PJS_USB_DEVICEADDR = (uint32_t)pjs_usb_ctrl.pending_address << PJS_USB_ADDR_SHIFT;
    }
    if (pjs_usb_ctrl.pending_configuration != 0xffu) {
        /* Endpoints were prepared at SETUP. Do not flush/reprime them here:
         * the host may already have queued CDC traffic after the status ACK. */
        pjs_usb_charging_authorized =
            pjs_usb_ctrl.pending_configuration == 1u;
    }
    pjs_usb_ctrl.stage = PJS_USB_CTRL_IDLE;
}

static void pjs_usb_transfer_complete(uint32_t endpoint, uint32_t direction)
{
    uint32_t pipe = PJS_USB_PIPE(endpoint, direction);
    uint32_t count;
    uint32_t dtd_status;
    pjs_usb_debug.completion_count++;
    pjs_usb_debug.last_complete = PJS_USB_PIPE_HW_BIT(pipe);
    pjs_usb_dma_invalidate(&pjs_usb_td[pipe], sizeof(pjs_usb_td[pipe]));
    dtd_status = pjs_usb_td[pipe].size_status;
    if ((dtd_status & PJS_USB_DTD_ERROR) != 0u) {
        pjs_usb_debug_error(2u, dtd_status);
        pjs_usb_flush_pipe(pipe);
        if (endpoint == PJS_USB_EP0) {
            pjs_usb_stall_control();
        } else if (endpoint == PJS_USB_EP_DATA) {
            if (direction == PJS_USB_DIR_OUT) {
                pjs_usb_rx_active = 0u;
            } else {
                pjs_usb_tx_active = 0u;
                pjs_usb_tx_active_flag = 0u;
            }
        }
        return;
    }
    if (endpoint == PJS_USB_EP0) {
        if (pjs_usb_ctrl.stage == PJS_USB_CTRL_DATA_IN && direction == PJS_USB_DIR_IN) {
            pjs_usb_ctrl.actual = (uint16_t)pjs_usb_transfer_count(pipe,
                                                                     pjs_usb_ctrl.expected);
            pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_OUT;
            (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_OUT, NULL, 0u);
        } else if (pjs_usb_ctrl.stage == PJS_USB_CTRL_DATA_OUT &&
                   direction == PJS_USB_DIR_OUT) {
            count = pjs_usb_transfer_count(pipe, pjs_usb_ctrl.expected);
            if (count == 7u && pjs_usb_ctrl.setup[1] == 0x20u) {
                pjs_usb_line.baud_rate = (uint32_t)pjs_usb_ctrl_dma[0] |
                    ((uint32_t)pjs_usb_ctrl_dma[1] << 8u) |
                    ((uint32_t)pjs_usb_ctrl_dma[2] << 16u) |
                    ((uint32_t)pjs_usb_ctrl_dma[3] << 24u);
                pjs_usb_line.stop_bits = pjs_usb_ctrl_dma[4];
                pjs_usb_line.parity = pjs_usb_ctrl_dma[5];
                pjs_usb_line.data_bits = pjs_usb_ctrl_dma[6];
                pjs_usb_ctrl.stage = PJS_USB_CTRL_STATUS_IN;
                (void)pjs_usb_prime(PJS_USB_EP0, PJS_USB_DIR_IN, NULL, 0u);
            } else {
                pjs_usb_stall_control();
            }
        } else if (pjs_usb_ctrl.stage == PJS_USB_CTRL_STATUS_IN &&
                   direction == PJS_USB_DIR_IN) {
            pjs_usb_apply_control_status();
        } else if (pjs_usb_ctrl.stage == PJS_USB_CTRL_STATUS_OUT &&
                   direction == PJS_USB_DIR_OUT) {
            pjs_usb_ctrl.stage = PJS_USB_CTRL_IDLE;
        }
        return;
    }
    if (endpoint == PJS_USB_EP_NOTIFY && direction == PJS_USB_DIR_IN) {
        pjs_usb_notify_active = 0u;
        pjs_usb_notify_queue();
        return;
    }
    if (endpoint == PJS_USB_EP_DATA && direction == PJS_USB_DIR_OUT) {
        count = pjs_usb_transfer_count(pipe, PJS_USB_RX_DMA_SIZE);
        for (uint32_t i = 0u; i < count; ++i) {
            uint16_t next = (uint16_t)((pjs_usb_rx_head + 1u) % PJS_USB_CDC_RX_CAPACITY);
            if (next == pjs_usb_rx_tail) {
                break;
            }
            pjs_usb_rx_ring[pjs_usb_rx_head] = pjs_usb_rx_dma[i];
            pjs_usb_rx_head = next;
        }
        pjs_usb_rx_active = 0u;
        if (pjs_usb_configured && pjs_usb_prime(PJS_USB_EP_DATA, PJS_USB_DIR_OUT,
                                                pjs_usb_rx_dma, PJS_USB_RX_DMA_SIZE) == PJS_USB_OK) {
            pjs_usb_rx_active = 1u;
        }
    } else if (endpoint == PJS_USB_EP_DATA && direction == PJS_USB_DIR_IN) {
        pjs_usb_tx_tail = (uint16_t)((pjs_usb_tx_tail + pjs_usb_tx_active) %
                                     PJS_USB_CDC_TX_CAPACITY);
        pjs_usb_tx_active = 0u;
        pjs_usb_tx_active_flag = 0u;
    }
}

static void pjs_usb_on_reset(void)
{
    pjs_usb_debug.reset_count++;
    PJS_USB_DEVICEADDR = 0u;
    /* Bus reset cancels an in-flight control transfer.  Retire any prime
     * command first, then flush both EP0 directions so a fast status dTD
     * cannot leak into the next SETUP packet. */
    if (pjs_usb_wait_clear(&PJS_USB_ENDPTPRIME, 0xffffffffu) != PJS_USB_OK) {
        pjs_usb_debug_error(5u, PJS_USB_ENDPTPRIME);
        PJS_USB_ENDPTFLUSH = 0xffffffffu;
        (void)pjs_usb_wait_clear(&PJS_USB_ENDPTFLUSH, 0xffffffffu);
    }
    pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP0, PJS_USB_DIR_IN));
    pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP0, PJS_USB_DIR_OUT));
    pjs_usb_configure_cdc(0u);
    pjs_usb_ctrl.stage = PJS_USB_CTRL_IDLE;
    PJS_USB_ENDPTSETUPSTAT = PJS_USB_ENDPTSETUPSTAT;
    PJS_USB_ENDPTCOMPLETE = PJS_USB_ENDPTCOMPLETE;
}

int pjs_usb_device_init(void)
{
    uint32_t index;
    int result;
    pjs_usb_debug.init_count++;
    /* The software default does not prove the inherited GPIO state. Force
     * a known limit before touching the controller or waiting for attach. */
    (void)power_charger_set_mode(PJS_POWER_CHARGER_SUSPEND);
    pjs_usb_charging_authorized = 0u;
    /* A chainloaded image cannot assume the previous firmware held its
     * disconnect long enough for the host. Keep the controller electrically
     * quiet before enabling the PP5020 USB blocks, which also makes handoff
     * robust when the installed sender predates the teardown fix. */
    timer_delay_us(PJS_USB_ATTACH_SETTLE_US);
    /* iPod Photo/Color exposes USB insertion on GPIO D.3. Configure it as a
     * GPIO input before touching the ARC block; this is the same pin setup
     * used by the platform USB path and is needed on a cold standalone boot. */
    PP_GPIOD_OUTPUT_EN &= ~0x08u;
    PP_GPIOD_ENABLE |= 0x08u;
    PP_USB_CLOCK |= 0x03000000u;
    PP_DEV_EN |= PP_DEV_USB0 | PP_DEV_USB1;
    /* The two USB blocks share clock/reset glue.  Rockbox's PP502x bring-up
     * releases them one at a time; a combined write can leave the ARC PHY in
     * reset on some PP5020 revisions. */
    PP_DEV_RS |= PP_DEV_USB0;
    PP_DEV_RS &= ~PP_DEV_USB0;
    PP_DEV_RS |= PP_DEV_USB1;
    PP_DEV_RS &= ~PP_DEV_USB1;
    PP_DEV_INIT2 |= PP_INIT_USB;
    for (index = 0u; index < PJS_USB_HW_WAIT_ITERS; ++index) {
        if ((PP_USB_STATUS & 0x80u) != 0u) {
            break;
        }
    }
    if (index == PJS_USB_HW_WAIT_ITERS) {
        return PJS_USB_ETIMEOUT;
    }
    PP_USB_STATUS |= 0x02u;
    /* Allow the external PHY/clock domain the same 100 ms settle interval as
     * the known PP502x device initialization. */
    timer_delay_us(100000u);
    PP_XMB_RAM_CFG |= 0x47au;

    PJS_USB_USBCMD &= ~PJS_USB_CMD_RUN;
    /* A running controller may still have bus transactions in flight. Give
     * the stop state time to quiesce before asserting controller reset. */
    timer_delay_us(50000u);
    PJS_USB_USBCMD |= PJS_USB_CMD_RESET;
    result = pjs_usb_wait_clear(&PJS_USB_USBCMD, PJS_USB_CMD_RESET);
    if (result != PJS_USB_OK) {
        return result;
    }
    PJS_USB_USBMODE = PJS_USB_MODE_DEVICE | PJS_USB_MODE_STREAM_OFF;
    PJS_USB_PORTSC1 |= PJS_USB_FORCE_FULL_SPEED;
    /* IRAM is not covered by the SDRAM .bss clear and survives a soft
     * handoff. Clear every controller-owned word before publishing the QH
     * table address, including the setup/overlay fields not touched below. */
    volatile uint32_t *usb_workspace =
        (volatile uint32_t *)(uintptr_t)PJS_USB_IRAM_BASE;
    for (index = 0u; index < PJS_USB_WORKSPACE_BYTES / sizeof(uint32_t);
         ++index) {
        usb_workspace[index] = 0u;
    }
    for (index = 0u; index < PJS_USB_QH_COUNT; ++index) {
        pjs_usb_qhs[index].cap = 64u << PJS_USB_QH_MPS_SHIFT;
        pjs_usb_qhs[index].current = 0u;
        pjs_usb_qhs[index].overlay.next = PJS_USB_DTD_TERMINATE;
        pjs_usb_qhs[index].overlay.size_status = 0u;
        pjs_usb_td[index].next = PJS_USB_DTD_TERMINATE;
        pjs_usb_td[index].size_status = 0u;
    }
    pjs_usb_qhs[PJS_USB_PIPE(PJS_USB_EP0, PJS_USB_DIR_OUT)].cap |= PJS_USB_QH_IOS;
    PJS_USB_ENDPOINTLISTADDR = pjs_usb_ptr(pjs_usb_qhs);
    PJS_USB_DEVICEADDR = 0u;
    PJS_USB_ENDPTCTRL(0) = 0u;
    pjs_usb_config_endpoint(PJS_USB_EP0, PJS_USB_DIR_OUT, 0u, 64u);
    pjs_usb_config_endpoint(PJS_USB_EP0, PJS_USB_DIR_IN, 0u, 64u);
    pjs_usb_config_endpoint(PJS_USB_EP_NOTIFY, PJS_USB_DIR_IN,
                            PJS_USB_EP_TYPE_INTERRUPT, 8u);
    pjs_usb_config_endpoint(PJS_USB_EP_DATA, PJS_USB_DIR_IN,
                            PJS_USB_EP_TYPE_BULK, 64u);
    pjs_usb_config_endpoint(PJS_USB_EP_DATA, PJS_USB_DIR_OUT,
                            PJS_USB_EP_TYPE_BULK, 64u);
    pjs_usb_configure_cdc(0u);
    PJS_USB_USBSTS = 0xffffffffu;
    PJS_USB_USBINTR = PJS_USB_INTR_ENABLE;
    PJS_USB_USBCMD |= PJS_USB_CMD_RUN;
    pjs_usb_running = 1u;
    pjs_usb_apply_charger_policy();
    return PJS_USB_OK;
}

void pjs_usb_device_shutdown(void)
{
    /* Leave the LTC4066 suspended even when the controller was already
     * stopped by an earlier failed initialization. */
    if (power_charger_mode() != PJS_POWER_CHARGER_SUSPEND) {
        (void)power_charger_set_mode(PJS_POWER_CHARGER_SUSPEND);
    }
    if (!pjs_usb_running) return;
    PJS_USB_USBINTR = 0u;
    PJS_USB_ENDPTFLUSH = 0xffffffffu;
    (void)pjs_usb_wait_clear(&PJS_USB_ENDPTFLUSH, 0xffffffffu);
    PJS_USB_USBCMD &= ~PJS_USB_CMD_RUN;
    pjs_usb_running = 0u;
    pjs_usb_configured = 0u;
    pjs_usb_charging_authorized = 0u;
    pjs_usb_rx_active = 0u;
    pjs_usb_tx_active_flag = 0u;
    /* Reset and gate the PP5020 blocks so the next owner starts from a real
     * module boundary instead of inheriting an ARC soft state. */
    PJS_USB_USBCMD |= PJS_USB_CMD_RESET;
    (void)pjs_usb_wait_clear(&PJS_USB_USBCMD, PJS_USB_CMD_RESET);
    PP_DEV_RS |= PP_DEV_USB0 | PP_DEV_USB1;
    PP_DEV_INIT2 &= ~PP_INIT_USB;
    PP_DEV_EN &= ~(PP_DEV_USB0 | PP_DEV_USB1);
    /* Keep the pull-up absent long enough for a host to observe a real
     * disconnect before a chainloaded image resets and starts the ARC block.
     * Without this boundary Windows can retire the old COM port but miss the
     * immediate reattach, leaving an otherwise-live image unreachable. */
    timer_delay_us(PJS_USB_DETACH_US);
}

void pjs_usb_device_poll(void)
{
    uint32_t status;
    uint32_t complete;
    uint32_t setup;
    if (!pjs_usb_running) {
        return;
    }
    if (!pjs_usb_connected_raw()) {
        if (pjs_usb_configured || pjs_usb_rx_active || pjs_usb_tx_active_flag) {
            pjs_usb_configure_cdc(0u);
        }
        pjs_usb_apply_charger_policy();
        return;
    }
    status = PJS_USB_USBSTS & PJS_USB_INTR_ENABLE;
    if ((status & PJS_USB_STS_RESET) != 0u) {
        PJS_USB_USBSTS = PJS_USB_STS_RESET;
        pjs_usb_on_reset();
    }
    if ((status & PJS_USB_STS_ERR) != 0u) {
        PJS_USB_USBSTS = PJS_USB_STS_ERR;
        pjs_usb_debug_error(1u, status);
        PJS_USB_ENDPTFLUSH = 0xffffffffu;
        (void)pjs_usb_wait_clear(&PJS_USB_ENDPTFLUSH, 0xffffffffu);
        PJS_USB_ENDPTCOMPLETE = PJS_USB_ENDPTCOMPLETE;
        pjs_usb_rx_active = 0u;
        pjs_usb_tx_active_flag = 0u;
    }
    setup = PJS_USB_ENDPTSETUPSTAT & 1u;
    /* A host is allowed to issue the next SETUP immediately after the status
     * handshake.  At a polling boundary ARC can therefore present both the
     * EP0-IN completion for SET_CONFIGURATION and the next SETUP bit.  Retire
     * that status first; otherwise the new request clears pending_configuration
     * and EP2 OUT is never primed. */
    if (setup != 0u && pjs_usb_ctrl.stage == PJS_USB_CTRL_STATUS_IN &&
        (PJS_USB_ENDPTCOMPLETE & PJS_USB_PIPE_BIT(PJS_USB_EP0,
                                                   PJS_USB_DIR_IN)) != 0u) {
        uint32_t ep0_in = PJS_USB_PIPE_BIT(PJS_USB_EP0, PJS_USB_DIR_IN);
        PJS_USB_ENDPTCOMPLETE = ep0_in;
        pjs_usb_debug.last_complete = ep0_in;
        pjs_usb_transfer_complete(PJS_USB_EP0, PJS_USB_DIR_IN);
    }
    if (setup != 0u) {
        uint32_t setup_lo;
        uint32_t setup_hi;
        pjs_usb_dma_invalidate(pjs_usb_qhs, PJS_USB_QH_BYTES);
        pjs_usb_debug.setup_count++;
        /* USBMODE leaves setup-lockout enabled (SLOM=0), so the ARC holds
         * this QH buffer stable until ENDPTSETUPSTAT is acknowledged. Read
         * both words as a single tripwire-protected snapshot, like Rockbox,
         * rather than issuing eight independent byte loads. */
        setup_lo = pjs_usb_qhs[0].setup[0];
        setup_hi = pjs_usb_qhs[0].setup[1];
        pjs_usb_barrier();
        pjs_usb_ctrl.setup[0] = (uint8_t)setup_lo;
        pjs_usb_ctrl.setup[1] = (uint8_t)(setup_lo >> 8u);
        pjs_usb_ctrl.setup[2] = (uint8_t)(setup_lo >> 16u);
        pjs_usb_ctrl.setup[3] = (uint8_t)(setup_lo >> 24u);
        pjs_usb_ctrl.setup[4] = (uint8_t)setup_hi;
        pjs_usb_ctrl.setup[5] = (uint8_t)(setup_hi >> 8u);
        pjs_usb_ctrl.setup[6] = (uint8_t)(setup_hi >> 16u);
        pjs_usb_ctrl.setup[7] = (uint8_t)(setup_hi >> 24u);
        pjs_usb_debug.last_setup_lo = setup_lo;
        pjs_usb_debug.last_setup_hi = setup_hi;
        PJS_USB_ENDPTSETUPSTAT = setup;
        pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP0, PJS_USB_DIR_IN));
        pjs_usb_flush_pipe(PJS_USB_PIPE(PJS_USB_EP0, PJS_USB_DIR_OUT));
        pjs_usb_control_setup();
    }
    complete = PJS_USB_ENDPTCOMPLETE;
    if (complete != 0u) {
        PJS_USB_ENDPTCOMPLETE = complete;
        pjs_usb_debug.last_complete = complete;
        for (uint32_t endpoint = 0u; endpoint <= PJS_USB_EP_DATA; ++endpoint) {
            if ((complete & PJS_USB_PIPE_BIT(endpoint, PJS_USB_DIR_OUT)) != 0u) {
                pjs_usb_transfer_complete(endpoint, PJS_USB_DIR_OUT);
            }
            if ((complete & PJS_USB_PIPE_BIT(endpoint, PJS_USB_DIR_IN)) != 0u) {
                pjs_usb_transfer_complete(endpoint, PJS_USB_DIR_IN);
            }
        }
    }
    pjs_usb_notify_queue();
    if (pjs_usb_configured && !pjs_usb_rx_active) {
        if (pjs_usb_prime(PJS_USB_EP_DATA, PJS_USB_DIR_OUT,
                          pjs_usb_rx_dma, PJS_USB_RX_DMA_SIZE) == PJS_USB_OK) {
            pjs_usb_rx_active = 1u;
        }
    }
    if (pjs_usb_configured && !pjs_usb_tx_active_flag &&
        pjs_usb_tx_head != pjs_usb_tx_tail) {
        uint16_t available = (uint16_t)((pjs_usb_tx_head + PJS_USB_CDC_TX_CAPACITY -
                                         pjs_usb_tx_tail) % PJS_USB_CDC_TX_CAPACITY);
        uint16_t contiguous = (uint16_t)(PJS_USB_CDC_TX_CAPACITY - pjs_usb_tx_tail);
        uint16_t amount = available < PJS_USB_TX_DMA_SIZE ? available : PJS_USB_TX_DMA_SIZE;
        if (amount > contiguous) {
            amount = contiguous;
        }
        for (uint16_t index = 0u; index < amount; ++index) {
            pjs_usb_tx_dma[index] = pjs_usb_tx_ring[pjs_usb_tx_tail + index];
        }
        if (pjs_usb_prime(PJS_USB_EP_DATA, PJS_USB_DIR_IN,
                          pjs_usb_tx_dma, amount) == PJS_USB_OK) {
            pjs_usb_tx_active = amount;
            pjs_usb_tx_active_flag = 1u;
        }
    }
    pjs_usb_apply_charger_policy();
}

void pjs_usb_device_poll_cooperative(void)
{
    static uint32_t last_poll_us;
    static bool polling;
    uint32_t now = timer_now_us();
    if (polling || !pjs_usb_running ||
        (uint32_t)(now - last_poll_us) < 1000u) return;
    last_poll_us = now;
    polling = true;
    pjs_usb_device_poll();
    polling = false;
}

void pjs_usb_debug_snapshot(PjsUsbDebugSnapshot *out)
{
    if (out != NULL) {
        *out = pjs_usb_debug;
    }
}

int pjs_usb_device_connected(void)
{
    return pjs_usb_running != 0u && pjs_usb_connected_raw();
}

uint32_t pjs_usb_cdc_epoch(void)
{
    return pjs_usb_epoch;
}

int pjs_usb_cdc_configured(void)
{
    return pjs_usb_configured != 0u;
}

int pjs_usb_device_suspended(void)
{
    return pjs_usb_running != 0u &&
           (PJS_USB_PORTSC1 & PJS_USB_PORT_SUSPEND) != 0u;
}

int pjs_usb_cdc_read(uint8_t *destination, size_t capacity)
{
    size_t count = 0u;
    if (destination == NULL && capacity != 0u) {
        return PJS_USB_EINVAL;
    }
    while (count < capacity && pjs_usb_rx_tail != pjs_usb_rx_head) {
        destination[count++] = pjs_usb_rx_ring[pjs_usb_rx_tail];
        pjs_usb_rx_tail = (uint16_t)((pjs_usb_rx_tail + 1u) % PJS_USB_CDC_RX_CAPACITY);
    }
    return (int)count;
}

int pjs_usb_cdc_write(const uint8_t *source, size_t length)
{
    size_t accepted = 0u;
    if (source == NULL && length != 0u) {
        return PJS_USB_EINVAL;
    }
    if (!pjs_usb_configured) {
        return PJS_USB_ENOTREADY;
    }
    while (accepted < length) {
        uint16_t next = (uint16_t)((pjs_usb_tx_head + 1u) % PJS_USB_CDC_TX_CAPACITY);
        if (next == pjs_usb_tx_tail) {
            break;
        }
        pjs_usb_tx_ring[pjs_usb_tx_head] = source[accepted++];
        pjs_usb_tx_head = next;
    }
    return (int)accepted;
}

size_t pjs_usb_cdc_rx_available(void)
{
    return (pjs_usb_rx_head + PJS_USB_CDC_RX_CAPACITY - pjs_usb_rx_tail) %
           PJS_USB_CDC_RX_CAPACITY;
}

size_t pjs_usb_cdc_tx_space(void)
{
    return (pjs_usb_tx_tail + PJS_USB_CDC_TX_CAPACITY - pjs_usb_tx_head - 1u) %
           PJS_USB_CDC_TX_CAPACITY;
}

int pjs_usb_cdc_tx_idle(void)
{
    return pjs_usb_tx_head == pjs_usb_tx_tail &&
           pjs_usb_tx_active_flag == 0u;
}

uint16_t pjs_usb_cdc_line_state(void)
{
    return pjs_usb_line_state_value;
}

void pjs_usb_cdc_get_line_coding(struct pjs_usb_cdc_line_coding *coding)
{
    if (coding != NULL) {
        *coding = pjs_usb_line;
    }
}
