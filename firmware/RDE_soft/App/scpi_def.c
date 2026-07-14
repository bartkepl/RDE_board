/*
 * scpi_def.c – SCPI command definitions for RDE_board (Resistance DEcade)
 */

#include "scpi_def.h"
#include "stm32g4xx_hal.h"
#include "relay/relay_ctrl.h"
#include "relay/relay_cal.h"
#include "net_config.h"
#include "w5500_net.h"
#include "utils/utils.h"
#include "usbtmc_app.h"
#include "fram/fm24c64b.h"
#include <string.h>
#include <stdio.h>

#define SCPI_PUSH_ERR(ctx, code)  do { SCPI_ErrorPush((ctx), (code)); return SCPI_RES_ERR; } while(0)

/* ===== Shared reply buffer ===== */
char     scpi_reply_buf[512];
uint16_t scpi_reply_len  = 0;
bool     scpi_reply_ready = false;

/* ===== Error LED state ===== */
/* Set by SCPI_Error callback; cleared when error queue drains to zero */
static volatile bool scpi_error_active = false;

/* ===== Forward declarations ===== */
static size_t        SCPI_Write(scpi_t *context, const char *data, size_t len);
static int           SCPI_Error(scpi_t *context, int_fast16_t err);
static scpi_result_t SCPI_Reset(scpi_t *context);

static scpi_result_t My_CoreTstQ(scpi_t *context);
static scpi_result_t SCPI_ResistanceValueSet(scpi_t *context);
static scpi_result_t SCPI_ResistanceValueQ(scpi_t *context);
static scpi_result_t SCPI_OutputStateSet(scpi_t *context);
static scpi_result_t SCPI_OutputStateQ(scpi_t *context);
static scpi_result_t SCPI_OutputRelayEnableSet(scpi_t *context);
static scpi_result_t SCPI_OutputRelayEnableQ(scpi_t *context);
static scpi_result_t SCPI_RelayRawSet(scpi_t *context);
static scpi_result_t SCPI_RelayRawQ(scpi_t *context);
static scpi_result_t SCPI_ResistanceDecadeSet(scpi_t *context);
static scpi_result_t SCPI_ResistanceDecadeQ(scpi_t *context);
static scpi_result_t SCPI_RelayStateSet(scpi_t *context);
static scpi_result_t SCPI_RelayStateQ(scpi_t *context);
static scpi_result_t SCPI_NetIpSet(scpi_t *context);
static scpi_result_t SCPI_NetIpQ(scpi_t *context);
static scpi_result_t SCPI_NetMaskSet(scpi_t *context);
static scpi_result_t SCPI_NetMaskQ(scpi_t *context);
static scpi_result_t SCPI_NetGwSet(scpi_t *context);
static scpi_result_t SCPI_NetGwQ(scpi_t *context);
static scpi_result_t SCPI_NetDhcpSet(scpi_t *context);
static scpi_result_t SCPI_NetDhcpQ(scpi_t *context);
static scpi_result_t SCPI_NetPhySet(scpi_t *context);
static scpi_result_t SCPI_NetPhyQ(scpi_t *context);
static scpi_result_t SCPI_NetApply(scpi_t *context);
static scpi_result_t SCPI_NetStateQ(scpi_t *context);
static scpi_result_t SCPI_SystemBootloaderEnter(scpi_t *context);
static scpi_result_t SCPI_SystemReset(scpi_t *context);
static scpi_result_t SCPI_SystemIdQ(scpi_t *context);
static scpi_result_t SCPI_SystemFramPingQ(scpi_t *context);
static scpi_result_t SCPI_SystemFramDiagQ(scpi_t *context);
static scpi_result_t SCPI_SystemI2cScanQ(scpi_t *context);
static scpi_result_t SCPI_CalDecadeSet(scpi_t *context);
static scpi_result_t SCPI_CalDecadeQ(scpi_t *context);
static scpi_result_t SCPI_CalSave(scpi_t *context);
static scpi_result_t SCPI_CalLoad(scpi_t *context);
static scpi_result_t SCPI_CalReset(scpi_t *context);
static scpi_result_t SCPI_CalEnableSet(scpi_t *context);
static scpi_result_t SCPI_CalEnableQ(scpi_t *context);

/* ===== Command list ===== */
static const scpi_command_t scpi_commands[] = {
    /* IEEE 488.2 mandatory commands */
    { .pattern = "*CLS",  .callback = SCPI_CoreCls,  },
    { .pattern = "*ESE",  .callback = SCPI_CoreEse,  },
    { .pattern = "*ESE?", .callback = SCPI_CoreEseQ, },
    { .pattern = "*ESR?", .callback = SCPI_CoreEsrQ, },
    { .pattern = "*IDN?", .callback = SCPI_CoreIdnQ, },
    { .pattern = "*OPC",  .callback = SCPI_CoreOpc,  },
    { .pattern = "*OPC?", .callback = SCPI_CoreOpcQ, },
    { .pattern = "*RST",  .callback = SCPI_CoreRst,  },
    { .pattern = "*SRE",  .callback = SCPI_CoreSre,  },
    { .pattern = "*SRE?", .callback = SCPI_CoreSreQ, },
    { .pattern = "*STB?", .callback = SCPI_CoreStbQ, },
    { .pattern = "*TST?", .callback = My_CoreTstQ,   },
    { .pattern = "*WAI",  .callback = SCPI_CoreWai,  },

    /* Required SCPI commands */
    { .pattern = "SYSTem:ERRor[:NEXT]?", .callback = SCPI_SystemErrorNextQ,  },
    { .pattern = "SYSTem:ERRor:COUNt?",  .callback = SCPI_SystemErrorCountQ, },
    { .pattern = "SYSTem:VERSion?",       .callback = SCPI_SystemVersionQ,    },

    /* System control (from SDT_board) */
    { .pattern = "SYSTem:BOOTloader:ENter", .callback = SCPI_SystemBootloaderEnter, },
    { .pattern = "SYSTem:RST",              .callback = SCPI_SystemReset,           },
    { .pattern = "SYSTem:ID?",              .callback = SCPI_SystemIdQ,             },
    { .pattern = "SYSTem:FRAM:PING?",       .callback = SCPI_SystemFramPingQ,       },
    { .pattern = "SYSTem:FRAM:DIAG?",       .callback = SCPI_SystemFramDiagQ,       },
    { .pattern = "SYSTem:I2C:SCAN?",        .callback = SCPI_SystemI2cScanQ,        },

    /* Resistance decade control */
    { .pattern = "RESistance:VALue",   .callback = SCPI_ResistanceValueSet,  },
    { .pattern = "RESistance:VALue?",  .callback = SCPI_ResistanceValueQ,    },
    { .pattern = "RESistance:DECade",  .callback = SCPI_ResistanceDecadeSet, },
    { .pattern = "RESistance:DECade?", .callback = SCPI_ResistanceDecadeQ,   },

    /* Output terminal relay */
    { .pattern = "OUTPut:STATe",  .callback = SCPI_OutputStateSet, },
    { .pattern = "OUTPut:STATe?", .callback = SCPI_OutputStateQ,   },

    /* Relay PSU power (REL_EN / PA4) */
    { .pattern = "OUTPut:RELay:ENable",  .callback = SCPI_OutputRelayEnableSet, },
    { .pattern = "OUTPut:RELay:ENable?", .callback = SCPI_OutputRelayEnableQ,   },

    /* Raw relay access for testing individual relays */
    { .pattern = "RELay:RAW",    .callback = SCPI_RelayRawSet,   },
    { .pattern = "RELay:RAW?",   .callback = SCPI_RelayRawQ,     },
    { .pattern = "RELay:STATe",  .callback = SCPI_RelayStateSet, },
    { .pattern = "RELay:STATe?", .callback = SCPI_RelayStateQ,   },

    /* Network configuration */
    { .pattern = "NET:IPADdress",  .callback = SCPI_NetIpSet,   },
    { .pattern = "NET:IPADdress?", .callback = SCPI_NetIpQ,     },
    { .pattern = "NET:SMASk",      .callback = SCPI_NetMaskSet, },
    { .pattern = "NET:SMASk?",     .callback = SCPI_NetMaskQ,   },
    { .pattern = "NET:GATEway",    .callback = SCPI_NetGwSet,   },
    { .pattern = "NET:GATEway?",   .callback = SCPI_NetGwQ,     },
    { .pattern = "NET:DHCP",        .callback = SCPI_NetDhcpSet, },
    { .pattern = "NET:DHCP?",       .callback = SCPI_NetDhcpQ,   },
    { .pattern = "NET:PHY:MODE",    .callback = SCPI_NetPhySet,  },
    { .pattern = "NET:PHY:MODE?",   .callback = SCPI_NetPhyQ,    },
    { .pattern = "NET:APPLy",       .callback = SCPI_NetApply,   },
    { .pattern = "NET:STATe?",      .callback = SCPI_NetStateQ,  },

    /* Decade calibration */
    { .pattern = "CALibration:DECade",   .callback = SCPI_CalDecadeSet,   },
    { .pattern = "CALibration:DECade?",  .callback = SCPI_CalDecadeQ,     },
    { .pattern = "CALibration:SAVE",     .callback = SCPI_CalSave,        },
    { .pattern = "CALibration:LOAD",     .callback = SCPI_CalLoad,        },
    { .pattern = "CALibration:RESet",    .callback = SCPI_CalReset,       },
    { .pattern = "CALibration:ENable",   .callback = SCPI_CalEnableSet,   },
    { .pattern = "CALibration:ENable?",  .callback = SCPI_CalEnableQ,     },

    SCPI_CMD_LIST_END
};

/* ===== SCPI interface ===== */
static scpi_interface_t scpi_interface = {
    .write = SCPI_Write,
    .error = SCPI_Error,
    .reset = SCPI_Reset,
};

scpi_t scpi_context;
static char scpi_input_buffer[1024];
static scpi_error_t scpi_error_queue[16];

/* ===== Public API ===== */

void SCPI_Main_Init(void)
{
    SCPI_Init(
        &scpi_context,
        scpi_commands,
        &scpi_interface,
        NULL,
        SCPI_IDN_MANUFACTURER,
        SCPI_IDN_MODEL,
        serial_get(),
        SCPI_IDN_FW,
        scpi_input_buffer,
        sizeof(scpi_input_buffer),
        scpi_error_queue,
        16
    );
}

void SCPI_Main_Input(const char *data, uint32_t len)
{
    scpi_reply_len   = 0;      /* reset accumulation for each new command */
    scpi_reply_ready = false;
    SCPI_Input(&scpi_context, data, len);
}

void SCPI_Main_Poll(void)
{
    /* No background tasks needed; LED_R is driven by main loop via SCPI_Main_HasErrors() */
}

bool SCPI_Main_HasErrors(void)
{
    return scpi_error_active;
}

/* ===== SCPI callbacks ===== */

static size_t SCPI_Write(scpi_t *context, const char *data, size_t len)
{
    (void)context;
    /* Accumulate fragments – libscpi calls Write multiple times per response
     * (content + terminator).  Overwriting from zero gives only the last
     * fragment (usually "\n") which PyVISA strips → empty reply. */
    size_t space = sizeof(scpi_reply_buf) - scpi_reply_len;
    if (len > space) len = space;
    memcpy(scpi_reply_buf + scpi_reply_len, data, len);
    scpi_reply_len  += (uint16_t)len;
    scpi_reply_ready = true;
    setReply(data, len);   /* USB-TMC already accumulates in setReply() */
    return len;
}

static int SCPI_Error(scpi_t *context, int_fast16_t err)
{
    (void)context;
    /* err != 0: new error pushed; err == 0: queue drained (called by SCPI_ErrorEmitEmpty) */
    scpi_error_active = (err != 0);
    return 0;
}

static scpi_result_t SCPI_Reset(scpi_t *context)
{
    (void)context;
    relay_set_resistance(0);
    relay_set_output(true);
    return SCPI_RES_OK;
}

/* ===== Command handlers ===== */

/* *TST? – 0 = all OK, bit 0 set = FRAM not reachable */
static scpi_result_t My_CoreTstQ(scpi_t *context)
{
    int32_t result = 0;
    if (!fm24_ping()) {
        result |= 1;
        SCPI_ErrorPush(context, SCPI_ERROR_HARDWARE_MISSING);
    }
    SCPI_ResultInt32(context, result);
    return SCPI_RES_OK;
}

/* RESistance:VALue <ohms>  –  0 to 999 999 Ω */
static scpi_result_t SCPI_ResistanceValueSet(scpi_t *context)
{
    uint32_t ohms = 0;
    if (!SCPI_ParamUInt32(context, &ohms, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (ohms > 999999u)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);

    relay_set_resistance(ohms);
    return SCPI_RES_OK;
}

/* RESistance:VALue?  –  returns resistance in ohms; if calibration enabled,
 * returns calibrated total as "XXXXXX.XXX" (3 decimal places from milliohms) */
static scpi_result_t SCPI_ResistanceValueQ(scpi_t *context)
{
    if (relay_cal_is_enabled()) {
        uint32_t mo = relay_cal_total_milliohm();
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu.%03lu", (unsigned long)(mo / 1000u), (unsigned long)(mo % 1000u));
        SCPI_ResultCharacters(context, buf, strlen(buf));
    } else {
        SCPI_ResultUInt32(context, relay_get_resistance());
    }
    return SCPI_RES_OK;
}

/* OUTPut:STATe ON|OFF|1|0
 *   ON  → input terminals connected to decade (Q6 closed, Q7 open)
 *   OFF → input terminals disconnected and shorted (Q6 open, Q7 closed)
 */
static scpi_result_t SCPI_OutputStateSet(scpi_t *context)
{
    bool state = true;
    if (!SCPI_ParamBool(context, &state, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);

    relay_set_output(state);
    return SCPI_RES_OK;
}

/* OUTPut:STATe?  –  returns 1 (ON) or 0 (OFF) */
static scpi_result_t SCPI_OutputStateQ(scpi_t *context)
{
    SCPI_ResultBool(context, relay_get_output());
    return SCPI_RES_OK;
}

/* OUTPut:RELay:ENable ON|OFF  –  controls REL_EN (PA4) */
static scpi_result_t SCPI_OutputRelayEnableSet(scpi_t *context)
{
    bool en = false;
    if (!SCPI_ParamBool(context, &en, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    relay_enable(en);
    return SCPI_RES_OK;
}

static scpi_result_t SCPI_OutputRelayEnableQ(scpi_t *context)
{
    SCPI_ResultBool(context, relay_is_enabled());
    return SCPI_RES_OK;
}

/* RELay:RAW <xRy>[,<xRy>,...]
 *
 * Each token is a 3-digit decimal: hundreds = decade (1–6), units = relay (0–5).
 * Mapping:  decade 1 (1 Ω)  → data[5] (STPIC1)
 *           decade 6 (100 kΩ) → data[0] (STPIC6)
 * Q6/Q7 (CONNECT/SHORT) in STPIC1 are preserved from current output state.
 *
 * Examples:
 *   RELay:RAW 103       – decade 1, relay Q3 (gives 3 Ω tap)
 *   RELay:RAW 103,206   – decade 1 Q3 AND decade 2 Q6 simultaneously
 */
static scpi_result_t SCPI_RelayRawSet(scpi_t *context)
{
    uint8_t data[6];
    memset(data, 0x00, sizeof(data));   /* all relays OFF */

    uint32_t val;
    bool got_one = false;
    while (SCPI_ParamUInt32(context, &val, FALSE)) {
        uint8_t decade = (uint8_t)(val / 100u);
        uint8_t relay  = (uint8_t)(val % 10u);
        if (decade < 1 || decade > 6 || relay > 5)
            SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
        /* decade 1 → index 5, decade 6 → index 0 */
        uint8_t idx = (uint8_t)(6u - decade);
        data[idx] |= (uint8_t)(1u << relay);   /* set bit = relay ON */
        got_one = true;
    }
    if (!got_one)
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);

    /* Preserve Q6/Q7 protection relay state from current output_connected flag */
    if (relay_get_output()) {
        data[5] |=  (1u << 6);   /* Q6=ON  → CONNECT closed */
        data[5] &= ~(1u << 7);   /* Q7=OFF → SHORT open     */
    } else {
        data[5] &= ~(1u << 6);   /* Q6=OFF → CONNECT open   */
        data[5] |=  (1u << 7);   /* Q7=ON  → SHORT closed   */
    }

    relay_set_raw(data);
    return SCPI_RES_OK;
}

/* RELay:RAW?  –  returns current relay_state as 6 decimal bytes (b0..b5) */
static scpi_result_t SCPI_RelayRawQ(scpi_t *context)
{
    uint8_t raw[6];
    relay_get_raw(raw);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u,%u,%u,%u,%u,%u",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* RESistance:DECade <1..6>,<0..9>  –  set one decade digit with protection sequence */
static scpi_result_t SCPI_ResistanceDecadeSet(scpi_t *context)
{
    uint32_t decade, digit;
    if (!SCPI_ParamUInt32(context, &decade, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &digit,  TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6 || digit > 9)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    relay_set_decade((uint8_t)decade, (uint8_t)digit);
    return SCPI_RES_OK;
}

/* RESistance:DECade? <1..6>  –  query digit for one decade (returns 0–9 or "UNKN") */
static scpi_result_t SCPI_ResistanceDecadeQ(scpi_t *context)
{
    uint32_t decade;
    if (!SCPI_ParamUInt32(context, &decade, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    uint8_t d = relay_get_decade_digit((uint8_t)decade);
    if (d == 0xFF)
        SCPI_ResultCharacters(context, "UNKN", 4);
    else
        SCPI_ResultUInt32(context, d);
    return SCPI_RES_OK;
}

/* RELay:STATe <1..6>,<0..5>,<ON|OFF|1|0>  –  set single Q-bit, no protection sequence */
static scpi_result_t SCPI_RelayStateSet(scpi_t *context)
{
    uint32_t decade, bit;
    bool on;
    if (!SCPI_ParamUInt32(context, &decade, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &bit,    TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamBool(context,   &on,     TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6 || bit > 5)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    relay_set_bit((uint8_t)decade, (uint8_t)bit, on);
    return SCPI_RES_OK;
}

/* RELay:STATe? <1..6>,<0..5>  –  query single Q-bit state */
static scpi_result_t SCPI_RelayStateQ(scpi_t *context)
{
    uint32_t decade, bit;
    if (!SCPI_ParamUInt32(context, &decade, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &bit,    TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6 || bit > 5)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    SCPI_ResultBool(context, relay_get_bit((uint8_t)decade, (uint8_t)bit));
    return SCPI_RES_OK;
}

/* ── Helper: parse "a.b.c.d" IP string into 4-byte array ────────────────── */
static bool parse_ip(const char *str, uint8_t ip[4])
{
    unsigned a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b;
    ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    return true;
}

/* NET:IPADdress <a.b.c.d>  –  set static IP in RAM config */
static scpi_result_t SCPI_NetIpSet(scpi_t *context)
{
    char str[20];
    size_t len;
    if (!SCPI_ParamCopyText(context, str, sizeof(str), &len, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!parse_ip(str, net_config_get()->ip))
        SCPI_PUSH_ERR(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
    return SCPI_RES_OK;
}

static scpi_result_t SCPI_NetIpQ(scpi_t *context)
{
    const uint8_t *ip = net_config_get()->ip;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* NET:SMASk <a.b.c.d> */
static scpi_result_t SCPI_NetMaskSet(scpi_t *context)
{
    char str[20]; size_t len;
    if (!SCPI_ParamCopyText(context, str, sizeof(str), &len, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!parse_ip(str, net_config_get()->sn))
        SCPI_PUSH_ERR(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
    return SCPI_RES_OK;
}

static scpi_result_t SCPI_NetMaskQ(scpi_t *context)
{
    const uint8_t *sn = net_config_get()->sn;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", sn[0], sn[1], sn[2], sn[3]);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* NET:GATEway <a.b.c.d> */
static scpi_result_t SCPI_NetGwSet(scpi_t *context)
{
    char str[20]; size_t len;
    if (!SCPI_ParamCopyText(context, str, sizeof(str), &len, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!parse_ip(str, net_config_get()->gw))
        SCPI_PUSH_ERR(context, SCPI_ERROR_ILLEGAL_PARAMETER_VALUE);
    return SCPI_RES_OK;
}

static scpi_result_t SCPI_NetGwQ(scpi_t *context)
{
    const uint8_t *gw = net_config_get()->gw;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* NET:DHCP ON|OFF|1|0 */
static scpi_result_t SCPI_NetDhcpSet(scpi_t *context)
{
    bool en = true;
    if (!SCPI_ParamBool(context, &en, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    net_config_get()->use_dhcp = en ? 1u : 0u;
    return SCPI_RES_OK;
}

static scpi_result_t SCPI_NetDhcpQ(scpi_t *context)
{
    SCPI_ResultBool(context, net_config_get()->use_dhcp != 0);
    return SCPI_RES_OK;
}

/* NET:PHY:MODE <0|1|2>  –  set PHY speed mode in RAM config.
 *
 *   0 = NET_PHY_AUTO    (auto-negotiation via PMODE pins)
 *   1 = NET_PHY_10M_HD  (force 10 Mbps half-duplex)
 *   2 = NET_PHY_100M_FD (force 100 Mbps full-duplex)
 *
 * Integer is used (instead of mnemonics like "10M") because libscpi parses
 * "10M"/"100M" as decimal-with-suffix tokens, not as program mnemonics. */
static scpi_result_t SCPI_NetPhySet(scpi_t *context)
{
    uint32_t mode = 0;
    if (!SCPI_ParamUInt32(context, &mode, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (mode > 2u)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    net_config_get()->phy_mode = (uint8_t)mode;
    return SCPI_RES_OK;
}

/* NET:PHY:MODE?  –  returns "AUTO", "10M", or "100M" (human-readable) */
static scpi_result_t SCPI_NetPhyQ(scpi_t *context)
{
    const char *label;
    switch (net_config_get()->phy_mode) {
    case NET_PHY_10M_HD:  label = "10M";  break;
    case NET_PHY_100M_FD: label = "100M"; break;
    default:              label = "AUTO"; break;
    }
    SCPI_ResultCharacters(context, label, strlen(label));
    return SCPI_RES_OK;
}

/* NET:APPLy  –  save config to FRAM + restart W5500 state machine.
 * Pushes SCPI_ERROR_HARDWARE_MISSING if FRAM write/verify fails. */
static scpi_result_t SCPI_NetApply(scpi_t *context)
{
    if (!net_config_save())
        SCPI_PUSH_ERR(context, SCPI_ERROR_HARDWARE_MISSING);
    w5500_net_restart();
    return SCPI_RES_OK;
}

/* NET:STATe?  –  read-only: returns current operational IP */
static scpi_result_t SCPI_NetStateQ(scpi_t *context)
{
    uint8_t ip[4];
    w5500_net_get_ip(ip);
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* ===== System commands ===== */

#define BOOT_ADDR   0x1FFF0000u
#define MCU_IRQS    82u

static scpi_result_t SCPI_SystemBootloaderEnter(scpi_t *context)
{
    (void)context;

    struct boot_vectable_ {
        uint32_t Initial_SP;
        void (*Reset_Handler)(void);
    };
    #define BOOTVTAB ((struct boot_vectable_ *)BOOT_ADDR)

    __disable_irq();
    SysTick->CTRL = 0;
    HAL_RCC_DeInit();

    for (uint8_t i = 0; i < (MCU_IRQS + 31u) / 32u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    __set_MSP(BOOTVTAB->Initial_SP);
    BOOTVTAB->Reset_Handler();

    return SCPI_RES_OK;
}

static scpi_result_t SCPI_SystemReset(scpi_t *context)
{
    (void)context;
    HAL_NVIC_SystemReset();
    return SCPI_RES_OK;
}

typedef enum { SERIAL_OPT_SHORT = 0, SERIAL_OPT_LONG } serial_opt_t;
static const scpi_choice_def_t serial_options[] = {
    { "SHORT", SERIAL_OPT_SHORT },
    { "LONG",  SERIAL_OPT_LONG  },
    SCPI_CHOICE_LIST_END
};

static scpi_result_t SCPI_SystemIdQ(scpi_t *context)
{
    int32_t opt = SERIAL_OPT_SHORT;
    if (!SCPI_ParamChoice(context, serial_options, &opt, FALSE))
        opt = SERIAL_OPT_SHORT;

    if (opt == SERIAL_OPT_LONG)
        SCPI_ResultCharacters(context, serial_get_full(), 40);
    else
        SCPI_ResultCharacters(context, serial_get(), 8);

    return SCPI_RES_OK;
}

/* SYSTem:FRAM:PING?  –  1 if FRAM responds on I2C, 0 if not */
static scpi_result_t SCPI_SystemFramPingQ(scpi_t *context)
{
    SCPI_ResultBool(context, fm24_ping());
    return SCPI_RES_OK;
}

/* SYSTem:FRAM:DIAG?  –  returns "ping,isr,state" for I2C hardware diagnosis
 * ping:  1=FRAM ACK, 0=NACK/timeout
 * isr:   I2C1->ISR register (raw hex) – BUSY,TCR,TC,STOPF,NACKF,ADDR,RXNE,TXIS,TXE
 * state: HAL I2C state (0=READY,1=BUSY,...) */
static scpi_result_t SCPI_SystemFramDiagQ(scpi_t *context)
{
    extern I2C_HandleTypeDef hi2c1;
    uint8_t ping   = fm24_ping();
    uint32_t isr   = hi2c1.Instance->ISR;
    uint32_t state = (uint32_t)HAL_I2C_GetState(&hi2c1);
    char buf[40];
    snprintf(buf, sizeof(buf), "%u,0x%08lX,%lu", ping, (unsigned long)isr, (unsigned long)state);
    SCPI_ResultCharacters(context, buf, strlen(buf));
    return SCPI_RES_OK;
}

/* SYSTem:I2C:SCAN?  –  scan addresses 0x08..0x77, return comma-separated hex list
 * e.g. "0x50,0x51" or "NONE" if nothing found */
static scpi_result_t SCPI_SystemI2cScanQ(scpi_t *context)
{
    extern I2C_HandleTypeDef hi2c1;
    char buf[128];
    uint16_t pos = 0;
    uint8_t found = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr << 1), 1, 10) == HAL_OK) {
            if (found) buf[pos++] = ',';
            pos += (uint16_t)snprintf(buf + pos, sizeof(buf) - pos, "0x%02X", addr);
            found = 1;
        }
    }
    if (!found) {
        memcpy(buf, "NONE", 4);
        pos = 4;
    }
    SCPI_ResultCharacters(context, buf, pos);
    return SCPI_RES_OK;
}

/* ===== Calibration command handlers ===== */

/* CALibration:DECade <1-6>,<0-9>,<milliohm>
 * Store measured milliohm value for the given decade and digit. */
static scpi_result_t SCPI_CalDecadeSet(scpi_t *context)
{
    uint32_t decade, digit, milliohm;
    if (!SCPI_ParamUInt32(context, &decade,   TRUE)) SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &digit,    TRUE)) SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &milliohm, TRUE)) SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6 || digit > 9)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    relay_cal_set((uint8_t)decade, (uint8_t)digit, milliohm);
    return SCPI_RES_OK;
}

/* CALibration:DECade? <1-6>,<0-9>
 * Return stored milliohm value for decade/digit. */
static scpi_result_t SCPI_CalDecadeQ(scpi_t *context)
{
    uint32_t decade, digit;
    if (!SCPI_ParamUInt32(context, &decade, TRUE)) SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (!SCPI_ParamUInt32(context, &digit,  TRUE)) SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    if (decade < 1 || decade > 6 || digit > 9)
        SCPI_PUSH_ERR(context, SCPI_ERROR_DATA_OUT_OF_RANGE);
    SCPI_ResultUInt32(context, relay_cal_get((uint8_t)decade, (uint8_t)digit));
    return SCPI_RES_OK;
}

/* CALibration:SAVE  –  write current calibration data to FRAM */
static scpi_result_t SCPI_CalSave(scpi_t *context)
{
    if (!relay_cal_save())
        SCPI_PUSH_ERR(context, SCPI_ERROR_HARDWARE_MISSING);
    return SCPI_RES_OK;
}

/* CALibration:LOAD  –  re-read calibration data from FRAM */
static scpi_result_t SCPI_CalLoad(scpi_t *context)
{
    (void)context;
    relay_cal_init();
    return SCPI_RES_OK;
}

/* CALibration:RESet  –  restore nominal values in RAM (does NOT write to FRAM) */
static scpi_result_t SCPI_CalReset(scpi_t *context)
{
    (void)context;
    relay_cal_reset();
    return SCPI_RES_OK;
}

/* CALibration:ENable ON|OFF|1|0 */
static scpi_result_t SCPI_CalEnableSet(scpi_t *context)
{
    bool en = false;
    if (!SCPI_ParamBool(context, &en, TRUE))
        SCPI_PUSH_ERR(context, SCPI_ERROR_MISSING_PARAMETER);
    relay_cal_enable(en);
    return SCPI_RES_OK;
}

/* CALibration:ENable?  –  returns 1 or 0 */
static scpi_result_t SCPI_CalEnableQ(scpi_t *context)
{
    SCPI_ResultBool(context, relay_cal_is_enabled());
    return SCPI_RES_OK;
}
