/*
 * scpi_def.c – SCPI command definitions for RDE_board (Resistance DEcade)
 */

#include "scpi_def.h"
#include "stm32g4xx_hal.h"
#include "relay/relay_ctrl.h"
#include "utils/utils.h"
#include "usbtmc_app.h"
#include <string.h>

#define SCPI_PUSH_ERR(ctx, code)  do { SCPI_ErrorPush((ctx), (code)); return SCPI_RES_ERR; } while(0)

/* ===== Shared reply buffer ===== */
char     scpi_reply_buf[512];
uint16_t scpi_reply_len  = 0;
bool     scpi_reply_ready = false;

/* ===== Forward declarations ===== */
static size_t        SCPI_Write(scpi_t *context, const char *data, size_t len);
static int           SCPI_Error(scpi_t *context, int_fast16_t err);
static scpi_result_t SCPI_Reset(scpi_t *context);

static scpi_result_t My_CoreTstQ(scpi_t *context);
static scpi_result_t SCPI_ResistanceValueSet(scpi_t *context);
static scpi_result_t SCPI_ResistanceValueQ(scpi_t *context);
static scpi_result_t SCPI_OutputStateSet(scpi_t *context);
static scpi_result_t SCPI_OutputStateQ(scpi_t *context);
static scpi_result_t SCPI_SystemBootloaderEnter(scpi_t *context);
static scpi_result_t SCPI_SystemReset(scpi_t *context);
static scpi_result_t SCPI_SystemIdQ(scpi_t *context);

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

    /* Resistance decade control */
    { .pattern = "RESistance:VALue",  .callback = SCPI_ResistanceValueSet, },
    { .pattern = "RESistance:VALue?", .callback = SCPI_ResistanceValueQ,   },

    /* Output terminal relay */
    { .pattern = "OUTPut:STATe",  .callback = SCPI_OutputStateSet, },
    { .pattern = "OUTPut:STATe?", .callback = SCPI_OutputStateQ,   },

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
    /* No background errors at the moment */
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
    (void)err;
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

static scpi_result_t My_CoreTstQ(scpi_t *context)
{
    SCPI_ResultInt32(context, 0);
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

/* RESistance:VALue?  –  returns current resistance in ohms */
static scpi_result_t SCPI_ResistanceValueQ(scpi_t *context)
{
    SCPI_ResultUInt32(context, relay_get_resistance());
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

    __enable_irq();
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
