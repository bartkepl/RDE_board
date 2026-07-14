/*
 * w5500_net.h – W5500 Ethernet init, DHCP, static-IP fallback
 *
 * Socket allocation:
 *   Socket 0 – DHCP client
 *   Socket 1 – VXI-11 portmapper TCP (port 111)
 *   Socket 2 – VXI-11 portmapper UDP (port 111)
 *   Socket 3 – VXI-11 Core TCP (port 703)
 *   Socket 4 – mDNS (UDP multicast 224.0.0.251:5353)
 */

#ifndef APP_W5500_NET_H_
#define APP_W5500_NET_H_

#include <stdint.h>
#include <stdbool.h>

void w5500_net_init(void);
void w5500_net_task(void);
void w5500_net_restart(void);   /* apply new net_config + restart state machine */

bool w5500_net_is_linked(void);
void w5500_net_get_ip(uint8_t ip[4]);

#endif /* APP_W5500_NET_H_ */
