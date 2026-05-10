/*
 * fm24c64b.c – driver for Cypress FM24C64B 64Kb FRAM (I2C)
 *
 * Uses STM32 HAL I2C memory-access functions which handle the
 * I2C START + device address + 16-bit memory address + data in one call.
 *
 * FM24C64B I2C protocol:
 *   Write: S | DevAddr+W | AddrMSB | AddrLSB | data... | P
 *   Read:  S | DevAddr+W | AddrMSB | AddrLSB | Sr | DevAddr+R | data... | NACK | P
 */

#include "fm24c64b.h"
#include <string.h>

#define FM24_TIMEOUT_MS  10u

extern I2C_HandleTypeDef hi2c1;
extern CRC_HandleTypeDef hcrc;

HAL_StatusTypeDef fm24_write(uint16_t addr, const void *data, uint16_t len)
{
    return HAL_I2C_Mem_Write(&hi2c1,
                             FM24_ADDR << 1,
                             addr,
                             I2C_MEMADD_SIZE_16BIT,
                             (uint8_t *)data,
                             len,
                             FM24_TIMEOUT_MS);
}

HAL_StatusTypeDef fm24_read(uint16_t addr, void *data, uint16_t len)
{
    return HAL_I2C_Mem_Read(&hi2c1,
                            FM24_ADDR << 1,
                            addr,
                            I2C_MEMADD_SIZE_16BIT,
                            (uint8_t *)data,
                            len,
                            FM24_TIMEOUT_MS);
}

uint8_t fm24_ping(void)
{
    return (HAL_I2C_IsDeviceReady(&hi2c1, FM24_ADDR << 1, 2, FM24_TIMEOUT_MS) == HAL_OK) ? 1u : 0u;
}

uint32_t fm24_crc32(const void *data, uint16_t len)
{
    return HAL_CRC_Calculate(&hcrc, (uint32_t *)data, len);
}
