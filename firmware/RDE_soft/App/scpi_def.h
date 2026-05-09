/*
 * scpi_def.h – SCPI command definitions for RDE_board (Resistance DEcade)
 */

#ifndef APP_SCPI_DEF_H_
#define APP_SCPI_DEF_H_

#include <scpi/scpi.h>
#include <stdint.h>
#include <stdbool.h>

#define SCPI_IDN_MANUFACTURER "bartkepl"
#define SCPI_IDN_MODEL        "RDE"
#define SCPI_IDN_FW           "1.0"

extern scpi_t scpi_context;

void SCPI_Main_Init(void);
void SCPI_Main_Input(const char *data, uint32_t len);
void SCPI_Main_Poll(void);

/* Shared reply buffer – written by SCPI_Write, read by usbtmc_app and vxi11_server */
extern char     scpi_reply_buf[512];
extern uint16_t scpi_reply_len;
extern bool     scpi_reply_ready;

#endif /* APP_SCPI_DEF_H_ */
