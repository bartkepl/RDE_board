/*
 * net_config.h – persistent network configuration stored in the last flash page
 *
 * STM32G431CBT6: 128 KB flash, 64 pages × 2 KB.
 * Config page: page 63 at 0x0801F800.
 *
 * The struct is 24 bytes → 3 double-words (minimum STM32G4 write unit = 8 bytes).
 * Config is validated by a magic word; corrupt/erased page falls back to defaults.
 *
 * Usage:
 *   net_config_init()  – call once at startup before w5500_net_init()
 *   net_config_save()  – erase page + write after changing fields
 *   net_config_get()   – returns pointer to live config struct
 */

#ifndef APP_NET_CONFIG_H_
#define APP_NET_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

#define NET_CONFIG_MAGIC  0xDE1AC0DEu

typedef struct {
    uint32_t magic;
    uint8_t  ip[4];
    uint8_t  sn[4];
    uint8_t  gw[4];
    uint8_t  use_dhcp;   /* 1 = try DHCP first (fallback static), 0 = static only */
    uint8_t  _pad[7];
} __attribute__((__packed__)) net_config_t;          /* 24 bytes = 3 × DWORD */

/* Defaults applied when flash is erased / magic does not match */
#define NET_CFG_DEFAULT_IP      {192, 168, 1, 50}
#define NET_CFG_DEFAULT_SN      {255, 255, 255, 0}
#define NET_CFG_DEFAULT_GW      {192, 168, 1, 1}
#define NET_CFG_DEFAULT_DHCP    1u

void          net_config_init(void);
void          net_config_save(void);
net_config_t *net_config_get(void);

#endif /* APP_NET_CONFIG_H_ */
