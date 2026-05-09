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
 * STPIC is open-drain/sink: Q output LOW (bit=0) → relay energised (CLOSED).
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
 *   Q6: CONNECT relay – 0 (ON) = input terminals connected to decade
 *   Q7: SHORT relay   – 0 (ON) = input terminals shorted (protection, before Q6)
 *
 * Resistor string: R1=1×unit, R2=R3=R4=R5=2×unit  → total 9×unit per decade
 * Range: 0–999 999 Ω in 1 Ω steps  (6 decades: 1, 10, 100, 1k, 10k, 100k)
 */

#ifndef APP_RELAY_RELAY_CTRL_H_
#define APP_RELAY_RELAY_CTRL_H_

#include <stdint.h>
#include <stdbool.h>

/* ── Low-level ──────────────────────────────────────────────────────────── */

void relay_init(void);
void relay_set_raw(const uint8_t data[6]);
void relay_enable(bool en);

/* ── High-level resistance control ─────────────────────────────────────── */

/* Set resistance 0–999 999 Ω (with protection relay sequence). */
void relay_set_resistance(uint32_t ohms);

/* Query currently set resistance. */
uint32_t relay_get_resistance(void);

/* Control terminal relays independently.
 *   connected = true  → Q6 ON (input terminals → decade), Q7 OFF (no short)
 *   connected = false → Q6 OFF (disconnected),            Q7 ON (short)
 * relay_set_resistance() manages these automatically. */
void relay_set_output(bool connected);
bool relay_get_output(void);

#endif /* APP_RELAY_RELAY_CTRL_H_ */
