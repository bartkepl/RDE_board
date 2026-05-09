/*
 * w5500_net.h – W5500 Ethernet init, DHCP, static-IP fallback
 *
 * Socket allocation:
 *   Socket 0 – DHCP client
 *   Socket 1 – VXI-11 portmapper TCP (port 111)
 *   Socket 2 – VXI-11 portmapper UDP (port 111)
 *   Socket 3 – VXI-11 Core TCP (port 703)
 */

#ifndef APP_W5500_NET_H_
#define APP_W5500_NET_H_

#include <stdint.h>
#include <stdbool.h>

/* Static IP fallback used when DHCP times out (must be outside DHCP range) */
#define RDE_STATIC_IP     {192, 168,   1,  50}
#define RDE_SUBNET_MASK   {255, 255, 255,   0}
#define RDE_GATEWAY       {192, 168,   1,   1}
#define RDE_DNS_SERVER    {  8,   8,   8,   8}   /* not used, reserved */

/* ── PHY speed selection ──────────────────────────────────────────────────
 * Change W5500_PHY_MODE to diagnose RX signal quality issues.
 *
 *  W5500_PHY_AUTO     Auto-negotiation via PMODE pins (requires clean 100M RX)
 *  W5500_PHY_10M_HD   10 Mbps half-duplex  (works with degraded RX signal)
 *  W5500_PHY_100M_FD  100 Mbps full-duplex (requires good RX signal)
 *
 * Board v0.3: 100M fails (RX path attenuation). Use 10M until HW is fixed.
 * ───────────────────────────────────────────────────────────────────────── */
#define W5500_PHY_AUTO     0
#define W5500_PHY_10M_HD   1
#define W5500_PHY_100M_FD  2

#define W5500_PHY_MODE     W5500_PHY_10M_HD   /* ← change for HW debugging */

void w5500_net_init(void);
void w5500_net_task(void);

bool w5500_net_is_linked(void);
void w5500_net_get_ip(uint8_t ip[4]);

#endif /* APP_W5500_NET_H_ */
