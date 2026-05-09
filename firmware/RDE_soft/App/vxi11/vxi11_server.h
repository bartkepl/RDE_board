/*
 * vxi11_server.h – VXI-11 Core server over W5500 sockets
 *
 * Socket assignment:
 *   Socket 1 – portmapper TCP  port 111
 *   Socket 2 – portmapper UDP  port 111
 *   Socket 3 – VXI-11 Core TCP port 703
 *
 * Compatible with NI-MAX: TCPIP::<ip>::INSTR
 * Compatible with PyVISA: rm.open_resource("TCPIP::<ip>::INSTR")
 */

#ifndef APP_VXI11_VXI11_SERVER_H_
#define APP_VXI11_VXI11_SERVER_H_

void vxi11_server_init(void);
void vxi11_server_task(void);
void vxi11_server_close(void);

#endif /* APP_VXI11_VXI11_SERVER_H_ */
