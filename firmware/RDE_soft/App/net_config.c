/*
 * net_config.c – persistent network configuration stored in STM32G4 flash
 *
 * Last page of 128 KB flash (page 63) is reserved for config.
 * Page start: 0x0801F800, page size: 2 KB.
 *
 * STM32G4 flash constraints:
 *   - Minimum write granularity: 64-bit double-word (8 bytes)
 *   - Must erase entire page before programming
 *   - net_config_t is padded to 24 bytes = 3 double-words
 */

#include "net_config.h"
#include "stm32g4xx_hal.h"
#include <string.h>

#define CONFIG_FLASH_PAGE    63u
#define CONFIG_FLASH_ADDR    0x0801F800u   /* page 63 start */
#define CONFIG_FLASH_BANK    FLASH_BANK_1

_Static_assert(sizeof(net_config_t) % 8 == 0,
    "net_config_t must be a multiple of 8 bytes for STM32G4 flash programming");

static net_config_t g_cfg;

static void apply_defaults(void)
{
    g_cfg.magic     = NET_CONFIG_MAGIC;
    uint8_t ip[] = NET_CFG_DEFAULT_IP;
    uint8_t sn[] = NET_CFG_DEFAULT_SN;
    uint8_t gw[] = NET_CFG_DEFAULT_GW;
    memcpy(g_cfg.ip, ip, 4);
    memcpy(g_cfg.sn, sn, 4);
    memcpy(g_cfg.gw, gw, 4);
    g_cfg.use_dhcp  = NET_CFG_DEFAULT_DHCP;
    g_cfg._pad[0]   = 0;
    g_cfg._pad[1]   = 0;
    g_cfg._pad[2]   = 0;
}

void net_config_init(void)
{
    const net_config_t *flash = (const net_config_t *)CONFIG_FLASH_ADDR;
    if (flash->magic == NET_CONFIG_MAGIC) {
        memcpy(&g_cfg, flash, sizeof(net_config_t));
    } else {
        apply_defaults();
    }
}

void net_config_save(void)
{
    g_cfg.magic = NET_CONFIG_MAGIC;

    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase = {
        .TypeErase = FLASH_TYPEERASE_PAGES,
        .Banks     = CONFIG_FLASH_BANK,
        .Page      = CONFIG_FLASH_PAGE,
        .NbPages   = 1,
    };
    uint32_t page_err = 0;
    HAL_FLASHEx_Erase(&erase, &page_err);

    const uint64_t *src  = (const uint64_t *)&g_cfg;
    uint32_t        addr = CONFIG_FLASH_ADDR;
    for (size_t i = 0; i < sizeof(net_config_t) / 8; i++) {
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, src[i]);
        addr += 8;
    }

    HAL_FLASH_Lock();
}

net_config_t *net_config_get(void)
{
    return &g_cfg;
}
