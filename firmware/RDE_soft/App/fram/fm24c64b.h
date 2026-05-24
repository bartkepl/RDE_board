/*
 * fm24c64b.h – driver for Cypress FM24C64B 64Kb FRAM (I2C)
 *
 * Hardware:
 *   I2C1, 100 kHz, 7-bit address 0x50 (A0=A1=A2=GND)
 *   FRAM capacity: 8192 bytes (addresses 0x0000 – 0x1FFF)
 *   No page-write limit, no erase required, >10^13 write cycles
 *
 * FRAM memory map (project RDE_board):
 *   0x0000–0x001F  net_config_t PRIMARY + CRC32 + padding  (32 B)
 *   0x0020–0x003F  net_config_t BACKUP  + CRC32 + padding  (32 B)
 *   0x0040–0x0133  cal_data_t   PRIMARY + CRC32            (244 B)
 *   0x0134–0x013F  cal_config_t PRIMARY + CRC32            (12 B)
 *   0x0140–0x0233  cal_data_t   BACKUP  + CRC32            (244 B)
 *   0x0234–0x023F  cal_config_t BACKUP  + CRC32            (12 B)
 *   0x0240–0x1FFF  Free
 *
 * CRC: hardware CRC-32 via hcrc (CRC_INPUTDATA_FORMAT_BYTES, default polynomial)
 */

#ifndef APP_FRAM_FM24C64B_H_
#define APP_FRAM_FM24C64B_H_

#include <stdint.h>
#include "stm32g4xx_hal.h"

/* I2C 7-bit device address (A0=A1=A2=GND → 0b1010000) */
#define FM24_ADDR        0x50u
/* Total FRAM capacity in bytes */
#define FM24_SIZE        8192u

/* ── Low-level byte access ───────────────────────────────────────────────── */

/* Write `len` bytes from `data` to FRAM starting at `addr`.
 * addr + len must not exceed FM24_SIZE.
 * Returns HAL_OK on success. */
HAL_StatusTypeDef fm24_write(uint16_t addr, const void *data, uint16_t len);

/* Read `len` bytes from FRAM starting at `addr` into `data`.
 * Returns HAL_OK on success. */
HAL_StatusTypeDef fm24_read(uint16_t addr, void *data, uint16_t len);

/* Verify FRAM is reachable on I2C bus. Returns 1 if present, 0 if not. */
uint8_t fm24_ping(void);

/* Reset I2C peripheral after a bus error or timeout (HAL DeInit + Init). */
void fm24_recover_bus(void);

/* ── CRC helper (uses hardware hcrc, CRC-32) ────────────────────────────── */

/* Calculate CRC-32 over `len` bytes starting at `data`.
 * Resets hcrc accumulator before calculation. */
uint32_t fm24_crc32(const void *data, uint16_t len);

#endif /* APP_FRAM_FM24C64B_H_ */
