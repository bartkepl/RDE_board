/*
 * mdns.h – minimal mDNS responder for VXI-11 service advertisement
 *
 * Listens on W5500 Socket 4, UDP multicast 224.0.0.251:5353.
 * Responds to DNS PTR queries for "_vxi-11._tcp.local" with:
 *   PTR  → "<name>._vxi-11._tcp.local"
 *   SRV  → port 703, target "<name>.local"
 *   A    → device IP
 *   TXT  → "txtvers=1"
 *
 * Device name is derived from serial_get() (8-char FNV-1a hash of MCU UID).
 *
 * Usage:
 *   mdns_init(ip)   – call after W5500 has a valid IP, opens socket
 *   mdns_task()     – call from main loop / w5500_net_task() in RUNNING state
 *   mdns_close()    – call on link-loss before closing other sockets
 */

#ifndef APP_MDNS_MDNS_H_
#define APP_MDNS_MDNS_H_

#include <stdint.h>

void mdns_init(const uint8_t device_ip[4]);
void mdns_task(void);
void mdns_close(void);

#endif /* APP_MDNS_MDNS_H_ */
