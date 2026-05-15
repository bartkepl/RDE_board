/*
 * utils.c – serial number generation from MCU UID (FNV-1a hash)
 * Adapted from SDT_board for STM32G4
 */

#include "utils.h"
#include "stm32g4xx_hal.h"
#include <stdint.h>
#include <stddef.h>

#define SERIAL_RAW_LEN 20

static uint8_t serial_raw[SERIAL_RAW_LEN];
static char serial_short[9];   /* 8 hex chars + NUL */
static char serial_full[41];   /* 40 hex chars + NUL */
static uint8_t serial_initialized = 0;

static const char hex[] = "0123456789ABCDEF";

static inline uint32_t fnv1a_32(const uint8_t *data, size_t len)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void to_hex(const uint8_t *in, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        out[2 * i]     = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0F];
    }
    out[2 * len] = '\0';
}

static void serial_init_once(void)
{
    if (serial_initialized) return;

    uint32_t *p = (uint32_t *)serial_raw;
    p[0] = HAL_GetDEVID();
    p[1] = HAL_GetREVID();
    p[2] = HAL_GetUIDw0();
    p[3] = HAL_GetUIDw1();
    p[4] = HAL_GetUIDw2();

    uint32_t hash = fnv1a_32(serial_raw, SERIAL_RAW_LEN);
    for (int i = 0; i < 8; i++) {
        serial_short[i] = hex[(hash >> (28 - 4 * i)) & 0xF];
    }
    serial_short[8] = '\0';

    to_hex(serial_raw, SERIAL_RAW_LEN, serial_full);

    serial_initialized = 1;
}

const char *serial_get(void)
{
    serial_init_once();
    return serial_short;
}

const char *serial_get_full(void)
{
    serial_init_once();
    return serial_full;
}
