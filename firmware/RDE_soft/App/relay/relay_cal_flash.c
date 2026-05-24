/*
 * relay_cal_flash.c – relay calibration stored in STM32G4 flash (page 62)
 *
 * Drop-in replacement for relay_cal.c (FRAM backend).
 * To switch backends, exclude ONE file from the CubeIDE build:
 *   Right-click → Resource Configurations → Exclude from Build
 *
 *   Active (flash):  relay_cal_flash.c included, relay_cal.c excluded
 *   Active (FRAM):   relay_cal.c included, relay_cal_flash.c excluded
 *
 * Flash page 62: start 0x0801F000, size 2 KB
 * NOTE: page 63 (0x0801F800) is used by net_config.c (flash backend).
 *       Ensure the linker does not place code past 0x0801EFFF.
 *
 * Layout (256 bytes at start of page 62):
 *   0x0000  cal_data_t   (240 B)
 *   0x00F0  CRC32 data   (4 B)
 *   0x00F4  cal_config_t (8 B)
 *   0x00FC  CRC32 cfg    (4 B)
 */

#include "relay_cal.h"
#include "relay_ctrl.h"
#include "../fram/fm24c64b.h"   /* fm24_crc32() uses hardware CRC, independent of I2C */
#include "stm32g4xx_hal.h"
#include <string.h>

#define CAL_FLASH_PAGE   62u
#define CAL_FLASH_ADDR   0x0801F000u
#define CAL_FLASH_BANK   FLASH_BANK_1

typedef struct {
    cal_data_t   data;       /* 240 bytes */
    uint32_t     data_crc;   /* 4 bytes   */
    cal_config_t cfg;        /* 8 bytes   */
    uint32_t     cfg_crc;    /* 4 bytes   */
} cal_flash_block_t;         /* 256 bytes total, 32 × 8-byte double-words */

_Static_assert(sizeof(cal_flash_block_t) % 8 == 0,
    "cal_flash_block_t must be a multiple of 8 bytes for STM32G4 flash programming");

/* Per-decade unit multipliers: index 0 = 1Ω decade, index 5 = 100kΩ decade */
static const uint32_t k_multiplier[CAL_DECADES] = {1, 10, 100, 1000, 10000, 100000};

static cal_data_t   g_cal;
static cal_config_t g_cfg;
static uint8_t      g_loaded;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void fill_nominal(cal_data_t *d)
{
    for (uint8_t dec = 0; dec < CAL_DECADES; dec++)
        for (uint8_t dig = 0; dig < CAL_DIGITS; dig++)
            d->milliohm[dec][dig] = (uint32_t)dig * k_multiplier[dec] * 1000u;
}

static uint8_t flash_write_block(const cal_flash_block_t *blk)
{
    HAL_FLASH_Unlock();

    /* Clear any stale error flags (OPTVERR etc.) that would cause
     * FLASH_WaitForLastOperation to return HAL_ERROR immediately */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_SR_ERRORS);

    FLASH_EraseInitTypeDef er = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = CAL_FLASH_BANK,
        .Page      = CAL_FLASH_PAGE,
        .NbPages   = 1u,
    };
    uint32_t err_page = 0;
    if (HAL_FLASHEx_Erase(&er, &err_page) != HAL_OK) {
        HAL_FLASH_Lock();
        return 0u;
    }

    const uint64_t *src = (const uint64_t *)blk;
    uint32_t addr = CAL_FLASH_ADDR;
    for (uint16_t i = 0; i < sizeof(cal_flash_block_t) / 8u; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, src[i]) != HAL_OK) {
            HAL_FLASH_Lock();
            return 0u;
        }
        addr += 8u;
    }

    HAL_FLASH_Lock();
    return 1u;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void relay_cal_init(void)
{
    const cal_flash_block_t *blk = (const cal_flash_block_t *)CAL_FLASH_ADDR;
    uint8_t data_ok = 0;
    uint8_t cfg_ok  = 0;

    if (fm24_crc32(&blk->data, sizeof(cal_data_t)) == blk->data_crc) {
        memcpy(&g_cal, &blk->data, sizeof(cal_data_t));
        data_ok = 1;
    }
    if (fm24_crc32(&blk->cfg, sizeof(cal_config_t)) == blk->cfg_crc &&
        blk->cfg.magic == CAL_MAGIC) {
        memcpy(&g_cfg, &blk->cfg, sizeof(cal_config_t));
        cfg_ok = 1;
    }

    if (!data_ok) fill_nominal(&g_cal);
    if (!cfg_ok) {
        g_cfg.magic   = CAL_MAGIC;
        g_cfg.enabled = 0;
        memset(g_cfg._pad, 0, sizeof(g_cfg._pad));
    }

    if (!data_ok || !cfg_ok) {
        cal_flash_block_t fresh;
        memcpy(&fresh.data, &g_cal, sizeof(cal_data_t));
        fresh.data_crc = fm24_crc32(&fresh.data, sizeof(cal_data_t));
        memcpy(&fresh.cfg,  &g_cfg, sizeof(cal_config_t));
        fresh.cfg_crc  = fm24_crc32(&fresh.cfg,  sizeof(cal_config_t));
        flash_write_block(&fresh);
    }

    g_loaded = data_ok;
}

uint8_t relay_cal_save(void)
{
    cal_flash_block_t blk;
    g_cfg.magic = CAL_MAGIC;
    memcpy(&blk.data, &g_cal, sizeof(cal_data_t));
    blk.data_crc = fm24_crc32(&blk.data, sizeof(cal_data_t));
    memcpy(&blk.cfg,  &g_cfg, sizeof(cal_config_t));
    blk.cfg_crc  = fm24_crc32(&blk.cfg,  sizeof(cal_config_t));
    uint8_t ok = flash_write_block(&blk);
    if (ok) g_loaded = 1;
    return ok;
}

void relay_cal_reset(void)
{
    fill_nominal(&g_cal);
}

void relay_cal_set(uint8_t decade, uint8_t digit, uint32_t milliohm)
{
    if (decade < 1 || decade > CAL_DECADES || digit >= CAL_DIGITS) return;
    g_cal.milliohm[decade - 1][digit] = milliohm;
}

uint32_t relay_cal_get(uint8_t decade, uint8_t digit)
{
    if (decade < 1 || decade > CAL_DECADES || digit >= CAL_DIGITS) return 0;
    return g_cal.milliohm[decade - 1][digit];
}

uint32_t relay_cal_total_milliohm(void)
{
    uint32_t total = 0;
    for (uint8_t d = 1; d <= CAL_DECADES; d++) {
        uint8_t dig = relay_get_decade_digit(d);
        if (dig == 0xFF) dig = 0;
        total += g_cal.milliohm[d - 1][dig];
    }
    return total;
}

void relay_cal_enable(bool en)    { g_cfg.enabled = en ? 1u : 0u; }
bool relay_cal_is_enabled(void)   { return g_cfg.enabled != 0; }
uint8_t relay_cal_is_loaded(void) { return g_loaded; }
