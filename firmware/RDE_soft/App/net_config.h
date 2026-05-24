/*
 * net_config.h – persistent network configuration stored in FRAM (FM24C64B)
 *
 * The struct is 24 bytes → 3 double-words.
 * Config is validated by a magic word; corrupt/missing FRAM falls back to defaults.
 *
 * Usage:
 *   net_config_init()  – call once at startup before w5500_net_init()
 *   net_config_save()  – write to FRAM after changing fields
 *   net_config_get()   – returns pointer to live config struct
 */

#ifndef APP_NET_CONFIG_H_
#define APP_NET_CONFIG_H_

#include <stdint.h>
#include <stdbool.h>

/* Bumped from 0xDE1AC0DE to invalidate old FRAM data after adding phy_mode field */
#define NET_CONFIG_MAGIC  0xDE1AC0DFu

/* PHY speed mode values stored in net_config_t.phy_mode */
#define NET_PHY_AUTO    0u   /* auto-negotiation via PMODE pins */
#define NET_PHY_10M_HD  1u   /* 10 Mbps half-duplex (forced)   */
#define NET_PHY_100M_FD 2u   /* 100 Mbps full-duplex (forced)  */

typedef struct {
    uint32_t magic;
    uint8_t  ip[4];
    uint8_t  sn[4];
    uint8_t  gw[4];
    uint8_t  use_dhcp;   /* 1 = try DHCP first (fallback static), 0 = static only */
    uint8_t  phy_mode;   /* NET_PHY_AUTO / NET_PHY_10M_HD / NET_PHY_100M_FD       */
    uint8_t  _pad[6];
} __attribute__((__packed__)) net_config_t;          /* 24 bytes = 3 × DWORD */

/* Defaults applied when FRAM is erased / magic does not match */
#define NET_CFG_DEFAULT_IP       {192, 168, 1, 6}
#define NET_CFG_DEFAULT_SN       {255, 255, 255, 0}
#define NET_CFG_DEFAULT_GW       {192, 168, 1, 1}
#define NET_CFG_DEFAULT_DHCP     1u
#define NET_CFG_DEFAULT_PHY_MODE NET_PHY_AUTO

void          net_config_init(void);
uint8_t       net_config_save(void);   /* 1=OK, 0=I2C error */
net_config_t *net_config_get(void);

#endif /* APP_NET_CONFIG_H_ */
