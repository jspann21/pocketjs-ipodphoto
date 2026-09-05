#ifndef POCKETJS_IPOD_PHOTO_PP5020_H
#define POCKETJS_IPOD_PHOTO_PP5020_H

#include <stdint.h>

#define MMIO8(address)  (*(volatile uint8_t *)(uintptr_t)(address))
#define MMIO16(address) (*(volatile uint16_t *)(uintptr_t)(address))
#define MMIO32(address) (*(volatile uint32_t *)(uintptr_t)(address))

/* Core identity and control. */
#define PP_PROCESSOR_ID MMIO32(0x60000000u)
#define PP_CPU_CTL      MMIO32(0x60007000u)
#define PP_COP_CTL      MMIO32(0x60007004u)
#define PP_PROC_SLEEP   0x80000000u
#define PP_DEV_SYSTEM   0x00000004u

/* Interrupt controller. */
#define PP_CPU_INT_STAT      MMIO32(0x60004000u)
#define PP_CPU_INT_EN_STAT   MMIO32(0x60004020u)
#define PP_CPU_INT_EN        MMIO32(0x60004024u)
#define PP_CPU_INT_DIS       MMIO32(0x60004028u)
#define PP_CPU_INT_PRIORITY  MMIO32(0x6000402cu)
#define PP_COP_INT_DIS       MMIO32(0x60004038u)
#define PP_INT_FORCED_CLR    MMIO32(0x6000401cu)
#define PP_CPU_HI_INT_STAT   MMIO32(0x60004100u)
#define PP_CPU_HI_INT_EN     MMIO32(0x60004124u)
#define PP_CPU_HI_INT_DIS    MMIO32(0x60004128u)
#define PP_COP_HI_INT_DIS    MMIO32(0x60004138u)
#define PP_HI_INT_FORCED_CLR MMIO32(0x6000411cu)

/* Timers and clock/reset controls. */
#define PP_TIMER1_CFG MMIO32(0x60005000u)
#define PP_TIMER1_VAL MMIO32(0x60005004u)
#define PP_TIMER2_CFG MMIO32(0x60005008u)
#define PP_TIMER2_VAL MMIO32(0x6000500cu)
#define PP_USEC_TIMER MMIO32(0x60005010u)
#define PP_TIMER1_MASK 0x00000001u
#define PP_TIMER2_MASK 0x00000002u
#define PP_DEV_RS     MMIO32(0x60006004u)
#define PP_DEV_RS2    MMIO32(0x60006008u)
#define PP_DEV_EN     MMIO32(0x6000600cu)
#define PP_DEV_EN2    MMIO32(0x60006010u)
#define PP_DEV_INIT1  MMIO32(0x70000010u)
#define PP_DEV_INIT2  MMIO32(0x70000020u)
#define PP_AUDIO_CLOCK_CTRL MMIO32(0x70000018u)
#define PP_CLOCK_SOURCE MMIO32(0x60006020u)
#define PP_CLCD_CLOCK_SRC MMIO32(0x600060a0u)
#define PP_GPO32_VAL          MMIO32(0x70000080u)
#define PP_GPO32_ENABLE       MMIO32(0x70000084u)
#define PP_GPO32_INPUT_VAL    MMIO32(0x70000088u)
#define PP_GPO32_INPUT_ENABLE MMIO32(0x7000008cu)

#define PP_DEV_I2C   0x00001000u
#define PP_DEV_EXTCLOCKS 0x00000002u
#define PP_DEV_I2S   0x00000800u
#define PP_DEV_USB0  0x00000008u
#define PP_DEV_USB1  0x00400000u
#define PP_DEV_OPTO  0x00010000u
#define PP_DEV_PWM   0x00020000u
#define PP_DEV_LCD   0x04000000u
#define PP_DEV_IDE0  0x02000000u
#define PP_INIT_BUTTONS 0x00040000u
#define PP_INIT_USB     0x80000000u

/* USB clock/PHY gates used by the ARC device controller. */
#define PP_USB_CLOCK    MMIO32(0x7000002cu)
#define PP_USB_STATUS   MMIO32(0x70000028u)
#define PP_XMB_RAM_CFG  MMIO32(0x7000003cu)

/* PP5020 DMA controller. DMA request 2 is the IIS TX pacing request. The
 * channel command transfers packed stereo words (left in the low halfword,
 * right in the high halfword) from uncached SDRAM to IISFIFO_WR. */
#define PP_DMA_IRQ             26u
#define PP_DMA_MASK            (1u << PP_DMA_IRQ)
#define PP_DMA_REQ_IIS         2u
#define PP_DMA_REQ_IIS_MASK    (1u << PP_DMA_REQ_IIS)
#define PP_DMA_MASTER_CONTROL  MMIO32(0x6000a000u)
#define PP_DMA_MASTER_STATUS   MMIO32(0x6000a004u)
#define PP_DMA_REQ_STATUS      MMIO32(0x6000a008u)
#define PP_DMA_MASTER_ENABLE   (1u << 31)

#define PP_DMA0_CMD            MMIO32(0x6000b000u)
#define PP_DMA0_STATUS         MMIO32(0x6000b004u)
#define PP_DMA0_RAM_ADDR       MMIO32(0x6000b010u)
#define PP_DMA0_FLAGS          MMIO32(0x6000b014u)
#define PP_DMA0_PER_ADDR       MMIO32(0x6000b018u)
#define PP_DMA0_INCR           MMIO32(0x6000b01cu)

#define PP_DMA_CMD_SIZE_MASK   0x0000ffffu
#define PP_DMA_CMD_REQ_ID_POS  16u
#define PP_DMA_CMD_WAIT_REQ   (1u << 24)
#define PP_DMA_CMD_SINGLE     (1u << 26)
#define PP_DMA_CMD_RAM_TO_PER (1u << 27)
#define PP_DMA_CMD_INTR       (1u << 30)
#define PP_DMA_CMD_START      (1u << 31)
#define PP_DMA_STATUS_SIZE_MASK 0x0000ffffu
#define PP_DMA_STATUS_INTR    (1u << 30)
#define PP_DMA_STATUS_BUSY    (1u << 31)
#define PP_DMA_FLAGS_UNK26    (1u << 26)
#define PP_DMA_INCR_RANGE_FIXED (1u << 16)
#define PP_DMA_INCR_WIDTH_32BIT (2u << 28)

/* Cache/MMAP. MMAP0 maps SDRAM's native 0x10000000 window to 0x00000000. */
#define PP_CACHE_PRIORITY   MMIO32(0x60006044u)
#define PP_CACHE_CTL        MMIO32(0x6000c000u)
#define PP_CACHE_MASK       MMIO32(0xf000f040u)
#define PP_CACHE_OPERATION  MMIO32(0xf000f044u)
#define PP_CACHE_CTL_ENABLE 0x0001u
#define PP_CACHE_CTL_RUN    0x0002u
#define PP_CACHE_CTL_INIT   0x0004u
#define PP_CACHE_CTL_READY  0x4000u
#define PP_CACHE_CTL_BUSY   0x8000u
#define PP_CACHE_OP_FLUSH   0x0002u
#define PP_CACHE_BYTES      0x00002000u
#define PP_CACHE_LINE_BYTES 16u
#define PP_NOCACHE_BASE     0x10000000u
#define PP_MMAP0_LOGICAL   MMIO32(0xf000f000u)
#define PP_MMAP0_PHYSICAL  MMIO32(0xf000f004u)
#define PP_MMAP_32M_MASK   0x00003e00u
#define PP_MMAP_SDRAM_FLAGS 0x10000f84u

/* GPIO A/B, plus PP502x atomic bitwise aliases (+0x800). */
#define PP_GPIOA_ENABLE     MMIO32(0x6000d000u)
#define PP_GPIOB_ENABLE     MMIO32(0x6000d004u)
#define PP_GPIOA_OUTPUT_EN  MMIO32(0x6000d010u)
#define PP_GPIOB_OUTPUT_EN  MMIO32(0x6000d014u)
#define PP_GPIOA_OUTPUT_VAL MMIO32(0x6000d020u)
#define PP_GPIOB_OUTPUT_VAL MMIO32(0x6000d024u)
#define PP_GPIOA_INPUT_VAL  MMIO32(0x6000d030u)
#define PP_GPIOB_INPUT_VAL  MMIO32(0x6000d034u)
#define PP_GPIOC_INPUT_VAL  MMIO32(0x6000d038u)
#define PP_GPIOD_INPUT_VAL  MMIO32(0x6000d03cu)
#define PP_GPIOD_ENABLE     MMIO32(0x6000d00cu)
#define PP_GPIOD_OUTPUT_EN  MMIO32(0x6000d01cu)

/* IDE0 power and pin-mux controls on the iPod Photo/Color PP5020. The
 * controller must be quiesced before GPIOJ.2 is asserted. */
#define PP_GPIOI_ENABLE     MMIO32(0x6000d100u)
#define PP_GPIOJ_ENABLE     MMIO32(0x6000d104u)
#define PP_GPIOK_ENABLE     MMIO32(0x6000d108u)
#define PP_GPIOI_OUTPUT_EN  MMIO32(0x6000d110u)
#define PP_GPIOJ_OUTPUT_EN  MMIO32(0x6000d114u)
#define PP_GPIOK_OUTPUT_EN  MMIO32(0x6000d118u)
#define PP_GPIOI_OUTPUT_VAL MMIO32(0x6000d120u)
#define PP_GPIOJ_OUTPUT_VAL MMIO32(0x6000d124u)
#define PP_GPIOK_OUTPUT_VAL MMIO32(0x6000d128u)
#define PP_GPIOI_INPUT_VAL  MMIO32(0x6000d130u)
#define PP_GPIOJ_INPUT_VAL  MMIO32(0x6000d134u)
#define PP_GPIOK_INPUT_VAL  MMIO32(0x6000d138u)

#define PP_GPIOA_ENABLE_ATOMIC     MMIO32(0x6000d800u)
#define PP_GPIOB_ENABLE_ATOMIC     MMIO32(0x6000d804u)
#define PP_GPIOA_OUTPUT_EN_ATOMIC  MMIO32(0x6000d810u)
#define PP_GPIOB_OUTPUT_EN_ATOMIC  MMIO32(0x6000d814u)
#define PP_GPIOA_OUTPUT_VAL_ATOMIC MMIO32(0x6000d820u)
#define PP_GPIOB_OUTPUT_VAL_ATOMIC MMIO32(0x6000d824u)
#define PP_GPIOI_OUTPUT_VAL_ATOMIC MMIO32(0x6000d920u)

/* PP5020 IIS audio block. WM8975 is the bus master on the iPod Photo, so
 * the PP5020 side intentionally leaves PP_IIS_MASTER clear. */
#define PP_IISDIV              MMIO32(0x60006080u)
#define PP_IISCONFIG           MMIO32(0x70002800u)
#define PP_IISCLK              MMIO32(0x70002808u)
#define PP_IISFIFO_CFG         MMIO32(0x7000280cu)
#define PP_IISFIFO_WR          MMIO32(0x70002840u)
#define PP_IISFIFO_WR16        MMIO16(0x70002840u)

#define PP_IIS_RESET           (1u << 31)
#define PP_IIS_TXFIFOEN        (1u << 29)
#define PP_IIS_RXFIFOEN        (1u << 28)
#define PP_IIS_MASTER          (1u << 25)
#define PP_IIS_IRQTX           (1u << 1)
#define PP_IIS_IRQRX           (1u << 0)
#define PP_IIS_FORMAT_MASK     (0x3u << 10)
#define PP_IIS_FORMAT_IIS      (0x0u << 10)
#define PP_IIS_SIZE_MASK       (0x3u << 8)
#define PP_IIS_SIZE_16BIT      (0x0u << 8)
#define PP_IIS_FIFO_FORMAT_MASK (0x7u << 4)
#define PP_IIS_FIFO_FORMAT_LE16_2 (0x7u << 4)
#define PP_IIS_RXCLR           (1u << 12)
#define PP_IIS_TXCLR           (1u << 8)
#define PP_IIS_RX_FULL_LVL_12  (0x3u << 4)
#define PP_IIS_TX_EMPTY_LVL_4 (0x1u << 0)
#define PP_IIS_TX_FREE_MASK    (0x3fu << 16)
#define PP_IIS_TX_FREE_COUNT   ((PP_IISFIFO_CFG & PP_IIS_TX_FREE_MASK) >> 16)
#define PP_IIS_TX_IS_EMPTY     (PP_IIS_TX_FREE_COUNT >= 16u)

static inline void pp_gpio_set(volatile uint32_t *atomic_reg, uint32_t mask)
{
    *atomic_reg = (mask << 8) | mask;
}

static inline void pp_gpio_clear(volatile uint32_t *atomic_reg, uint32_t mask)
{
    *atomic_reg = mask << 8;
}

/* Color LCD bridge. */
#define PP_LCD2_PORT         MMIO32(0x70008a0cu)
#define PP_LCD2_BLOCK_CTRL   MMIO32(0x70008a20u)
#define PP_LCD2_BLOCK_CONFIG MMIO32(0x70008a24u)
#define PP_LCD2_BLOCK_DATA   MMIO32(0x70008b00u)
#define PP_LCD2_BUSY_MASK    0x80000000u
#define PP_LCD2_CMD_MASK     0x80000000u
#define PP_LCD2_DATA_MASK    0x81000000u
#define PP_LCD2_BLOCK_READY  0x04000000u
#define PP_LCD2_BLOCK_TXOK   0x01000000u

/* A1099 backlight PWM. */
#define PP_PWM_BACKLIGHT MMIO32(0x7000a010u)

/* PP5020 IDE host and primary ATA task file. Package reads and the running
 * handoff quiesce path use the read-only ALT_STATUS/DATA view; preallocated
 * state writes remain behind their separate source gate. */
#define PP_IDE0_PRI_TIMING0 MMIO32(0xc3000000u)
#define PP_IDE0_PRI_TIMING1 MMIO32(0xc3000004u)
#define PP_IDE0_CFG         MMIO32(0xc3000028u)
#define PP_ATA_DATA         MMIO16(0xc30001e0u)
#define PP_ATA_ERROR        MMIO8(0xc30001e4u)
#define PP_ATA_NSECTOR      MMIO8(0xc30001e8u)
#define PP_ATA_SECTOR       MMIO8(0xc30001ecu)
#define PP_ATA_LCYL         MMIO8(0xc30001f0u)
#define PP_ATA_HCYL         MMIO8(0xc30001f4u)
#define PP_ATA_SELECT       MMIO8(0xc30001f8u)
#define PP_ATA_STATUS       MMIO8(0xc30001fcu)
#define PP_ATA_COMMAND      MMIO8(0xc30001fcu)
#define PP_ATA_ALT_STATUS   MMIO8(0xc30003f8u)
#define PP_ATA_CONTROL      MMIO8(0xc30003f8u)

/* PP5020 I2C controller. Registers are byte-wide and spaced by 4. */
#define PP_I2C_CTRL      MMIO8(0x7000c000u)
#define PP_I2C_ADDR      MMIO8(0x7000c004u)
#define PP_I2C_DATA(index) MMIO8(0x7000c00cu + 4u * (uint32_t)(index))
#define PP_I2C_STATUS    MMIO8(0x7000c01cu)
#define PP_I2C_CLOCK     MMIO32(0x600060a4u)

/* Synaptics/Opto click wheel controller. */
#define PP_WHEEL_CTRL0 MMIO32(0x7000c100u)
#define PP_WHEEL_CTRL1 MMIO32(0x7000c104u)
#define PP_WHEEL_DATA  MMIO32(0x7000c140u)

static inline uint32_t pp_current_core_id(void)
{
    return PP_PROCESSOR_ID & 0xffu;
}

static inline void pp_nop3(void)
{
    __asm__ volatile("nop\n\tnop\n\tnop" ::: "memory");
}

static inline void pp_reboot(void)
{
    PP_CPU_INT_DIS = 0xffffffffu;
    PP_COP_INT_DIS = 0xffffffffu;
    PP_DEV_RS |= PP_DEV_SYSTEM;
    for (;;) {
        __asm__ volatile("nop");
    }
}

static inline void pp_reboot_disk_mode(void)
{
    /* Rockbox's PP5020 iPod boot and USB handoff paths use this exact
     * boot-ROM marker. PP5022 uses a different address; A1099 is PP5020. */
    static const uint8_t marker[21] = {
        'd','i','s','k','m','o','d','e',0,0,
        'h','o','t','s','t','u','f','f',0,0,1
    };
    volatile uint8_t *destination =
        (volatile uint8_t *)(uintptr_t)0x40017f00u;
    for (uint32_t index = 0u; index < sizeof(marker); ++index) {
        destination[index] = marker[index];
    }
    __asm__ volatile("" ::: "memory");
    pp_reboot();
}

#endif
