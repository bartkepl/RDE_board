/*
 * relay_ctrl.h – driver for 6x STPIC6C595TTR resistance decade relay chain
 *
 * Hardware:
 *   6x STPIC6C595TTR chained in series (SPI1, TX-only, MSB first, Mode 0)
 *   PA4 = REL_EN  (Output Enable, active HIGH – enable after init)
 *   PA6 = REL_RCK (Latch – rising edge transfers shift reg → output reg)
 *
 * Chain order (data[0] sent first → arrives at last chip = STPIC6):
 *   data[0] → STPIC6 = 100 kΩ decade  (bits Q0..Q5)
 *   data[1] → STPIC5 =  10 kΩ decade
 *   data[2] → STPIC4 =   1 kΩ decade
 *   data[3] → STPIC3 = 100  Ω decade
 *   data[4] → STPIC2 =  10  Ω decade
 *   data[5] → STPIC1 =   1  Ω decade  (also Q6=CONNECT relay, Q7=SHORT relay)
 *
 * STPIC is open-drain/sink: Q output LOW (bit=1) → relay energised (CLOSED).
 *
 * Per-decade bit layout (Q0..Q5):
 *   Q0 (SW1): short/bypass (0 Ω in this decade)
 *   Q1 (SW2): bypass the first resistor R1 (enables even-step access)
 *   Q2 (SW3): tap after R1
 *   Q3 (SW4): tap after R2
 *   Q4 (SW5): tap after R3
 *   Q5 (SW6): tap after R4
 *
 * STPIC1-only (data[5]) bits:
 *   Q6: CONNECT relay – 1 (ON) = decade chain connected to terminals (ohms > 0)
 *   Q7: SHORT relay   – 1 (ON) = terminals shorted directly (ohms = 0 or output OFF)
 *
 * Resistor string: R1=1×unit, R2=R3=R4=R5=2×unit  → total 9×unit per decade
 * Range: 0–999 999 Ω in 1 Ω steps  (6 decades: 1, 10, 100, 1k, 10k, 100k)
 */

#ifndef APP_RELAY_RELAY_CTRL_H_
#define APP_RELAY_RELAY_CTRL_H_

#include <stdint.h>
#include <stdbool.h>

/* ── Sentinel for "resistance unknown" (after relay_set_bit) ────────────── */
#define RELAY_RESISTANCE_UNKNOWN  UINT32_MAX

/* ── Low-level ──────────────────────────────────────────────────────────── */

void relay_init(void);
void relay_set_raw(const uint8_t data[6]);
void relay_enable(bool en);
bool relay_is_enabled(void);
void relay_get_raw(uint8_t out[6]);

/* ── High-level resistance control ─────────────────────────────────────── */

/* Set resistance 0–999 999 Ω (with protection relay sequence). */
void relay_set_resistance(uint32_t ohms);

/* Query currently set resistance. */
uint32_t relay_get_resistance(void);

/* Control terminal relays independently.
 *   connected = true  → re-applies current resistance (0Ω→SHORT, >0→CONNECT)
 *   connected = false → Q6 OFF, Q7 ON (terminals shorted, decade disconnected) */
void relay_set_output(bool connected);
bool relay_get_output(void);

/* ── Per-decade digit control ───────────────────────────────────────────── */

/* Set decade d (1=1Ω … 6=100kΩ) to digit 0–9 with protection sequence.
 * Recalculates current_resistance. Sets output_connected = true. */
void relay_set_decade(uint8_t decade, uint8_t digit);

/* Returns digit 0–9 for that decade.
 * Returns 0xFF when current_resistance == RELAY_RESISTANCE_UNKNOWN. */
uint8_t relay_get_decade_digit(uint8_t decade);

/* ── Single relay bit access (no protection sequence) ───────────────────── */

/* Set/get single Q-bit (bit 0–5) in decade d (1–6).
 * Q6/Q7 (CONNECT/SHORT) are NOT accessible.
 * relay_set_bit() sets current_resistance = RELAY_RESISTANCE_UNKNOWN. */
void relay_set_bit(uint8_t decade, uint8_t bit, bool on);
bool relay_get_bit(uint8_t decade, uint8_t bit);

#endif /* APP_RELAY_RELAY_CTRL_H_ */
