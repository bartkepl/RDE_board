/*
 * relay_ctrl.c – 6x STPIC6C595TTR resistance decade relay driver
 *
 * Topology (1-2-2-2-2 series string with taps):
 *
 *   IN ─┬─────[R1=1]─────[R2=2]─────[R3=2]─────[R4=2]─────[R5=2]─── TO_NEXT
 *       │          │            │           │           │
 *      SW1        SW3          SW4         SW5         SW6
 *   (Q0=bypass) (Q2=tap) (Q3=tap) (Q4=tap) (Q5=tap)
 *       └────────────────────────────────────────────────────────────── OUT bus
 *
 *   SW2 (Q1): connects IN directly to after-R1 (bypasses R1)
 *
 * Value encoding per decade digit d (0-9):
 *   d=0:     Q0=0  (SW1 closed → direct short)
 *   d=1:     Q2=0  (SW3, tap after R1=1Ω)
 *   d=2:     Q1=0, Q3=0  (bypass R1, tap after R2→2Ω)
 *   d=3:     Q3=0  (tap after R1+R2=3Ω)
 *   d=4:     Q1=0, Q4=0  (bypass R1, tap after R3→4Ω)
 *   d=5:     Q4=0  (tap after R1+R2+R3=5Ω)
 *   d=6:     Q1=0, Q5=0  (bypass R1, tap after R4→6Ω)
 *   d=7:     Q5=0  (tap after R1+R2+R3+R4=7Ω)
 *   d=8:     Q1=0  (bypass R1, all taps open → R2+R3+R4+R5=8Ω)
 *   d=9:     all open (R1+R2+R3+R4+R5=9Ω through-path)
 *
 * STPIC sink-mode: bit 0 in shift register → Q output LOW → relay energised.
 *
 * Protection relay sequence (on resistance change):
 *   1. Enable SHORT (Q7=0) – short input terminals
 *   2. Disable CONNECT (Q6=1) – disconnect decade from terminals
 *   3. Set new resistance in shift registers
 *   4. Enable CONNECT (Q6=0) – reconnect decade
 *   5. Disable SHORT (Q7=1) – remove short
 */

#include "relay_ctrl.h"
#include "main.h"
#include "stm32g4xx_hal.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* ── Private state ─────────────────────────────────────────────────────── */

static uint8_t  relay_state[6]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint32_t current_resistance = 0;
static bool     output_connected   = false;

/* ── SPI transfer ──────────────────────────────────────────────────────── */

static void relay_latch(void)
{
    HAL_GPIO_WritePin(REL_RCK_GPIO_Port, REL_RCK_Pin, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(REL_RCK_GPIO_Port, REL_RCK_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(REL_RCK_GPIO_Port, REL_RCK_Pin, GPIO_PIN_RESET);
}

/* ── Decade encoding ───────────────────────────────────────────────────── */

/*
 * Returns the 8-bit STPIC output word for a single decade digit d (0–9).
 * Bits 6 and 7 are set to 1 (both protection relays OFF by default).
 * STPIC sink: 0 = relay ON (closed), 1 = relay OFF (open).
 */
static uint8_t decade_byte(uint8_t d)
{
    uint8_t bits = 0xFF;  /* all relays OFF */

    switch (d) {
    case 0: bits &= ~(1u << 0);                      break;  /* Q0=ON (SW1 short)         */
    case 1: bits &= ~(1u << 2);                      break;  /* Q2=ON (SW3, 1R tap)        */
    case 2: bits &= ~((1u<<1)|(1u<<3));              break;  /* Q1+Q3 (bypass+SW4, 2R)     */
    case 3: bits &= ~(1u << 3);                      break;  /* Q3=ON (SW4, 3R tap)        */
    case 4: bits &= ~((1u<<1)|(1u<<4));              break;  /* Q1+Q4 (bypass+SW5, 4R)     */
    case 5: bits &= ~(1u << 4);                      break;  /* Q4=ON (SW5, 5R tap)        */
    case 6: bits &= ~((1u<<1)|(1u<<5));              break;  /* Q1+Q5 (bypass+SW6, 6R)     */
    case 7: bits &= ~(1u << 5);                      break;  /* Q5=ON (SW6, 7R tap)        */
    case 8: bits &= ~(1u << 1);                      break;  /* Q1=ON (bypass, 8R through) */
    case 9: /* all open → 9R full string */           break;
    default: break;
    }

    return bits;
}

/*
 * Build the full 6-byte data array from an ohms value.
 * data[0] → STPIC6 (100 kΩ decade), data[5] → STPIC1 (1 Ω decade).
 * Protection relay bits in data[5] are NOT set here – caller sets them.
 */
static void build_data(uint32_t ohms, uint8_t data[6])
{
    static const uint32_t scale[6] = {100000, 10000, 1000, 100, 10, 1};
    uint32_t r = ohms;
    for (int i = 0; i < 6; i++) {
        uint8_t d = (uint8_t)(r / scale[i]);
        r %= scale[i];
        data[i] = decade_byte(d);
    }
}

/* Apply protection relay state to data[5] (STPIC1). */
static void apply_protection(uint8_t data[6], bool connected)
{
    if (connected) {
        data[5] &= ~(1u << 6);  /* Q6=0 (ON)  → CONNECT relay closed */
        data[5] |=  (1u << 7);  /* Q7=1 (OFF) → SHORT relay open     */
    } else {
        data[5] |=  (1u << 6);  /* Q6=1 (OFF) → CONNECT relay open   */
        data[5] &= ~(1u << 7);  /* Q7=0 (ON)  → SHORT relay closed   */
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void relay_init(void)
{
    HAL_GPIO_WritePin(REL_EN_GPIO_Port,  REL_EN_Pin,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(REL_RCK_GPIO_Port, REL_RCK_Pin, GPIO_PIN_RESET);

    /* Safe initial state: all decade relays off, terminals shorted (protected) */
    uint8_t data[6];
    memset(data, 0xFF, 6);
    apply_protection(data, false);  /* SHORT=ON, CONNECT=OFF */

    memcpy(relay_state, data, 6);
    HAL_SPI_Transmit(&hspi1, data, 6, 20);
    relay_latch();
   // relay_enable(true);
}

void relay_set_raw(const uint8_t data[6])
{
    memcpy(relay_state, data, 6);
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, 6, 20);
    relay_latch();
}

void relay_enable(bool en)
{
    HAL_GPIO_WritePin(REL_EN_GPIO_Port, REL_EN_Pin,
                      en ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void relay_set_resistance(uint32_t ohms)
{
    if (ohms > 999999u) ohms = 999999u;
    current_resistance = ohms;

    uint8_t data[6];

    /* Step 1: engage SHORT, disengage CONNECT */
    build_data(ohms, data);
    apply_protection(data, false);
    relay_set_raw(data);
    HAL_Delay(10);  /* relay settle time */

    /* Step 2: engage CONNECT, disengage SHORT */
    apply_protection(data, true);
    relay_set_raw(data);
    HAL_Delay(10);

    output_connected = true;
}

uint32_t relay_get_resistance(void)
{
    return current_resistance;
}

void relay_set_output(bool connected)
{
    output_connected = connected;
    uint8_t data[6];
    memcpy(data, relay_state, 6);
    apply_protection(data, connected);
    relay_set_raw(data);
    if (connected) HAL_Delay(10);
}

bool relay_get_output(void)
{
    return output_connected;
}
