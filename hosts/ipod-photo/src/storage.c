#include "storage.h"
#include "app_store.h"

#include <stdint.h>

#include "data_fs.h"
#include "heap.h"
#include "package_store.h"
#include "pp5020.h"
#include "timer.h"
#include "usb_device.h"

#define ATA_STATUS_BSY 0x80u
#define ATA_STATUS_RDY 0x40u
#define ATA_STATUS_DF  0x20u
#define ATA_STATUS_DRQ 0x08u
#define ATA_STATUS_ERR 0x01u
#define ATA_SELECT_LBA 0x40u
#define ATA_CMD_READ_SECTORS 0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_FLUSH_CACHE 0xe7u
#define ATA_CMD_STANDBY_IMMEDIATE 0xe0u
#define ATA_TIMEOUT_US 2000000u
#define ATA_POWER_ON_SETTLE_US 100000u
#define ATA_STATE_TRANSACTION_TIMEOUT_US 5000000u


static uint32_t storage_error;
static uint32_t storage_sector_reads;
static uint32_t storage_first_failed_lba = UINT32_MAX;
static uint32_t storage_sector_writes;
static uint32_t storage_sector_flushes;
static uint32_t storage_failed_operation;
static uint32_t storage_failed_status;
static uint32_t storage_failed_error;
static const char guest_directory[11] = {'P','O','C','K','E','T','J','S',' ',' ',' '};
static const char guest_filename[11] = {'A','P','P',' ',' ',' ',' ',' ','P','K','T'};
static const char apps_directory[11] = {'A','P','P','S',' ',' ',' ',' ',' ',' ',' '};
static const char package_extension[3] = {'P','K','T'};
static const char state_filenames[2][11] = {
    {'S','T','A','T','E','0',' ',' ','B','I','N'},
    {'S','T','A','T','E','1',' ',' ','B','I','N'},
};
static uint32_t state_lbas[2];
static bool state_lbas_ready;
static bool ata_transaction_active;
static uint32_t ata_transaction_deadline;
static bool ata_owned;
static bool disk_claimed;
static uint8_t disk_power_state = PJS_STORAGE_DISK_UNKNOWN;
static bool disk_handoff_armed;

static uint32_t package_bank_lbas[PJS_PACKAGE_STORE_BANK_COUNT]
                                  [PJS_PACKAGE_STORE_BANK_SECTORS];
static uint32_t package_bank_sorted[PJS_PACKAGE_STORE_BANK_COUNT]
                                     [PJS_PACKAGE_STORE_BANK_SECTORS];
static uint32_t package_bank_scratch[PJS_PACKAGE_STORE_BANK_SECTORS];
static bool package_bank_ready;

static void ide_power_set(bool enabled);
static void ata_prepare(void);
static bool ata_command_no_data(uint8_t command, uint8_t *status_out);
static bool ata_read_sector(void *context, uint32_t lba,
                            uint8_t sector[PJS_STORAGE_SECTOR_BYTES]);
static bool ata_write_sector_unflushed(
    uint32_t lba, const uint8_t sector[PJS_STORAGE_SECTOR_BYTES]);
static bool ata_flush(void);

static bool package_bank_read(void *context, uint32_t bank, uint32_t sector,
                              uint8_t bytes[PJS_PACKAGE_STORE_SECTOR_BYTES])
{
    (void)context;
    if (!package_bank_ready || bank >= PJS_PACKAGE_STORE_BANK_COUNT ||
        sector >= PJS_PACKAGE_STORE_BANK_SECTORS || bytes == 0) return false;
    ata_prepare();
    return ata_read_sector(0, package_bank_lbas[bank][sector], bytes);
}

static bool package_bank_write(void *context, uint32_t bank, uint32_t sector,
                               const uint8_t bytes[PJS_PACKAGE_STORE_SECTOR_BYTES])
{
    (void)context;
    if (!package_bank_ready || bank >= PJS_PACKAGE_STORE_BANK_COUNT ||
        sector >= PJS_PACKAGE_STORE_BANK_SECTORS || bytes == 0) return false;
    ata_prepare();
    return ata_write_sector_unflushed(
        package_bank_lbas[bank][sector], bytes);
}

static bool package_bank_flush(void *context, uint32_t bank)
{
    (void)context;
    if (!package_bank_ready || bank >= PJS_PACKAGE_STORE_BANK_COUNT) return false;
    ata_prepare();
    return ata_flush();
}

static const PjsPackageStoreIo package_bank_io = {
    .context = 0,
    .read = package_bank_read,
    .write = package_bank_write,
    .flush = package_bank_flush,
};

static bool ata_read_failed(uint32_t lba)
{
    if (storage_first_failed_lba == UINT32_MAX) storage_first_failed_lba = lba;
    return false;
}

static void record_io_failure(uint32_t operation, uint32_t lba)
{
    storage_failed_operation = operation;
    if (lba != UINT32_MAX && storage_first_failed_lba == UINT32_MAX) {
        storage_first_failed_lba = lba;
    }
    storage_failed_status = PP_ATA_ALT_STATUS;
    storage_failed_error =
        (storage_failed_status & ATA_STATUS_ERR) != 0u ? PP_ATA_ERROR : 0u;
}

static bool wait_bsy_clear(void)
{
    uint32_t start = timer_now_us();
    for (;;) {
        pjs_usb_device_poll_cooperative();
        uint32_t now = timer_now_us();
        if ((uint32_t)(now - start) >= ATA_TIMEOUT_US ||
            (ata_transaction_active &&
             (int32_t)(now - ata_transaction_deadline) >= 0)) return false;
        if ((PP_ATA_ALT_STATUS & ATA_STATUS_BSY) == 0u) return true;
    }
}

static bool wait_drq(void)
{
    uint32_t start = timer_now_us();
    for (;;) {
        pjs_usb_device_poll_cooperative();
        uint32_t now = timer_now_us();
        if ((uint32_t)(now - start) >= ATA_TIMEOUT_US ||
            (ata_transaction_active &&
             (int32_t)(now - ata_transaction_deadline) >= 0)) return false;
        uint8_t status = PP_ATA_ALT_STATUS;
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0u) return false;
        if ((status & ATA_STATUS_BSY) == 0u && (status & ATA_STATUS_DRQ) != 0u) {
            return true;
        }
    }
}

static bool completion_ready(uint8_t status)
{
    return (status & ATA_STATUS_RDY) != 0u &&
           (status & (ATA_STATUS_BSY | ATA_STATUS_DF |
                      ATA_STATUS_DRQ | ATA_STATUS_ERR)) == 0u;
}

static bool wait_ready(void)
{
    if (!wait_bsy_clear()) return false;
    return completion_ready(PP_ATA_ALT_STATUS);
}

static void ata_delay_400ns(void)
{
    (void)PP_ATA_ALT_STATUS;
    (void)PP_ATA_ALT_STATUS;
    (void)PP_ATA_ALT_STATUS;
    (void)PP_ATA_ALT_STATUS;
}

static void ide_power_set(bool enabled)
{
    if (enabled) {
        /* GPIOJ.2 is active-low disk power on the Photo/Color board. Route
         * the IDE pins before enabling the host so no bus line is driven while
         * the disk rail is off. */
        PP_GPIOJ_OUTPUT_VAL &= ~0x04u;
        PP_GPIOI_ENABLE &= ~0xbfu;
        PP_GPIOK_ENABLE &= ~0x1fu;
        PP_DEV_EN |= PP_DEV_IDE0;
        timer_delay_us(10u);
        return;
    }

    /* The caller has already completed FLUSH CACHE and STANDBY IMMEDIATE. A
     * reset or a command is never issued after this point. */
    PP_DEV_EN &= ~PP_DEV_IDE0;
    timer_delay_us(10u);
    PP_GPIOI_ENABLE |= 0xbfu;
    PP_GPIOK_ENABLE |= 0x1fu;
    PP_GPIOJ_OUTPUT_VAL |= 0x04u;
}

static void ata_prepare(void)
{
    /* Campaign 3 owns the disk rail explicitly. Earlier gates leave the
     * inherited rail alone and retain their read-only handoff behavior. */
    ide_power_set(true);
    PP_DEV_EN |= PP_DEV_IDE0;
    PP_IDE0_CFG |= (1u << 5);
    PP_IDE0_CFG &= ~0x10000000u;
    PP_IDE0_PRI_TIMING0 = 0x0000c293u;
    PP_IDE0_PRI_TIMING1 = 0x80002150u;
    PP_ATA_CONTROL = 0x02u; /* nIEN: keep ATA IRQ delivery disabled. */
    ata_delay_400ns();
    ata_owned = true;
    disk_claimed = true;
    disk_power_state = PJS_STORAGE_DISK_ON;
}

uint8_t pjs_storage_ata_status_classify(uint8_t status)
{
    /* DRQ wins over BSY so a pending data phase can never be mistaken for an
     * idle device. The handoff path must refuse rather than discard a word. */
    if ((status & ATA_STATUS_DRQ) != 0u) {
        return PJS_STORAGE_ATA_HANDOFF_DATA;
    }
    if ((status & ATA_STATUS_BSY) != 0u) {
        return PJS_STORAGE_ATA_HANDOFF_BUSY;
    }
    if ((status & (ATA_STATUS_DF | ATA_STATUS_ERR)) != 0u) {
        return PJS_STORAGE_ATA_HANDOFF_FAULT;
    }
    if ((status & ATA_STATUS_RDY) != 0u) {
        return PJS_STORAGE_ATA_HANDOFF_READY;
    }
    return PJS_STORAGE_ATA_HANDOFF_UNKNOWN;
}

int pjs_storage_ata_quiesce(PjsStorageDiskHandoff *handoff)
{
    if (handoff == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *handoff = (PjsStorageDiskHandoff){0};
    if (!ata_owned) {
        /* No ATA command has been issued by this image. Avoid touching an
         * inherited controller just to prove that it is idle. */
        handoff->state = PJS_STORAGE_ATA_HANDOFF_NOT_OWNED;
        return PJS_STORAGE_OK;
    }
    if (ata_transaction_active) {
        handoff->state = PJS_STORAGE_ATA_HANDOFF_BUSY;
        return PJS_STORAGE_ERR_ATA;
    }

    uint32_t started = timer_now_us();
    for (;;) {
        uint8_t status = PP_ATA_ALT_STATUS;
        handoff->ata_status = status;
        if (handoff->polls != UINT16_MAX) ++handoff->polls;
        uint8_t state = pjs_storage_ata_status_classify(status);
        if (state == PJS_STORAGE_ATA_HANDOFF_READY) {
            handoff->state = state;
            handoff->elapsed_us = (uint32_t)(timer_now_us() - started);
            return PJS_STORAGE_OK;
        }
        if (state != PJS_STORAGE_ATA_HANDOFF_BUSY) {
            handoff->state = state;
            handoff->elapsed_us = (uint32_t)(timer_now_us() - started);
            return PJS_STORAGE_ERR_ATA;
        }
        uint32_t now = timer_now_us();
        if ((uint32_t)(now - started) >=
            PJS_STORAGE_ATA_QUIESCE_TIMEOUT_US) {
            handoff->state = PJS_STORAGE_ATA_HANDOFF_TIMEOUT;
            handoff->elapsed_us = (uint32_t)(now - started);
            return PJS_STORAGE_ERR_ATA;
        }
    }
}

int pjs_storage_prepare_disk_handoff(PjsStorageDiskHandoff *handoff)
{
    if (handoff == 0) return PJS_STORAGE_ERR_ARGUMENT;
    disk_handoff_armed = false;
    int rc = pjs_storage_ata_quiesce(handoff);
    if (rc != PJS_STORAGE_OK) return rc;
    /* NOT_OWNED is a valid result: the running image has no outstanding ATA
     * operation. The terminal reset remains the caller's responsibility. */
    disk_handoff_armed = true;
    return PJS_STORAGE_OK;
}

void pjs_storage_disk_handoff_clear(void)
{
    disk_handoff_armed = false;
}

bool pjs_storage_disk_handoff_armed(void)
{
    return disk_handoff_armed;
}

int pjs_storage_disk_power_on(void)
{
    if (!disk_claimed) {
        disk_power_state = PJS_STORAGE_DISK_NOT_OWNED;
        ata_owned = false;
        return PJS_STORAGE_OK;
    }
    if (disk_power_state == PJS_STORAGE_DISK_ON) return PJS_STORAGE_OK;
    if (disk_power_state != PJS_STORAGE_DISK_OFF &&
        disk_power_state != PJS_STORAGE_DISK_UNKNOWN &&
        disk_power_state != PJS_STORAGE_DISK_REFUSED &&
        disk_power_state != PJS_STORAGE_DISK_FLUSHED &&
        disk_power_state != PJS_STORAGE_DISK_STANDBY) {
        return PJS_STORAGE_ERR_STATE;
    }
    ata_prepare();
    /* The Photo's disk rail is switched independently of the PP5020 IDE
     * block. Give a stopped spindle a bounded settle window, then require
     * ATA RDY before the caller wakes the LCD or resumes the guest. */
    timer_delay_us(ATA_POWER_ON_SETTLE_US);
    if (!wait_ready()) {
        disk_power_state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_ATA;
    }
    return PJS_STORAGE_OK;
}

static void disk_power_result_init(PjsStorageDiskPower *power)
{
    if (power != 0) *power = (PjsStorageDiskPower){
        .state = disk_power_state,
    };
}

int pjs_storage_disk_flush(PjsStorageDiskPower *power)
{
    if (power == 0) return PJS_STORAGE_ERR_ARGUMENT;
    disk_power_result_init(power);
    if (!disk_claimed || !ata_owned) {
        if (disk_power_state == PJS_STORAGE_DISK_OFF) {
            power->state = PJS_STORAGE_DISK_REFUSED;
            return PJS_STORAGE_ERR_STATE;
        }
        power->state = PJS_STORAGE_DISK_NOT_OWNED;
        disk_power_state = PJS_STORAGE_DISK_NOT_OWNED;
        return PJS_STORAGE_OK;
    }
    if (ata_transaction_active) {
        power->state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_BUSY;
    }
    if (disk_power_state == PJS_STORAGE_DISK_FLUSHED) {
        /* A failed later lifecycle stage must be retryable without issuing a
         * second cache flush against a disk already known to be clean. */
        power->state = PJS_STORAGE_DISK_FLUSHED;
        power->commands = 1u;
        return PJS_STORAGE_OK;
    }
    if (disk_power_state == PJS_STORAGE_DISK_STANDBY ||
        disk_power_state == PJS_STORAGE_DISK_OFF) {
        power->state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_STATE;
    }

    uint32_t started = timer_now_us();
    uint8_t status = 0u;
    if (!ata_command_no_data(ATA_CMD_FLUSH_CACHE, &status)) {
        power->ata_status = status;
        power->elapsed_us = (uint32_t)(timer_now_us() - started);
        power->state = PJS_STORAGE_DISK_REFUSED;
        disk_power_state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_ATA;
    }
    power->ata_status = status;
    power->commands = 1u;
    power->elapsed_us = (uint32_t)(timer_now_us() - started);
    power->state = PJS_STORAGE_DISK_FLUSHED;
    disk_power_state = PJS_STORAGE_DISK_FLUSHED;
    return PJS_STORAGE_OK;
}

int pjs_storage_disk_standby(PjsStorageDiskPower *power)
{
    if (power == 0) return PJS_STORAGE_ERR_ARGUMENT;
    if (disk_power_state == PJS_STORAGE_DISK_ON) {
        int rc = pjs_storage_disk_flush(power);
        if (rc != PJS_STORAGE_OK) return rc;
    } else if (power->state != PJS_STORAGE_DISK_FLUSHED ||
               disk_power_state != PJS_STORAGE_DISK_FLUSHED) {
        disk_power_result_init(power);
    }
    if (!disk_claimed || !ata_owned) {
        power->state = PJS_STORAGE_DISK_NOT_OWNED;
        return PJS_STORAGE_OK;
    }
    if (disk_power_state == PJS_STORAGE_DISK_STANDBY) return PJS_STORAGE_OK;
    if (disk_power_state != PJS_STORAGE_DISK_FLUSHED) {
        power->state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_STATE;
    }
    if (ata_transaction_active) {
        power->state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_BUSY;
    }

    uint32_t started = timer_now_us();
    uint8_t status = 0u;
    if (!ata_command_no_data(ATA_CMD_STANDBY_IMMEDIATE, &status)) {
        power->ata_status = status;
        power->elapsed_us = (uint32_t)(timer_now_us() - started);
        power->state = PJS_STORAGE_DISK_REFUSED;
        disk_power_state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_ATA;
    }
    power->ata_status = status;
    if (power->commands != UINT16_MAX) ++power->commands;
    power->elapsed_us += (uint32_t)(timer_now_us() - started);
    power->state = PJS_STORAGE_DISK_STANDBY;
    disk_power_state = PJS_STORAGE_DISK_STANDBY;
    return PJS_STORAGE_OK;
}

int pjs_storage_disk_power_off(PjsStorageDiskPower *power)
{
    if (power == 0) return PJS_STORAGE_ERR_ARGUMENT;
    if (power->state != PJS_STORAGE_DISK_STANDBY ||
        disk_power_state != PJS_STORAGE_DISK_STANDBY) {
        disk_power_result_init(power);
    } else {
        power->state = disk_power_state;
    }
    if (!disk_claimed) {
        power->state = PJS_STORAGE_DISK_NOT_OWNED;
        disk_power_state = PJS_STORAGE_DISK_NOT_OWNED;
        return PJS_STORAGE_OK;
    }
    if (!ata_owned && disk_power_state == PJS_STORAGE_DISK_OFF) {
        power->state = PJS_STORAGE_DISK_OFF;
        return PJS_STORAGE_OK;
    }
    if (disk_power_state != PJS_STORAGE_DISK_STANDBY) {
        power->state = PJS_STORAGE_DISK_REFUSED;
        return PJS_STORAGE_ERR_STATE;
    }
    ide_power_set(false);
    ata_owned = false;
    disk_handoff_armed = false;
    disk_power_state = PJS_STORAGE_DISK_OFF;
    power->state = PJS_STORAGE_DISK_OFF;
    return PJS_STORAGE_OK;
}

int pjs_storage_disk_flush_standby_off(PjsStorageDiskPower *power)
{
    if (power == 0) return PJS_STORAGE_ERR_ARGUMENT;
    int rc = pjs_storage_disk_flush(power);
    if (rc != PJS_STORAGE_OK) return rc;
    rc = pjs_storage_disk_standby(power);
    if (rc != PJS_STORAGE_OK) return rc;
    return pjs_storage_disk_power_off(power);
}

uint8_t pjs_storage_disk_power_state(void)
{
    return disk_power_state;
}

static bool ata_read_sector(void *context, uint32_t lba,
                            uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    (void)context;
    if (storage_sector_reads != UINT32_MAX) ++storage_sector_reads;
    if (lba >= 0x10000000u || sector == 0) return ata_read_failed(lba);
    if (!wait_bsy_clear()) return ata_read_failed(lba);

    PP_ATA_SELECT = (uint8_t)(ATA_SELECT_LBA | ((lba >> 24) & 0x0fu));
    ata_delay_400ns();
    if (!wait_ready()) return ata_read_failed(lba);

    PP_ATA_NSECTOR = 1u;
    PP_ATA_SECTOR = (uint8_t)lba;
    PP_ATA_LCYL = (uint8_t)(lba >> 8);
    PP_ATA_HCYL = (uint8_t)(lba >> 16);
    PP_ATA_COMMAND = ATA_CMD_READ_SECTORS;
    ata_delay_400ns();
    if (!wait_drq()) return ata_read_failed(lba);

    for (uint32_t word = 0u; word < PJS_STORAGE_SECTOR_BYTES / 2u; ++word) {
        uint16_t value = PP_ATA_DATA;
        sector[word * 2u] = (uint8_t)value;
        sector[word * 2u + 1u] = (uint8_t)(value >> 8);
    }
    if (!wait_bsy_clear()) return ata_read_failed(lba);
    uint8_t status = PP_ATA_STATUS;
    return completion_ready(status) ? true : ata_read_failed(lba);
}

static bool ata_command_no_data(uint8_t command, uint8_t *status_out)
{
    if (!wait_ready()) return false;
    PP_ATA_COMMAND = command;
    ata_delay_400ns();
    if (!wait_bsy_clear()) return false;
    uint8_t status = PP_ATA_STATUS;
    if (status_out != 0) *status_out = status;
    return completion_ready(status);
}

static bool ata_flush(void)
{
    return ata_command_no_data(ATA_CMD_FLUSH_CACHE, 0);
}

static bool ata_write_sector_unflushed(
    uint32_t lba, const uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    if (lba >= 0x10000000u || sector == 0) return ata_read_failed(lba);
    if (!wait_bsy_clear()) return ata_read_failed(lba);
    PP_ATA_SELECT = (uint8_t)(ATA_SELECT_LBA | ((lba >> 24) & 0x0fu));
    ata_delay_400ns();
    if (!wait_ready()) return ata_read_failed(lba);

    PP_ATA_NSECTOR = 1u;
    PP_ATA_SECTOR = (uint8_t)lba;
    PP_ATA_LCYL = (uint8_t)(lba >> 8);
    PP_ATA_HCYL = (uint8_t)(lba >> 16);
    PP_ATA_COMMAND = ATA_CMD_WRITE_SECTORS;
    ata_delay_400ns();
    if (!wait_drq()) return ata_read_failed(lba);
    for (uint32_t word = 0u; word < PJS_STORAGE_SECTOR_BYTES / 2u; ++word) {
        PP_ATA_DATA = (uint16_t)sector[word * 2u] |
                      ((uint16_t)sector[word * 2u + 1u] << 8);
    }
    if (!wait_bsy_clear()) return ata_read_failed(lba);
    uint8_t status = PP_ATA_STATUS;
    return completion_ready(status) ? true : ata_read_failed(lba);
}

static bool ata_write_sector(uint32_t lba,
                             const uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    /* Keep the qualified lineage/state write path byte-for-byte independent
     * from the batched data.fs writer. On PP5020 the completion check and
     * cache flush form one timing-sensitive transaction. */
    if (lba >= 0x10000000u || sector == 0) return ata_read_failed(lba);
    if (!wait_bsy_clear()) return ata_read_failed(lba);
    PP_ATA_SELECT = (uint8_t)(ATA_SELECT_LBA | ((lba >> 24) & 0x0fu));
    ata_delay_400ns();
    if (!wait_ready()) return ata_read_failed(lba);

    PP_ATA_NSECTOR = 1u;
    PP_ATA_SECTOR = (uint8_t)lba;
    PP_ATA_LCYL = (uint8_t)(lba >> 8);
    PP_ATA_HCYL = (uint8_t)(lba >> 16);
    PP_ATA_COMMAND = ATA_CMD_WRITE_SECTORS;
    ata_delay_400ns();
    if (!wait_drq()) return ata_read_failed(lba);
    for (uint32_t word = 0u; word < PJS_STORAGE_SECTOR_BYTES / 2u; ++word) {
        PP_ATA_DATA = (uint16_t)sector[word * 2u] |
                      ((uint16_t)sector[word * 2u + 1u] << 8);
    }
    if (!wait_bsy_clear()) return ata_read_failed(lba);
    uint8_t status = PP_ATA_STATUS;
    if (!completion_ready(status)) return ata_read_failed(lba);
    if (!ata_flush()) return ata_read_failed(lba);
    return true;
}

static bool sectors_equal(const uint8_t *left, const uint8_t *right)
{
    for (uint32_t index = 0u; index < PJS_STORAGE_SECTOR_BYTES; ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

static int resolve_state_lbas(PjsFat32 *fat)
{
    for (uint32_t slot = 0u; slot < 2u; ++slot) {
        int rc = pjs_fat32_short_file_sector(
            fat, guest_directory, state_filenames[slot],
            PJS_STORAGE_SECTOR_BYTES, &state_lbas[slot]);
        if (rc != PJS_STORAGE_OK) return rc;
    }
    if (state_lbas[0] == state_lbas[1]) return PJS_STORAGE_ERR_STATE;
    state_lbas_ready = true;
    return PJS_STORAGE_OK;
}

static bool generation_after(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) > 0;
}

int pjs_storage_state_load(PjsPersistenceState *state)
{
    if (state == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *state = (PjsPersistenceState){0};
    state_lbas_ready = false;
    ata_prepare();
    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc == PJS_STORAGE_OK) rc = resolve_state_lbas(&fat);
    if (rc != PJS_STORAGE_OK) {
        state->error = (uint32_t)(-rc);
        return rc;
    }

    bool valid[2] = {false, false};
    uint32_t generations[2] = {0u, 0u};
    uint32_t payloads[2] = {0u, 0u};
    uint8_t record[PJS_STORAGE_SECTOR_BYTES];
    for (uint32_t slot = 0u; slot < 2u; ++slot) {
        if (!ata_read_sector(0, state_lbas[slot], record)) {
            state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
            return PJS_STORAGE_ERR_ATA;
        }
        valid[slot] = pjs_state_record_read(
            record, &generations[slot], &payloads[slot]);
    }
    if (!valid[0] && !valid[1]) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_STATE);
        return PJS_STORAGE_ERR_STATE;
    }
    uint32_t selected = valid[1] &&
        (!valid[0] || generation_after(generations[1], generations[0])) ? 1u : 0u;
    state->available = 1u;
    state->active_slot = selected;
    state->generation = generations[selected];
    state->payload = payloads[selected];
    return PJS_STORAGE_OK;
}

static int storage_state_write_bounded(PjsPersistenceState *state, bool publish,
                                       uint32_t *attempted_slot,
                                       uint32_t *attempted_generation)
{
    if (state == 0 || attempted_slot == 0 || attempted_generation == 0 ||
        state->available == 0u || !state_lbas_ready) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    uint32_t slot = state->active_slot ^ 1u;
    uint32_t generation = state->generation + 1u;
    *attempted_slot = slot;
    *attempted_generation = generation;

    uint8_t record[PJS_STORAGE_SECTOR_BYTES];
    uint8_t readback[PJS_STORAGE_SECTOR_BYTES];
    pjs_state_record_build(record, generation, generation, false);
    if (!ata_write_sector(state_lbas[slot], record) ||
        !ata_read_sector(0, state_lbas[slot], readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        return PJS_STORAGE_ERR_ATA;
    }
    if (!sectors_equal(record, readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_VERIFY);
        return PJS_STORAGE_ERR_VERIFY;
    }
    if (!publish) return PJS_STORAGE_OK;

    pjs_state_record_build(record, generation, generation, true);
    if (!ata_write_sector(state_lbas[slot], record) ||
        !ata_read_sector(0, state_lbas[slot], readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        return PJS_STORAGE_ERR_ATA;
    }
    uint32_t verified_generation = 0u;
    uint32_t verified_payload = 0u;
    if (!sectors_equal(record, readback) ||
        !pjs_state_record_read(
            readback, &verified_generation, &verified_payload) ||
        verified_generation != generation || verified_payload != generation) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_VERIFY);
        return PJS_STORAGE_ERR_VERIFY;
    }
    state->active_slot = slot;
    state->generation = generation;
    state->payload = generation;
    state->error = 0u;
    return PJS_STORAGE_OK;
}

int pjs_storage_state_write(PjsPersistenceState *state, bool publish,
                            uint32_t *attempted_slot,
                            uint32_t *attempted_generation)
{
    ata_prepare();
    ata_transaction_active = true;
    ata_transaction_deadline = timer_now_us() + ATA_STATE_TRANSACTION_TIMEOUT_US;
    int rc = storage_state_write_bounded(
        state, publish, attempted_slot, attempted_generation);
    if (rc == PJS_STORAGE_OK &&
        (int32_t)(timer_now_us() - ata_transaction_deadline) >= 0) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        rc = PJS_STORAGE_ERR_ATA;
    }
    ata_transaction_active = false;
    return rc;
}

int pjs_storage_lineage_load(PjsLineageState *state)
{
    if (state == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *state = (PjsLineageState){0};
    state_lbas_ready = false;
    ata_prepare();
    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc == PJS_STORAGE_OK) rc = resolve_state_lbas(&fat);
    if (rc != PJS_STORAGE_OK) {
        state->error = (uint32_t)(-rc);
        return rc;
    }

    bool valid[2] = {false, false};
    PjsLineageRecord records[2] = {{0}, {0}};
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    for (uint32_t slot = 0u; slot < 2u; ++slot) {
        if (!ata_read_sector(0, state_lbas[slot], sector)) {
            state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
            return PJS_STORAGE_ERR_ATA;
        }
        valid[slot] = pjs_lineage_record_read(sector, &records[slot]);
    }
    if (!valid[0] && !valid[1]) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_STATE);
        return PJS_STORAGE_ERR_STATE;
    }
    uint32_t selected = valid[1] &&
        (!valid[0] || generation_after(
            records[1].generation, records[0].generation)) ? 1u : 0u;
    state->available = 1u;
    state->active_slot = selected;
    state->record = records[selected];
    return PJS_STORAGE_OK;
}

static int lineage_write_bounded(PjsLineageState *state,
                                 const PjsLineageRecord *requested)
{
    if (state == 0 || requested == 0 || state->available == 0u ||
        !state_lbas_ready) return PJS_STORAGE_ERR_ARGUMENT;
    uint32_t slot = state->active_slot ^ 1u;
    PjsLineageRecord next = *requested;
    next.generation = state->record.generation + 1u;
    uint8_t record[PJS_STORAGE_SECTOR_BYTES];
    uint8_t readback[PJS_STORAGE_SECTOR_BYTES];
    pjs_lineage_record_build(record, &next, false);
    if (!ata_write_sector(state_lbas[slot], record) ||
        !ata_read_sector(0, state_lbas[slot], readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        return PJS_STORAGE_ERR_ATA;
    }
    if (!sectors_equal(record, readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_VERIFY);
        return PJS_STORAGE_ERR_VERIFY;
    }
    pjs_lineage_record_build(record, &next, true);
    if (!ata_write_sector(state_lbas[slot], record) ||
        !ata_read_sector(0, state_lbas[slot], readback)) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        return PJS_STORAGE_ERR_ATA;
    }
    PjsLineageRecord verified = {0};
    if (!sectors_equal(record, readback) ||
        !pjs_lineage_record_read(readback, &verified) ||
        verified.generation != next.generation) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_VERIFY);
        return PJS_STORAGE_ERR_VERIFY;
    }
    state->active_slot = slot;
    state->record = verified;
    state->error = 0u;
    return PJS_STORAGE_OK;
}

int pjs_storage_lineage_write(PjsLineageState *state,
                              const PjsLineageRecord *record)
{
    ata_prepare();
    ata_transaction_active = true;
    ata_transaction_deadline = timer_now_us() + ATA_STATE_TRANSACTION_TIMEOUT_US;
    int rc = lineage_write_bounded(state, record);
    if (rc == PJS_STORAGE_OK &&
        (int32_t)(timer_now_us() - ata_transaction_deadline) >= 0) {
        state->error = (uint32_t)(-PJS_STORAGE_ERR_ATA);
        rc = PJS_STORAGE_ERR_ATA;
    }
    ata_transaction_active = false;
    return rc;
}

void pjs_storage_reset_diagnostics(void)
{
    storage_error = 0u;
    storage_sector_reads = 0u;
    storage_first_failed_lba = UINT32_MAX;
    storage_sector_writes = 0u;
    storage_sector_flushes = 0u;
    storage_failed_operation = 0u;
    storage_failed_status = 0u;
    storage_failed_error = 0u;
}

int pjs_storage_load_guest_named(PjsStorageFile *file,
                                 const char file_name[11])
{
    if (file == 0 || file_name == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *file = (PjsStorageFile){0};
    storage_error = 0u;
    ata_prepare();

    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != PJS_STORAGE_OK) {
        storage_error = (uint32_t)(-rc);
        return rc;
    }

    uint32_t expected = 0u;
    rc = pjs_fat32_short_file_size(&fat, guest_directory, file_name, &expected);
    if (rc != PJS_STORAGE_OK) {
        storage_error = (uint32_t)(-rc);
        return rc;
    }
    if (expected == 0u || expected > PJS_STORAGE_MAX_FILE_BYTES) {
        storage_error = (uint32_t)(-PJS_STORAGE_ERR_TOO_LARGE);
        return PJS_STORAGE_ERR_TOO_LARGE;
    }
    uint8_t *bytes = pjs_heap_alloc(expected, 16u);
    if (bytes == 0) {
        storage_error = (uint32_t)(-PJS_STORAGE_ERR_ALLOC);
        return PJS_STORAGE_ERR_ALLOC;
    }
    uint32_t length = 0u;
    rc = pjs_fat32_read_short_file(&fat, guest_directory, file_name,
                                   bytes, expected, &length);
    if (rc != PJS_STORAGE_OK) {
        pjs_heap_free(bytes);
        storage_error = (uint32_t)(-rc);
        return rc;
    }
    if (length == 0u) {
        pjs_heap_free(bytes);
        storage_error = (uint32_t)(-PJS_STORAGE_ERR_SHORT_READ);
        return PJS_STORAGE_ERR_SHORT_READ;
    }
    file->bytes = bytes;
    file->length = length;
    return PJS_STORAGE_OK;
}

int pjs_storage_load_guest(PjsStorageFile *file)
{
    pjs_storage_reset_diagnostics();
    return pjs_storage_load_guest_named(file, guest_filename);
}

static bool app_name_valid(const char file_name[11])
{
    bool saw_character = false;
    bool saw_space = false;
    for (uint32_t index = 0u; index < 8u; ++index) {
        char character = file_name[index];
        if (character == ' ') {
            saw_space = true;
            continue;
        }
        if (saw_space) return false;
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '_' || character == '-')) return false;
        saw_character = true;
    }
    return saw_character && file_name[8] == 'P' &&
           file_name[9] == 'K' && file_name[10] == 'T';
}

static int compare_app_names(const PjsStorageApp *left,
                             const PjsStorageApp *right)
{
    for (uint32_t index = 0u; index < 11u; ++index) {
        uint8_t a = (uint8_t)left->file_name[index];
        uint8_t b = (uint8_t)right->file_name[index];
        if (a < b) return -1;
        if (a > b) return 1;
    }
    return 0;
}

int pjs_storage_discover_apps(PjsStorageCatalog *catalog)
{
    if (catalog == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *catalog = (PjsStorageCatalog){0};
    storage_error = 0u;
    ata_prepare();

    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != PJS_STORAGE_OK) goto failed;
    uint32_t pocketjs_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        &fat, fat.root_cluster, guest_directory, &pocketjs_cluster);
    if (rc != PJS_STORAGE_OK) goto failed;
    uint32_t apps_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        &fat, pocketjs_cluster, apps_directory, &apps_cluster);
    if (rc != PJS_STORAGE_OK) goto failed;
    rc = pjs_fat32_list_short_files(
        &fat, apps_cluster, package_extension, catalog->apps,
        PJS_STORAGE_MAX_APPS, &catalog->count);
    if (rc != PJS_STORAGE_OK) goto failed;

    uint32_t write = 0u;
    for (uint32_t read = 0u; read < catalog->count; ++read) {
        if (!app_name_valid(catalog->apps[read].file_name) ||
            catalog->apps[read].size > PJS_STORAGE_MAX_FILE_BYTES) continue;
        if (write != read) catalog->apps[write] = catalog->apps[read];
        ++write;
    }
    catalog->count = write;
    rc = pjs_app_store_merge(catalog);
    if (rc != PJS_STORAGE_OK) goto failed;
    for (uint32_t end = catalog->count; end > 1u; --end) {
        for (uint32_t index = 1u; index < end; ++index) {
            if (compare_app_names(&catalog->apps[index - 1u],
                                  &catalog->apps[index]) <= 0) continue;
            PjsStorageApp temporary = catalog->apps[index - 1u];
            catalog->apps[index - 1u] = catalog->apps[index];
            catalog->apps[index] = temporary;
        }
    }
    return PJS_STORAGE_OK;

failed:
    storage_error = (uint32_t)(-rc);
    return rc;
}

int pjs_storage_load_app(PjsStorageFile *file, const char file_name[11])
{
    if (!file || !file_name || !app_name_valid(file_name)) return PJS_STORAGE_ERR_ARGUMENT;
    *file = (PjsStorageFile){0};
    uint32_t slot, state;
    int rc = pjs_app_store_lookup(file_name, &slot, &state);
    if (rc == PJS_STORAGE_OK) {
        if (state != 1u) return PJS_STORAGE_ERR_NOT_FOUND;
        return pjs_app_store_load(file_name, file);
    }
    if (rc != PJS_STORAGE_ERR_NOT_FOUND) return rc;
    return pjs_storage_load_legacy_app(file, file_name);
}

int pjs_storage_load_legacy_app(PjsStorageFile *file, const char file_name[11])
{
    if (file == 0 || file_name == 0 || !app_name_valid(file_name)) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    *file = (PjsStorageFile){0};
    storage_error = 0u;
    ata_prepare();

    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != PJS_STORAGE_OK) goto failed;
    uint32_t pocketjs_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        &fat, fat.root_cluster, guest_directory, &pocketjs_cluster);
    if (rc != PJS_STORAGE_OK) goto failed;
    uint32_t apps_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        &fat, pocketjs_cluster, apps_directory, &apps_cluster);
    if (rc != PJS_STORAGE_OK) goto failed;
    uint32_t expected = 0u;
    rc = pjs_fat32_short_file_size_at(
        &fat, apps_cluster, file_name, &expected);
    if (rc != PJS_STORAGE_OK) goto failed;
    if (expected == 0u || expected > PJS_STORAGE_MAX_FILE_BYTES) {
        rc = PJS_STORAGE_ERR_TOO_LARGE;
        goto failed;
    }
    uint8_t *bytes = pjs_heap_alloc(expected, 16u);
    if (bytes == 0) {
        rc = PJS_STORAGE_ERR_ALLOC;
        goto failed;
    }
    uint32_t length = 0u;
    rc = pjs_fat32_read_short_file_at(
        &fat, apps_cluster, file_name, bytes, expected, &length);
    if (rc != PJS_STORAGE_OK || length == 0u) {
        pjs_heap_free(bytes);
        if (rc == PJS_STORAGE_OK) rc = PJS_STORAGE_ERR_SHORT_READ;
        goto failed;
    }
    file->bytes = bytes;
    file->length = length;
    return PJS_STORAGE_OK;

failed:
    if (rc != PJS_STORAGE_ERR_NOT_FOUND) storage_error = (uint32_t)(-rc);
    return rc;
}

void pjs_storage_release(PjsStorageFile *file)
{
    if (file == 0) return;
    if (file->bytes != 0) pjs_heap_free(file->bytes);
    *file = (PjsStorageFile){0};
}

uint32_t pjs_storage_last_error(void)
{
    return storage_error;
}

uint32_t pjs_storage_sector_read_count(void)
{
    return storage_sector_reads;
}

uint32_t pjs_storage_first_failed_lba(void)
{
    return storage_first_failed_lba;
}

uint32_t pjs_storage_sector_write_count(void)
{
    return storage_sector_writes;
}

uint32_t pjs_storage_sector_flush_count(void)
{
    return storage_sector_flushes;
}

uint32_t pjs_storage_failed_operation(void)
{
    return storage_failed_operation;
}

uint32_t pjs_storage_failed_status(void)
{
    return storage_failed_status;
}

uint32_t pjs_storage_failed_error(void)
{
    return storage_failed_error;
}

static void package_heap_sift_down(uint32_t values[], uint32_t count,
                                   uint32_t root)
{
    if (count < 2u) return;
    while (root <= (count - 2u) / 2u) {
        uint32_t child = root * 2u + 1u;
        if (child + 1u < count && values[child] < values[child + 1u]) ++child;
        if (values[root] >= values[child]) return;
        uint32_t temporary = values[root];
        values[root] = values[child];
        values[child] = temporary;
        root = child;
    }
}

static void package_sort_lbas(uint32_t values[], uint32_t count)
{
    if (count < 2u) return;
    for (uint32_t root = count / 2u; root != 0u; --root)
        package_heap_sift_down(values, count, root - 1u);
    for (uint32_t end = count - 1u; end != 0u; --end) {
        uint32_t temporary = values[0];
        values[0] = values[end];
        values[end] = temporary;
        package_heap_sift_down(values, end, 0u);
    }
}

static bool package_lba_contains(const uint32_t values[], uint32_t count,
                                 uint32_t value)
{
    uint32_t first = 0u;
    uint32_t last = count;
    while (first < last) {
        uint32_t middle = first + (last - first) / 2u;
        if (values[middle] == value) return true;
        if (values[middle] < value) first = middle + 1u;
        else last = middle;
    }
    return false;
}

static bool package_cluster_lba(const PjsFat32 *fat, uint32_t cluster,
                                uint32_t *lba)
{
    if (fat == 0 || lba == 0 || cluster < 2u ||
        cluster - 2u >= fat->cluster_count) return false;
    uint64_t first = (uint64_t)fat->data_lba +
        (uint64_t)(cluster - 2u) * fat->sectors_per_cluster;
    uint64_t end = first + fat->sectors_per_cluster;
    uint64_t partition_end = (uint64_t)fat->partition_lba + fat->partition_sectors;
    if (first > UINT32_MAX || end <= first || end > partition_end) return false;
    *lba = (uint32_t)first;
    return true;
}

static int package_next_cluster(const PjsFat32 *fat, uint32_t cluster,
                                uint32_t *next)
{
    if (fat == 0 || next == 0 || cluster < 2u ||
        cluster - 2u >= fat->cluster_count) return PJS_STORAGE_ERR_CHAIN;
    uint64_t offset = (uint64_t)cluster * 4u;
    uint32_t sector_index = (uint32_t)(offset / PJS_STORAGE_SECTOR_BYTES);
    uint32_t byte_index = (uint32_t)(offset % PJS_STORAGE_SECTOR_BYTES);
    if (sector_index >= fat->fat_sectors || byte_index > 508u) {
        return PJS_STORAGE_ERR_CHAIN;
    }
    uint64_t lba = (uint64_t)fat->fat_lba + sector_index;
    if (lba > UINT32_MAX || lba + 1u >
        (uint64_t)fat->partition_lba + fat->partition_sectors) {
        return PJS_STORAGE_ERR_CHAIN;
    }
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    if (!fat->read_sector(fat->context, (uint32_t)lba, sector))
        return PJS_STORAGE_ERR_ATA;
    uint32_t value = (uint32_t)sector[byte_index] |
        ((uint32_t)sector[byte_index + 1u] << 8) |
        ((uint32_t)sector[byte_index + 2u] << 16) |
        ((uint32_t)sector[byte_index + 3u] << 24);
    value &= 0x0fffffffu;
    if (value == 0x0ffffff7u || value < 2u) return PJS_STORAGE_ERR_CHAIN;
    *next = value;
    return PJS_STORAGE_OK;
}

static int package_find_entry_at(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char name[11], uint32_t *file_cluster,
                                 uint32_t *file_size)
{
    if (fat == 0 || name == 0 || file_cluster == 0 || file_size == 0 ||
        directory_cluster < 2u || directory_cluster - 2u >= fat->cluster_count)
        return PJS_STORAGE_ERR_ARGUMENT;
    uint32_t cluster = directory_cluster;
    uint32_t visited = 0u;
    uint8_t sector[PJS_STORAGE_SECTOR_BYTES];
    for (;;) {
        if (cluster < 2u || cluster - 2u >= fat->cluster_count ||
            visited++ >= fat->cluster_count) return PJS_STORAGE_ERR_CHAIN;
        uint32_t first = 0u;
        if (!package_cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster; ++sec) {
            if (!fat->read_sector(fat->context, first + sec, sector))
                return PJS_STORAGE_ERR_ATA;
            for (uint32_t offset = 0u; offset < PJS_STORAGE_SECTOR_BYTES;
                 offset += 32u) {
                const uint8_t *entry = sector + offset;
                if (entry[0] == 0x00u) return PJS_STORAGE_ERR_NOT_FOUND;
                if (entry[0] == 0xe5u || entry[11] == 0x0fu ||
                    (entry[11] & 0x08u) != 0u) continue;
                bool match = true;
                for (uint32_t index = 0u; index < 11u; ++index)
                    if (entry[index] != (uint8_t)name[index]) match = false;
                if (!match) continue;
                *file_cluster = (((uint32_t)entry[20] |
                    ((uint32_t)entry[21] << 8)) << 16) |
                    (uint32_t)entry[26] | ((uint32_t)entry[27] << 8);
                *file_cluster &= 0x0fffffffu;
                *file_size = (uint32_t)entry[28] |
                    ((uint32_t)entry[29] << 8) |
                    ((uint32_t)entry[30] << 16) |
                    ((uint32_t)entry[31] << 24);
                return PJS_STORAGE_OK;
            }
        }
        uint32_t next = 0u;
        int rc = package_next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (next >= 0x0ffffff8u) return PJS_STORAGE_ERR_NOT_FOUND;
        cluster = next;
    }
}

static int package_resolve_at(PjsFat32 *fat, uint32_t directory_cluster,
                              const char name[11], uint32_t expected_size,
                              uint32_t output[PJS_PACKAGE_STORE_BANK_SECTORS])
{
    if (fat == 0 || name == 0 || output == 0 || expected_size == 0u ||
        expected_size > PJS_PACKAGE_STORE_PAYLOAD_LIMIT +
                        PJS_STORAGE_SECTOR_BYTES) return PJS_STORAGE_ERR_ARGUMENT;
    uint32_t cluster = 0u;
    uint32_t size = 0u;
    int rc = package_find_entry_at(fat, directory_cluster, name, &cluster, &size);
    if (rc != PJS_STORAGE_OK) return rc;
    if (size != expected_size || cluster < 2u ||
        cluster - 2u >= fat->cluster_count) return PJS_STORAGE_ERR_STATE;
    uint32_t needed = (expected_size + PJS_STORAGE_SECTOR_BYTES - 1u) /
                      PJS_STORAGE_SECTOR_BYTES;
    if (needed == 0u || needed > PJS_PACKAGE_STORE_BANK_SECTORS)
        return PJS_STORAGE_ERR_TOO_LARGE;
    uint32_t written = 0u;
    uint32_t visited = 0u;
    while (written < needed) {
        if (cluster < 2u || cluster - 2u >= fat->cluster_count ||
            cluster == fat->root_cluster || cluster == directory_cluster ||
            visited++ >= fat->cluster_count) return PJS_STORAGE_ERR_CHAIN;
        uint32_t first = 0u;
        if (!package_cluster_lba(fat, cluster, &first)) return PJS_STORAGE_ERR_CHAIN;
        for (uint32_t sec = 0u; sec < fat->sectors_per_cluster &&
             written < needed; ++sec) output[written++] = first + sec;
        uint32_t next = 0u;
        rc = package_next_cluster(fat, cluster, &next);
        if (rc != PJS_STORAGE_OK) return rc;
        if (written < needed && next >= 0x0ffffff8u)
            return PJS_STORAGE_ERR_SHORT_READ;
        if (written == needed && next < 0x0ffffff8u)
            return PJS_STORAGE_ERR_CHAIN;
        cluster = next;
    }
    return PJS_STORAGE_OK;
}

static int package_protect_chain(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char name[11], uint32_t expected_size,
                                 bool optional)
{
    int rc = package_resolve_at(fat, directory_cluster, name, expected_size,
                                package_bank_scratch);
    if (optional && rc == PJS_STORAGE_ERR_NOT_FOUND) return PJS_STORAGE_OK;
    if (rc != PJS_STORAGE_OK) return rc;
    uint32_t count = (expected_size + PJS_STORAGE_SECTOR_BYTES - 1u) /
                     PJS_STORAGE_SECTOR_BYTES;
    for (uint32_t index = 0u; index < count; ++index) {
        if (package_lba_contains(package_bank_sorted[0],
                                 PJS_PACKAGE_STORE_BANK_SECTORS,
                                 package_bank_scratch[index]) ||
            package_lba_contains(package_bank_sorted[1],
                                 PJS_PACKAGE_STORE_BANK_SECTORS,
                                 package_bank_scratch[index]))
            return PJS_STORAGE_ERR_VERIFY;
    }
    return PJS_STORAGE_OK;
}

static int package_protect_named(PjsFat32 *fat, uint32_t directory_cluster,
                                 const char name[11], bool optional)
{
    uint32_t size = 0u;
    int rc = pjs_fat32_short_file_size_at(
        fat, directory_cluster, name, &size);
    if (optional && rc == PJS_STORAGE_ERR_NOT_FOUND) return PJS_STORAGE_OK;
    if (rc != PJS_STORAGE_OK) return rc;
    return package_protect_chain(fat, directory_cluster, name, size, false);
}

static int package_validate_protected(PjsFat32 *fat, uint32_t pocketjs_cluster)
{
    static const char fs0[11] = {
        'F','S','B','A','N','K','0',' ','B','I','N',
    };
    static const char fs1[11] = {
        'F','S','B','A','N','K','1',' ','B','I','N',
    };
    static const char state0[11] = {
        'S','T','A','T','E','0',' ',' ','B','I','N',
    };
    static const char state1[11] = {
        'S','T','A','T','E','1',' ',' ','B','I','N',
    };
    static const char fixed[5][11] = {
        {'A','P','P',' ',' ',' ',' ',' ','P','K','T'},
        {'P','E','N','D','I','N','G',' ','P','K','T'},
        {'A','C','T','I','V','E',' ',' ','P','K','T'},
        {'L','A','S','T','G','O','O','D','P','K','T'},
        {'L','A','U','N','C','H','E','R','P','K','T'},
    };
    int rc = package_protect_chain(
        fat, pocketjs_cluster, fs0,
        PJS_DATA_FS_BANK_SECTORS * PJS_STORAGE_SECTOR_BYTES, false);
    if (rc != PJS_STORAGE_OK) return rc;
    rc = package_protect_chain(
        fat, pocketjs_cluster, fs1,
        PJS_DATA_FS_BANK_SECTORS * PJS_STORAGE_SECTOR_BYTES, false);
    if (rc != PJS_STORAGE_OK) return rc;
    rc = package_protect_chain(fat, pocketjs_cluster, state0,
                               PJS_STORAGE_SECTOR_BYTES, false);
    if (rc != PJS_STORAGE_OK) return rc;
    rc = package_protect_chain(fat, pocketjs_cluster, state1,
                               PJS_STORAGE_SECTOR_BYTES, false);
    if (rc != PJS_STORAGE_OK) return rc;
    for (uint32_t index = 0u; index < 5u; ++index) {
        rc = package_protect_named(fat, pocketjs_cluster, fixed[index], true);
        if (rc != PJS_STORAGE_OK) return rc;
    }

    uint32_t apps_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        fat, pocketjs_cluster, apps_directory, &apps_cluster);
    if (rc == PJS_STORAGE_ERR_NOT_FOUND) return PJS_STORAGE_OK;
    if (rc != PJS_STORAGE_OK) return rc;
    PjsStorageApp apps[PJS_STORAGE_MAX_APPS] = {0};
    uint32_t count = 0u;
    rc = pjs_fat32_list_short_files(
        fat, apps_cluster, package_extension, apps,
        PJS_STORAGE_MAX_APPS, &count);
    if (rc != PJS_STORAGE_OK) return rc;
    for (uint32_t index = 0u; index < count; ++index) {
        if (apps[index].size == 0u ||
            apps[index].size > PJS_PACKAGE_STORE_PAYLOAD_LIMIT +
                                PJS_STORAGE_SECTOR_BYTES)
            return PJS_STORAGE_ERR_TOO_LARGE;
        rc = package_protect_chain(fat, apps_cluster, apps[index].file_name,
                                   apps[index].size, false);
        if (rc != PJS_STORAGE_OK) return rc;
    }

    /* The installed rockbox.ipod and its long-name handoff backup are outside
     * this short-name resolver. The host bootstrap must prove they do not
     * cross-link package banks before provisioning them. */
    return PJS_STORAGE_OK;
}

int pjs_storage_package_bank_init(void)
{
    package_bank_ready = false;
    ata_prepare();
    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != PJS_STORAGE_OK) return rc;
    uint32_t pocketjs_cluster = 0u;
    rc = pjs_fat32_find_short_directory(
        &fat, fat.root_cluster, guest_directory, &pocketjs_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    static const char bank_names[2][11] = {
        {'P','K','G','B','A','N','K','0','B','I','N'},
        {'P','K','G','B','A','N','K','1','B','I','N'},
    };
    for (uint32_t bank = 0u; bank < PJS_PACKAGE_STORE_BANK_COUNT; ++bank) {
        rc = package_resolve_at(
            &fat, pocketjs_cluster, bank_names[bank],
            PJS_PACKAGE_STORE_BANK_SECTORS * PJS_STORAGE_SECTOR_BYTES,
            package_bank_lbas[bank]);
        if (rc != PJS_STORAGE_OK) return rc;
        for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_BANK_SECTORS; ++index)
            package_bank_sorted[bank][index] = package_bank_lbas[bank][index];
        package_sort_lbas(package_bank_sorted[bank],
                          PJS_PACKAGE_STORE_BANK_SECTORS);
        for (uint32_t index = 1u; index < PJS_PACKAGE_STORE_BANK_SECTORS; ++index)
            if (package_bank_sorted[bank][index] ==
                package_bank_sorted[bank][index - 1u]) return PJS_STORAGE_ERR_VERIFY;
    }
    for (uint32_t index = 0u; index < PJS_PACKAGE_STORE_BANK_SECTORS; ++index)
        if (package_lba_contains(package_bank_sorted[0],
                                 PJS_PACKAGE_STORE_BANK_SECTORS,
                                 package_bank_sorted[1][index]))
            return PJS_STORAGE_ERR_VERIFY;
    rc = package_validate_protected(&fat, pocketjs_cluster);
    if (rc != PJS_STORAGE_OK) return rc;
    package_bank_ready = true;
    return PJS_STORAGE_OK;
}

int pjs_storage_package_bank_inspect(uint32_t bank,
                                     PjsPackageStoreHeader *header)
{
    if (!package_bank_ready || header == 0) return PJS_STORAGE_ERR_ARGUMENT;
    int rc = pjs_package_store_inspect_header(&package_bank_io, bank, header);
    if (rc == PJS_PACKAGE_STORE_ERR_IO) return PJS_STORAGE_ERR_ATA;
    return rc == PJS_PACKAGE_STORE_OK ? PJS_STORAGE_OK : PJS_STORAGE_ERR_VERIFY;
}

int pjs_storage_load_package_bank(uint32_t bank, uint32_t expected_hash_low,
                                  uint32_t expected_hash_high,
                                  PjsStorageFile *file,
                                  PjsPackageStoreHeader *header)
{
    if (file == 0) return PJS_STORAGE_ERR_ARGUMENT;
    *file = (PjsStorageFile){0};
    if (!package_bank_ready) return PJS_STORAGE_ERR_NOT_OWNED;
    PjsPackageStoreHeader inspected = {0};
    int rc = pjs_storage_package_bank_inspect(bank, &inspected);
    if (rc != PJS_STORAGE_OK) return rc;
    uint8_t *bytes = pjs_heap_alloc(inspected.payload_length, 16u);
    if (bytes == 0) return PJS_STORAGE_ERR_ALLOC;
    rc = pjs_package_store_load_exact(
        &package_bank_io, bank, expected_hash_low, expected_hash_high,
        bytes, inspected.payload_length, &inspected);
    if (rc != PJS_PACKAGE_STORE_OK) {
        pjs_heap_free(bytes);
        return PJS_STORAGE_ERR_VERIFY;
    }
    file->bytes = bytes;
    file->length = inspected.payload_length;
    if (header != 0) *header = inspected;
    return PJS_STORAGE_OK;
}

int pjs_storage_stage_package_bank(uint32_t bank, uint32_t generation,
                                   uint32_t expected_payload_crc,
                                   uint32_t hash_low, uint32_t hash_high,
                                   const uint8_t *payload,
                                   uint32_t payload_length)
{
    if (!package_bank_ready) return PJS_STORAGE_ERR_NOT_OWNED;
    int rc = pjs_package_store_stage(
        &package_bank_io, bank, generation, expected_payload_crc,
        hash_low, hash_high, payload, payload_length);
    return rc == PJS_PACKAGE_STORE_OK ? PJS_STORAGE_OK : PJS_STORAGE_ERR_VERIFY;
}


bool pjs_storage_sector_read(void *context, uint32_t lba,
                             uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    (void)context;
    ata_prepare();
    bool ok = ata_read_sector(0, lba, sector);
    if (!ok) record_io_failure(1u, lba);
    return ok;
}

bool pjs_storage_sector_write(void *context, uint32_t lba,
                              const uint8_t sector[PJS_STORAGE_SECTOR_BYTES])
{
    (void)context;
    ata_prepare();
    if (storage_sector_writes != UINT32_MAX) ++storage_sector_writes;
    bool ok = ata_write_sector_unflushed(lba, sector);
    if (!ok) record_io_failure(2u, lba);
    return ok;
}

bool pjs_storage_sector_flush(void *context)
{
    (void)context;
    ata_prepare();
    if (storage_sector_flushes != UINT32_MAX) ++storage_sector_flushes;
    bool ok = ata_flush();
    if (!ok) record_io_failure(3u, UINT32_MAX);
    return ok;
}

int pjs_storage_resolve_short_file(void *context, const char file_name[11],
                                    uint32_t expected_sectors,
                                    uint32_t lba_out[PJS_STORAGE_MAX_FILE_SECTORS])
{
    (void)context;
    if (file_name == 0 || lba_out == 0) return PJS_STORAGE_ERR_ARGUMENT;
    ata_prepare();
    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != PJS_STORAGE_OK) return rc;
    static const char directory[11] = {
        'P','O','C','K','E','T','J','S',' ',' ',' ',
    };
    if (expected_sectors == 0u ||
        expected_sectors > PJS_STORAGE_MAX_FILE_SECTORS) {
        return PJS_STORAGE_ERR_ARGUMENT;
    }
    return pjs_fat32_short_file_sectors(
        &fat, directory, file_name,
        expected_sectors * PJS_STORAGE_SECTOR_BYTES, lba_out);
}

/* Managed files are allocated by the host while FAT is mounted by Windows.
 * Native operations change only resolved data sectors, never FAT metadata. */
static uint32_t app_pair_lbas[2][PJS_PACKAGE_STORE_BANK_SECTORS];
static uint32_t app_pair_sectors;
static bool app_pair_writable;

static void app_pair_name(uint32_t slot, uint32_t bank, char name[11])
{
    static const char base[11] = {'A','P','P','0','0','A',' ',' ','B','I','N'};
    for (uint32_t i = 0; i < 11u; ++i) name[i] = base[i];
    if (slot == PJS_APP_STORE_INDEX_SLOT) {
        name[3] = 'I'; name[4] = 'D'; name[5] = 'X'; name[6] = (char)('0' + bank);
    } else {
        name[3] = (char)('0' + slot / 10u); name[4] = (char)('0' + slot % 10u);
        name[5] = (char)('A' + bank);
    }
}

static bool app_pair_read(void *context, uint32_t bank, uint32_t sector, uint8_t bytes[512])
{
    (void)context;
    return bank < 2u && sector < app_pair_sectors &&
        pjs_storage_sector_read(0, app_pair_lbas[bank][sector], bytes);
}
static bool app_pair_write(void *context, uint32_t bank, uint32_t sector, const uint8_t bytes[512])
{
    (void)context;
    return app_pair_writable && bank < 2u && sector < app_pair_sectors &&
        pjs_storage_sector_write(0, app_pair_lbas[bank][sector], bytes);
}
static bool app_pair_flush(void *context, uint32_t bank)
{
    (void)context;
    return app_pair_writable && bank < 2u && pjs_storage_sector_flush(0);
}
static const PjsPackageStoreIo app_pair_io = {
    .read = app_pair_read, .write = app_pair_write, .flush = app_pair_flush,
};

int pjs_storage_app_pair(uint32_t slot, bool writable, const PjsPackageStoreIo **io)
{
    app_pair_sectors = 0u;
    app_pair_writable = false;
    if (slot > PJS_APP_STORE_INDEX_SLOT || !io) return PJS_STORAGE_ERR_ARGUMENT;
    ata_prepare();
    PjsFat32 fat = {0};
    int rc = pjs_fat32_mount(&fat, ata_read_sector, 0);
    if (rc != 0) return rc;
    uint32_t directory;
    rc = pjs_fat32_find_short_directory(&fat, fat.root_cluster, guest_directory, &directory);
    if (rc != 0) return rc;
    uint32_t sectors = slot == PJS_APP_STORE_INDEX_SLOT ?
        PJS_APP_STORE_INDEX_BYTES / 512u : PJS_PACKAGE_STORE_BANK_SECTORS;
    for (uint32_t bank = 0; bank < 2u; ++bank) {
        char name[11];
        app_pair_name(slot, bank, name);
        rc = package_resolve_at(&fat, directory, name, sectors * 512u, app_pair_lbas[bank]);
        if (rc != 0) return rc;
        if (!writable) continue;
        /* Reuse the package-store verification workspace. Its map is not
         * the managed I/O map and no mounted legacy bank is redirected. */
        for (uint32_t i = 0; i < PJS_PACKAGE_STORE_BANK_SECTORS; ++i)
            package_bank_sorted[bank][i] = i < sectors ? app_pair_lbas[bank][i] : UINT32_MAX;
        package_sort_lbas(package_bank_sorted[bank], PJS_PACKAGE_STORE_BANK_SECTORS);
        for (uint32_t i = 1; i < sectors; ++i)
            if (package_bank_sorted[bank][i] == package_bank_sorted[bank][i - 1u])
                return PJS_STORAGE_ERR_VERIFY;
    }
    if (writable) {
        for (uint32_t i = 0; i < sectors; ++i)
            if (package_lba_contains(package_bank_sorted[0], sectors, package_bank_sorted[1][i]))
                return PJS_STORAGE_ERR_VERIFY;
        rc = package_validate_protected(&fat, directory);
        if (rc != 0) return rc;
        for (uint32_t other = 0; other <= PJS_APP_STORE_INDEX_SLOT; ++other) {
            if (other == slot) continue;
            for (uint32_t bank = 0; bank < 2u; ++bank) {
                char name[11]; app_pair_name(other, bank, name);
                rc = package_protect_chain(&fat, directory, name,
                    other == PJS_APP_STORE_INDEX_SLOT ? PJS_APP_STORE_INDEX_BYTES :
                    PJS_PACKAGE_STORE_BANK_SECTORS * 512u, false);
                if (rc != 0) return rc;
            }
        }
        static const char legacy_banks[2][11] = {
            {'P','K','G','B','A','N','K','0','B','I','N'},
            {'P','K','G','B','A','N','K','1','B','I','N'},
        };
        for (uint32_t bank = 0; bank < 2u; ++bank) {
            rc = package_protect_named(&fat, directory, legacy_banks[bank], true);
            if (rc != 0) return rc;
        }
    }
    app_pair_sectors = sectors;
    app_pair_writable = writable;
    *io = &app_pair_io;
    return 0;
}
