/*
 * net_config_fram.c – persistent network configuration stored in FM24C64B FRAM
 *
 * Exposes the same API as net_config.c (flash backend).
 * To switch backends, exclude ONE of these files from the CubeIDE build:
 *   Right-click file → Resource Configurations → Exclude from Build → check Debug + Release
 *
 * Active backend: FRAM (FM24C64B, I2C1, address 0x50)
 *
 * Storage layout in FRAM:
 *   0x0000  net_config_t PRIMARY  (24 B)
 *   0x0018  CRC32 PRIMARY         (4 B)
 *   0x001C  padding               (4 B)   → block total 32 B
 *   0x0020  net_config_t BACKUP   (24 B)
 *   0x0038  CRC32 BACKUP          (4 B)
 *   0x003C  padding               (4 B)   → block total 32 B
 *
 * Read algorithm:
 *   1. Read PRIMARY + CRC → verify
 *   2. If OK → use PRIMARY
 *   3. If bad → read BACKUP + CRC → verify
 *   4. If OK → use BACKUP, restore PRIMARY
 *   5. If both bad → apply defaults, write both
 *
 * Write algorithm:
 *   1. Calculate CRC of new data
 *   2. Write PRIMARY (data + CRC), verify by read-back
 *   3. Write BACKUP  (data + CRC), verify by read-back
 */

#include "net_config.h"
#include "fram/fm24c64b.h"
#include <string.h>

/* FRAM offsets */
#define FRAM_NET_PRIMARY_ADDR   0x0000u
#define FRAM_NET_BACKUP_ADDR    0x0020u
#define FRAM_NET_CRC_OFFSET     0x0018u   /* relative to block start */

static net_config_t g_cfg;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void apply_defaults(void)
{
    g_cfg.magic    = NET_CONFIG_MAGIC;
    uint8_t ip[]   = NET_CFG_DEFAULT_IP;
    uint8_t sn[]   = NET_CFG_DEFAULT_SN;
    uint8_t gw[]   = NET_CFG_DEFAULT_GW;
    memcpy(g_cfg.ip, ip, 4);
    memcpy(g_cfg.sn, sn, 4);
    memcpy(g_cfg.gw, gw, 4);
    g_cfg.use_dhcp  = NET_CFG_DEFAULT_DHCP;
    g_cfg.phy_mode  = NET_CFG_DEFAULT_PHY_MODE;
    memset(g_cfg._pad, 0, sizeof(g_cfg._pad));
}

/* Write cfg + CRC to one block. Returns 1=OK, 0=I2C error. */
static uint8_t write_block(uint16_t base_addr, const net_config_t *cfg)
{
    uint32_t crc = fm24_crc32(cfg, sizeof(net_config_t));
    if (fm24_write(base_addr, cfg, sizeof(net_config_t)) != HAL_OK)
        return 0u;
    if (fm24_write(base_addr + FRAM_NET_CRC_OFFSET, &crc, sizeof(crc)) != HAL_OK)
        return 0u;
    return 1u;
}

static uint8_t read_block(uint16_t base_addr, net_config_t *cfg)
{
    uint32_t stored_crc = 0;
    if (fm24_read(base_addr, cfg, sizeof(net_config_t)) != HAL_OK)
        return 0u;
    if (fm24_read(base_addr + FRAM_NET_CRC_OFFSET, &stored_crc, sizeof(stored_crc)) != HAL_OK)
        return 0u;
    uint32_t calc_crc = fm24_crc32(cfg, sizeof(net_config_t));
    return (calc_crc == stored_crc) ? 1u : 0u;
}

/* ── Public API (same as net_config.c) ───────────────────────────────────── */

void net_config_init(void)
{
    net_config_t tmp;

    if (read_block(FRAM_NET_PRIMARY_ADDR, &tmp) && tmp.magic == NET_CONFIG_MAGIC) {
        memcpy(&g_cfg, &tmp, sizeof(net_config_t));
        return;
    }

    if (read_block(FRAM_NET_BACKUP_ADDR, &tmp) && tmp.magic == NET_CONFIG_MAGIC) {
        memcpy(&g_cfg, &tmp, sizeof(net_config_t));
        write_block(FRAM_NET_PRIMARY_ADDR, &g_cfg);   /* restore primary */
        return;
    }

    apply_defaults();
    write_block(FRAM_NET_PRIMARY_ADDR, &g_cfg);
    write_block(FRAM_NET_BACKUP_ADDR,  &g_cfg);
}

/* Returns 1 if both blocks written and verified OK, 0 on any I2C error. */
uint8_t net_config_save(void)
{
    g_cfg.magic = NET_CONFIG_MAGIC;
    uint8_t ok  = write_block(FRAM_NET_PRIMARY_ADDR, &g_cfg);
    ok         &= write_block(FRAM_NET_BACKUP_ADDR,  &g_cfg);
    return ok;
}

net_config_t *net_config_get(void)
{
    return &g_cfg;
}

