/*
 * relay_cal.h – per-decade resistance calibration for RDE_board
 *
 * Stores calibration values (actual resistance in milliohms) for each of
 * the 60 decade-digit combinations (6 decades × 10 digits).
 * Data is persisted in FM24C64B FRAM with CRC protection and a backup copy.
 *
 * FRAM layout:
 *   0x0040  cal_data_t  PRIMARY  (240 B) + CRC32 (4 B)  → 244 B
 *   0x0134  cal_config_t PRIMARY (8 B)   + CRC32 (4 B)  → 12 B
 *   0x0140  cal_data_t  BACKUP   (240 B) + CRC32 (4 B)  → 244 B
 *   0x0234  cal_config_t BACKUP  (8 B)   + CRC32 (4 B)  → 12 B
 *
 * Decade numbering (matches SCPI): 1 = 1Ω, 2 = 10Ω, ... 6 = 100kΩ
 * Internally stored as index 0–5 (index = decade_number - 1).
 *
 * Nominal values (no calibration):
 *   milliohm[d][n] = n × multiplier[d] × 1000
 *   multiplier = {1, 10, 100, 1000, 10000, 100000} for d = 0..5
 */

#ifndef APP_RELAY_RELAY_CAL_H_
#define APP_RELAY_RELAY_CAL_H_

#include <stdint.h>
#include <stdbool.h>

#define CAL_MAGIC        0xCA11B001UL

#define CAL_DECADES      6u
#define CAL_DIGITS       10u

typedef struct {
    uint32_t milliohm[CAL_DECADES][CAL_DIGITS]; /* [decade_idx 0-5][digit 0-9] */
} cal_data_t;   /* 240 bytes */

typedef struct {
    uint32_t magic;      /* CAL_MAGIC if calibration data is valid */
    uint8_t  enabled;    /* 1 = apply calibration in RESistance:VALue? */
    uint8_t  _pad[3];
} cal_config_t; /* 8 bytes */

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Load calibration from FRAM (primary → backup → nominal defaults). */
void relay_cal_init(void);

/* Write current calibration data + config to FRAM (primary + backup). */
void relay_cal_save(void);

/* Reset in-RAM calibration values to nominal (does NOT write to FRAM). */
void relay_cal_reset(void);

/* ── Point access ────────────────────────────────────────────────────────── */

/* Set calibration point for decade d (1–6), digit n (0–9).
 * milliohm: actual measured resistance in milliohms. */
void relay_cal_set(uint8_t decade, uint8_t digit, uint32_t milliohm);

/* Get calibration value for decade d (1–6), digit n (0–9) in milliohms. */
uint32_t relay_cal_get(uint8_t decade, uint8_t digit);

/* ── Calibrated total ────────────────────────────────────────────────────── */

/* Return sum of calibrated milliohm values for all 6 current decade digits.
 * Uses relay_get_decade_digit() to read current hardware state. */
uint32_t relay_cal_total_milliohm(void);

/* ── Enable / disable ────────────────────────────────────────────────────── */

void relay_cal_enable(bool en);
bool relay_cal_is_enabled(void);

/* Returns 1 if calibration data was successfully loaded from FRAM. */
uint8_t relay_cal_is_loaded(void);

#endif /* APP_RELAY_RELAY_CAL_H_ */
