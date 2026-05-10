/*
 * relay_cal.c – per-decade resistance calibration stored in FRAM
 */

#include "relay_cal.h"
#include "relay_ctrl.h"
#include "../fram/fm24c64b.h"
#include <string.h>

/* FRAM base addresses */
#define FRAM_CAL_DATA_PRIMARY    0x0040u
#define FRAM_CAL_DATA_BACKUP     0x0140u
#define FRAM_CAL_CFG_PRIMARY     0x0134u
#define FRAM_CAL_CFG_BACKUP      0x0234u

/* Per-decade unit multipliers: index 0 = 1Ω decade, index 5 = 100kΩ decade */
static const uint32_t k_multiplier[CAL_DECADES] = {1, 10, 100, 1000, 10000, 100000};

static cal_data_t   g_cal;
static cal_config_t g_cfg;
static uint8_t      g_loaded;  /* 1 = loaded from FRAM */

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void fill_nominal(cal_data_t *d)
{
    for (uint8_t dec = 0; dec < CAL_DECADES; dec++) {
        for (uint8_t dig = 0; dig < CAL_DIGITS; dig++) {
            d->milliohm[dec][dig] = (uint32_t)dig * k_multiplier[dec] * 1000u;
        }
    }
}

static void write_data_block(uint16_t addr, const cal_data_t *d)
{
    uint32_t crc = fm24_crc32(d, sizeof(cal_data_t));
    fm24_write(addr, d, sizeof(cal_data_t));
    fm24_write(addr + sizeof(cal_data_t), &crc, sizeof(crc));
}

static uint8_t read_data_block(uint16_t addr, cal_data_t *d)
{
    uint32_t stored = 0;
    fm24_read(addr, d, sizeof(cal_data_t));
    fm24_read(addr + sizeof(cal_data_t), &stored, sizeof(stored));
    return (fm24_crc32(d, sizeof(cal_data_t)) == stored) ? 1u : 0u;
}

static void write_cfg_block(uint16_t addr, const cal_config_t *c)
{
    uint32_t crc = fm24_crc32(c, sizeof(cal_config_t));
    fm24_write(addr, c, sizeof(cal_config_t));
    fm24_write(addr + sizeof(cal_config_t), &crc, sizeof(crc));
}

static uint8_t read_cfg_block(uint16_t addr, cal_config_t *c)
{
    uint32_t stored = 0;
    fm24_read(addr, c, sizeof(cal_config_t));
    fm24_read(addr + sizeof(cal_config_t), &stored, sizeof(stored));
    return (fm24_crc32(c, sizeof(cal_config_t)) == stored) ? 1u : 0u;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void relay_cal_init(void)
{
    cal_data_t   tmp_data;
    cal_config_t tmp_cfg;
    uint8_t data_ok = 0;
    uint8_t cfg_ok  = 0;

    /* Try primary */
    if (read_data_block(FRAM_CAL_DATA_PRIMARY, &tmp_data)) {
        memcpy(&g_cal, &tmp_data, sizeof(cal_data_t));
        data_ok = 1;
    }
    if (read_cfg_block(FRAM_CAL_CFG_PRIMARY, &tmp_cfg) && tmp_cfg.magic == CAL_MAGIC) {
        memcpy(&g_cfg, &tmp_cfg, sizeof(cal_config_t));
        cfg_ok = 1;
    }

    /* Fall back to backup for any failed block */
    if (!data_ok) {
        if (read_data_block(FRAM_CAL_DATA_BACKUP, &tmp_data)) {
            memcpy(&g_cal, &tmp_data, sizeof(cal_data_t));
            data_ok = 1;
            write_data_block(FRAM_CAL_DATA_PRIMARY, &g_cal);   /* restore */
        }
    }
    if (!cfg_ok) {
        if (read_cfg_block(FRAM_CAL_CFG_BACKUP, &tmp_cfg) && tmp_cfg.magic == CAL_MAGIC) {
            memcpy(&g_cfg, &tmp_cfg, sizeof(cal_config_t));
            cfg_ok = 1;
            write_cfg_block(FRAM_CAL_CFG_PRIMARY, &g_cfg);     /* restore */
        }
    }

    if (!data_ok) {
        fill_nominal(&g_cal);
        write_data_block(FRAM_CAL_DATA_PRIMARY, &g_cal);
        write_data_block(FRAM_CAL_DATA_BACKUP,  &g_cal);
    }
    if (!cfg_ok) {
        g_cfg.magic   = CAL_MAGIC;
        g_cfg.enabled = 0;
        memset(g_cfg._pad, 0, sizeof(g_cfg._pad));
        write_cfg_block(FRAM_CAL_CFG_PRIMARY, &g_cfg);
        write_cfg_block(FRAM_CAL_CFG_BACKUP,  &g_cfg);
    }

    g_loaded = data_ok;
}

void relay_cal_save(void)
{
    g_cfg.magic = CAL_MAGIC;
    write_data_block(FRAM_CAL_DATA_PRIMARY, &g_cal);
    write_data_block(FRAM_CAL_DATA_BACKUP,  &g_cal);
    write_cfg_block(FRAM_CAL_CFG_PRIMARY, &g_cfg);
    write_cfg_block(FRAM_CAL_CFG_BACKUP,  &g_cfg);
    g_loaded = 1;
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
        if (dig == 0xFF) {
            /* Unknown (raw bit access used): fall back to nominal */
            dig = 0;
        }
        total += g_cal.milliohm[d - 1][dig];
    }
    return total;
}

void relay_cal_enable(bool en)
{
    g_cfg.enabled = en ? 1u : 0u;
}

bool relay_cal_is_enabled(void)
{
    return g_cfg.enabled != 0;
}

uint8_t relay_cal_is_loaded(void)
{
    return g_loaded;
}
