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
 *   d=0:     Q0=1  (SW1 closed → direct short of this decade)
 *   d=1:     Q2=1  (SW3, tap after R1=1Ω)
 *   d=2:     Q1=1, Q3=1  (bypass R1, tap after R2→2Ω)
 *   d=3:     Q3=1  (tap after R1+R2=3Ω)
 *   d=4:     Q1=1, Q4=1  (bypass R1, tap after R3→4Ω)
 *   d=5:     Q4=1  (tap after R1+R2+R3=5Ω)
 *   d=6:     Q1=1, Q5=1  (bypass R1, tap after R4→6Ω)
 *   d=7:     Q5=1  (tap after R1+R2+R3+R4=7Ω)
 *   d=8:     Q1=1  (bypass R1, all taps open → R2+R3+R4+R5=8Ω)
 *   d=9:     all 0 (R1+R2+R3+R4+R5=9Ω through-path)
 *
 * STPIC sink-mode: bit 1 in shift register → DRAIN LOW → relay energised (CLOSED).
 *
 * Output relay states (Q6/Q7 in data[5]):
 *   Resistance >0 Ω : Q6=1 (CONNECT ON),  Q7=0 (SHORT OFF)
 *   Resistance = 0 Ω : Q6=0 (CONNECT OFF), Q7=1 (SHORT ON) – only SHORT relay used
 *   Output OFF      : Q6=0 (CONNECT OFF),  Q7=1 (SHORT ON) – terminals shorted (safe)
 *
 * Resistance change: relay values are written directly – no SHORT toggle during change.
 */

#include "relay_ctrl.h"
#include "main.h"
#include "stm32g4xx_hal.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* ── Private state ─────────────────────────────────────────────────────── */

static uint8_t  relay_state[6]     = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static uint8_t  decade_digits[6]   = {0, 0, 0, 0, 0, 0};
static uint32_t current_resistance = 0;
static bool     output_connected   = false;
static bool     relay_en_state     = false;

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
 * STPIC sink: 1 = relay ON (DRAIN LOW, coil energised), 0 = relay OFF.
 * Bits 6 and 7 are left 0 (protection relays set separately).
 */
static uint8_t decade_byte(uint8_t d)
{
    uint8_t bits = 0x00;  /* all relays OFF */

    switch (d) {
    case 0: bits |= (1u << 0);                     break;  /* Q0=ON (SW1 short)         */
    case 1: bits |= (1u << 2);                     break;  /* Q2=ON (SW3, 1R tap)        */
    case 2: bits |= ((1u<<1)|(1u<<3));             break;  /* Q1+Q3 (bypass+SW4, 2R)     */
    case 3: bits |= (1u << 3);                     break;  /* Q3=ON (SW4, 3R tap)        */
    case 4: bits |= ((1u<<1)|(1u<<4));             break;  /* Q1+Q4 (bypass+SW5, 4R)     */
    case 5: bits |= (1u << 4);                     break;  /* Q4=ON (SW5, 5R tap)        */
    case 6: bits |= ((1u<<1)|(1u<<5));             break;  /* Q1+Q5 (bypass+SW6, 6R)     */
    case 7: bits |= (1u << 5);                     break;  /* Q5=ON (SW6, 7R tap)        */
    case 8: bits |= (1u << 1);                     break;  /* Q1=ON (bypass, 8R through) */
    case 9: /* all 0 → 9R full string */            break;
    default: break;
    }

    return bits;
}

/*
 * Build the full 6-byte data array from an ohms value.
 * data[0] → STPIC6 (100 kΩ decade), data[5] → STPIC1 (1 Ω decade).
 * Q6/Q7 bits in data[5] are NOT set here – caller sets them.
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

/*
 * Write data array with correct Q6/Q7 for the given ohms value.
 *   ohms  > 0 : Q6=ON (CONNECT), Q7=OFF  – decade chain in circuit
 *   ohms == 0 : Q6=OFF,          Q7=ON   – SHORT relay only, decades all off
 */
static void apply_output(uint8_t data[6], uint32_t ohms)
{
    if (ohms == 0) {
        data[5] &= ~(1u << 6);  /* Q6=OFF: CONNECT open   */
        data[5] |=  (1u << 7);  /* Q7=ON:  SHORT closed   */
    } else {
        data[5] |=  (1u << 6);  /* Q6=ON:  CONNECT closed */
        data[5] &= ~(1u << 7);  /* Q7=OFF: SHORT open     */
    }
}

/* ── Public API ────────────────────────────────────────────────────────── */

void relay_init(void)
{
    HAL_GPIO_WritePin(REL_EN_GPIO_Port,  REL_EN_Pin,  GPIO_PIN_RESET);
    HAL_GPIO_WritePin(REL_RCK_GPIO_Port, REL_RCK_Pin, GPIO_PIN_RESET);

    /* Initial state: all relays open (deenergised) – 0 = relay OFF */
    uint8_t data[6];
    memset(data, 0x00, 6);

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
    relay_en_state = en;
    HAL_GPIO_WritePin(REL_EN_GPIO_Port, REL_EN_Pin,
                      en ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

bool relay_is_enabled(void)
{
    return relay_en_state;
}

void relay_get_raw(uint8_t out[6])
{
    memcpy(out, relay_state, 6);
}

void relay_set_resistance(uint32_t ohms)
{
    if (ohms > 999999u) ohms = 999999u;
    current_resistance = ohms;

    /* Populate decade_digits */
    static const uint32_t scale6[6] = {100000, 10000, 1000, 100, 10, 1};
    uint32_t r2 = ohms;
    for (int i = 0; i < 6; i++) {
        decade_digits[i] = (uint8_t)(r2 / scale6[i]);
        r2 %= scale6[i];
    }

    uint8_t data[6];
    if (ohms == 0) {
        /* 0 Ω: SHORT relay only – no decade bypass relays, CONNECT off */
        memset(data, 0x00, 6);
    } else {
        build_data(ohms, data);
    }
    apply_output(data, ohms);
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

    if (!connected) {
        /* Disconnect: preserve decades but short terminals for safety */
        memcpy(data, relay_state, 6);
        data[5] &= ~(1u << 6);  /* Q6=OFF: CONNECT open  */
        relay_set_raw(data);
        return;
    }

    /* Reconnect: rebuild from current_resistance to handle 0Ω correctly */
    if (current_resistance == 0 || current_resistance == RELAY_RESISTANCE_UNKNOWN) {
        memcpy(data, relay_state, 6);
    } else {
        build_data(current_resistance, data);
    }
    apply_output(data, current_resistance == RELAY_RESISTANCE_UNKNOWN ? 1u : current_resistance);
    relay_set_raw(data);
    HAL_Delay(10);
}

bool relay_get_output(void)
{
    return output_connected;
}

void relay_set_decade(uint8_t decade, uint8_t digit)
{
    if (decade < 1 || decade > 6 || digit > 9) return;
    uint8_t idx = (uint8_t)(6u - decade);
    decade_digits[idx] = digit;

    static const uint32_t scale[6] = {100000, 10000, 1000, 100, 10, 1};
    uint32_t new_ohms = 0;
    for (int i = 0; i < 6; i++)
        new_ohms += (uint32_t)decade_digits[i] * scale[i];
    current_resistance = new_ohms;

    uint8_t data[6];
    if (new_ohms == 0) {
        memset(data, 0x00, 6);
    } else {
        build_data(new_ohms, data);
    }
    apply_output(data, new_ohms);
    relay_set_raw(data);
    HAL_Delay(10);
    output_connected = true;
}

uint8_t relay_get_decade_digit(uint8_t decade)
{
    if (decade < 1 || decade > 6) return 0xFF;
    if (current_resistance == RELAY_RESISTANCE_UNKNOWN) return 0xFF;
    return decade_digits[6u - decade];
}

void relay_set_bit(uint8_t decade, uint8_t bit, bool on)
{
    if (decade < 1 || decade > 6 || bit > 5) return;
    uint8_t idx = (uint8_t)(6u - decade);
    uint8_t data[6];
    memcpy(data, relay_state, 6);
    if (on)
        data[idx] |= (uint8_t)(1u << bit);      /* 1 = relay ON (DRAIN LOW) */
    else
        data[idx] &= (uint8_t)(~(1u << bit));   /* 0 = relay OFF */
    current_resistance = RELAY_RESISTANCE_UNKNOWN;
    relay_set_raw(data);
}

bool relay_get_bit(uint8_t decade, uint8_t bit)
{
    if (decade < 1 || decade > 6 || bit > 5) return false;
    uint8_t idx = (uint8_t)(6u - decade);
    return !!(relay_state[idx] & (1u << bit));  /* bit=1 → relay ON */
}
