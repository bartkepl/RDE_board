/*
 * w5500_net.c – W5500 Ethernet non-blocking state machine for RDE_board
 *
 * SPI2 = W5500 at 12.5 Mbps, Mode 0, software CS on PB12.
 * MCO1 (PA8) outputs HSE/1 = 25 MHz as W5500_CLK (set in SystemClock_Config).
 *
 * w5500_net_init() – registers SPI callbacks, starts async reset sequence
 * w5500_net_task() – call every main-loop iteration; drives state machine
 *
 * State flow:
 *   RESET_ASSERT → (10 ms) → RESET_DEASSERT → (50 ms) → CHECK_CHIP
 *   CHECK_CHIP: VERSIONR==0x04 → CHIP_INIT : ERROR (retry 2 s)
 *   CHIP_INIT → WAIT_LINK → (link ON) → DHCP_START → DHCP_RUN
 *   DHCP_RUN → (acquired or 5 s) → APPLY_IP → OPEN_SOCKETS → RUNNING
 *   RUNNING → (link lost) → LINK_DOWN → (link ON) → DHCP_START
 */

#include "w5500_net.h"
#include "net_config.h"
#include "main.h"
#include "utils/utils.h"
#include "vxi11/vxi11_server.h"
#include "wizchip_conf.h"
#include "socket.h"
#include "dhcp.h"
#include "stm32g4xx_hal.h"
#include <string.h>

extern SPI_HandleTypeDef hspi2;

/* ── SPI / chip-select callbacks ────────────────────────────────────────── */

static void w5500_cs_select(void)       { HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_RESET); }
static void w5500_cs_deselect(void)     { HAL_GPIO_WritePin(W5500_CS_GPIO_Port, W5500_CS_Pin, GPIO_PIN_SET);   }
static void w5500_cris_enter(void)      { __disable_irq(); }
static void w5500_cris_exit(void)       { __enable_irq();  }
static uint8_t w5500_spi_read(void)    { uint8_t b = 0; HAL_SPI_Receive(&hspi2, &b, 1, 10); return b; }
static void w5500_spi_write(uint8_t b) { HAL_SPI_Transmit(&hspi2, &b, 1, 10); }

/* ── State machine ──────────────────────────────────────────────────────── */

typedef enum {
    W5500_ST_RESET_ASSERT = 0,  /* drive RST low */
    W5500_ST_RESET_DEASSERT,    /* release RST after 10 ms */
    W5500_ST_BOOT_WAIT,         /* wait 50 ms for W5500 internal boot */
    W5500_ST_CHECK_CHIP,        /* read VERSIONR – must be 0x04 */
    W5500_ST_CHIP_INIT,         /* wizchip_init + build MAC */
    W5500_ST_WAIT_LINK,         /* poll PHY link, no timeout */
    W5500_ST_LINK_SETTLE,       /* wait 1500 ms after link-up (STP convergence) */
    W5500_ST_DHCP_START,        /* DHCP_init */
    W5500_ST_DHCP_RUN,          /* DHCP_run per tick + 12 s timeout */
    W5500_ST_APPLY_IP,          /* commit DHCP or static IP */
    W5500_ST_OPEN_SOCKETS,      /* vxi11_server_init */
    W5500_ST_RUNNING,           /* DHCP renewal + VXI-11 service */
    W5500_ST_LINK_DOWN,         /* cable removed, wait for link */
    W5500_ST_ERROR,             /* chip not responding, retry after 2 s */
} w5500_state_t;

static w5500_state_t state         = W5500_ST_RESET_ASSERT;
static uint32_t      state_tick    = 0;
static uint32_t      dhcp_tick_ms  = 0;
static bool          dhcp_acquired = false;

/* ── DHCP ───────────────────────────────────────────────────────────────── */

#define DHCP_SOCKET      0
#define DHCP_TIMEOUT_MS  12000u

static uint8_t dhcp_buf[548];
static bool    dhcp_done = false;

static void dhcp_ip_assigned(void) { dhcp_done = true; }
static void dhcp_ip_conflict(void) { }

/* ── Network configuration ──────────────────────────────────────────────── */

static wiz_NetInfo net_info = {
    .mac  = {0x02, 0x08, 0xDC, 0x00, 0x00, 0x00},
    .ip   = {192, 168, 1, 50},
    .sn   = {255, 255, 255, 0},
    .gw   = {192, 168, 1, 1},
    .dns  = {8, 8, 8, 8},
    .dhcp = NETINFO_DHCP,
};

static void build_mac_from_uid(uint8_t mac[6])
{
    const char *s = serial_get_full();
    mac[0] = 0x00;   /* WIZnet OUI (globally administered) – better DHCP compat */
    mac[1] = 0x08;
    mac[2] = 0xDC;
    for (int i = 0; i < 3; i++) {
        uint8_t hi = (s[i*2]   >= 'A') ? (uint8_t)(s[i*2]   - 'A' + 10) : (uint8_t)(s[i*2]   - '0');
        uint8_t lo = (s[i*2+1] >= 'A') ? (uint8_t)(s[i*2+1] - 'A' + 10) : (uint8_t)(s[i*2+1] - '0');
        mac[3+i] = (uint8_t)((hi << 4) | lo);
    }
}

/* ── Gratuitous ARP (ARP Announcement) ─────────────────────────────────── */

/* Send an ARP Announcement so the router and other hosts update their ARP
 * tables immediately after we apply our IP address.
 *
 * ARP Probe  (sent by DHCP library): Sender IP = 0.0.0.0, Target IP = X
 * ARP Announcement (this function):  Sender IP = X,       Target IP = X
 *
 * Uses Socket 7 (MACRAW) temporarily; closed after sending. */
static void send_gratuitous_arp(void)
{
    uint8_t mac[6], ip[4];
    getSHAR(mac);
    getSIPR(ip);

    /* Build raw Ethernet frame (60 bytes, zero-padded) */
    uint8_t frame[60];
    memset(frame, 0, sizeof(frame));

    /* Ethernet header */
    memset(frame + 0,  0xFF, 6);          /* Dst MAC = broadcast */
    memcpy(frame + 6,  mac,  6);          /* Src MAC = our MAC   */
    frame[12] = 0x08; frame[13] = 0x06;  /* EtherType = ARP     */

    /* ARP payload */
    frame[14] = 0x00; frame[15] = 0x01;  /* HTYPE = Ethernet  */
    frame[16] = 0x08; frame[17] = 0x00;  /* PTYPE = IPv4      */
    frame[18] = 0x06;                     /* HLEN = 6          */
    frame[19] = 0x04;                     /* PLEN = 4          */
    frame[20] = 0x00; frame[21] = 0x01;  /* OPER = Request    */
    memcpy(frame + 22, mac, 6);           /* SHA = our MAC     */
    memcpy(frame + 28, ip,  4);           /* SPA = our IP      */
    memset(frame + 32, 0xFF, 6);          /* THA = broadcast   */
    memcpy(frame + 38, ip,  4);           /* TPA = our IP (same as SPA = Announcement) */

    /* W5500 MACRAW is only supported on socket 0.
     * Close socket 0 (may be the DHCP UDP socket) temporarily.
     *
     * We bypass the WIZnet library socket() here because it has an internal
     * busy-wait (while getSn_SR == SOCK_CLOSED) with no timeout that would
     * block the main loop indefinitely if W5500 does not acknowledge. */
    close(DHCP_SOCKET);

    /* Direct register writes: set mode = MACRAW, issue OPEN command */
    setSn_MR(DHCP_SOCKET, Sn_MR_MACRAW);
    setSn_CR(DHCP_SOCKET, Sn_CR_OPEN);

    /* Wait max 5 ms for the socket to enter MACRAW state */
    uint32_t t0 = HAL_GetTick();
    while (getSn_SR(DHCP_SOCKET) != SOCK_MACRAW) {
        if ((HAL_GetTick() - t0) >= 5u) {
            close(DHCP_SOCKET);
            return;   /* W5500 not responding – skip GARP */
        }
    }

    send(DHCP_SOCKET, frame, sizeof(frame));
    HAL_Delay(5);
    close(DHCP_SOCKET);
}

/* ── State machine helpers ──────────────────────────────────────────────── */

static void enter_state(w5500_state_t s)
{
    state      = s;
    state_tick = HAL_GetTick();
}

static bool elapsed(uint32_t ms)
{
    return (HAL_GetTick() - state_tick) >= ms;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void w5500_net_init(void)
{
    /* Register callbacks – no SPI activity, non-blocking */
    reg_wizchip_cris_cbfunc(w5500_cris_enter, w5500_cris_exit);
    reg_wizchip_cs_cbfunc(w5500_cs_select, w5500_cs_deselect);
    reg_wizchip_spi_cbfunc(w5500_spi_read, w5500_spi_write);
    w5500_cs_deselect();   /* ensure CS is idle-high */
    enter_state(W5500_ST_RESET_ASSERT);
}

void w5500_net_task(void)
{
    switch (state) {

    /* ── Hardware reset ── */
    case W5500_ST_RESET_ASSERT:
        HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_RESET);
        enter_state(W5500_ST_RESET_DEASSERT);
        break;

    case W5500_ST_RESET_DEASSERT:
        if (elapsed(10)) {
            HAL_GPIO_WritePin(W5500_RST_GPIO_Port, W5500_RST_Pin, GPIO_PIN_SET);
            enter_state(W5500_ST_BOOT_WAIT);
        }
        break;

    case W5500_ST_BOOT_WAIT:
        if (elapsed(50)) {
            enter_state(W5500_ST_CHECK_CHIP);
        }
        break;

    case W5500_ST_CHECK_CHIP:
        if (getVERSIONR() == 0x04) {
            enter_state(W5500_ST_CHIP_INIT);
        } else {
            enter_state(W5500_ST_ERROR);
        }
        break;

    case W5500_ST_CHIP_INIT: {
        uint8_t tx[8] = {2, 2, 2, 2, 2, 2, 2, 2};
        uint8_t rx[8] = {2, 2, 2, 2, 2, 2, 2, 2};
        wizchip_init(tx, rx);

        /* PHY speed – configured at runtime from net_config_t.phy_mode (FRAM).
         * PHYCFGR bits: [7]=RST [6]=OPMD [5]=DPX [4]=SPD
         *   reset phase : RST=0, OPMD=1, desired SPD/DPX
         *   run phase   : RST=1, OPMD=1, desired SPD/DPX */
        /* PHYCFGR encoding:
         *   bit 7   = RST   (0 assert, 1 release)
         *   bit 6   = OPMD  (1 = override PMODE pins with OPMDC)
         *   bits 5..3 = OPMDC: 000=10BT HD  011=100BT FD  111=all-auto  110=POWER DOWN
         * IMPORTANT: do NOT use 0x70/0xF0 — that encodes OPMDC=110 = power down. */
        switch (net_config_get()->phy_mode) {
        case NET_PHY_10M_HD:
            /* OPMD=1, OPMDC=000 (10BT HD, no auto-neg) */
            setPHYCFGR(0x40); HAL_Delay(2); setPHYCFGR(0xC0); HAL_Delay(100);
            break;
        case NET_PHY_100M_FD:
            /* OPMD=1, OPMDC=011 (100BT FD, no auto-neg) */
            setPHYCFGR(0x58); HAL_Delay(2); setPHYCFGR(0xD8); HAL_Delay(100);
            break;
        default: /* NET_PHY_AUTO – HW reset restores OPMD=0 → PMODE pins drive auto-neg */
            break;
        }

        build_mac_from_uid(net_info.mac);
        /* Seed net_info with config static address (DHCP will override if acquired) */
        const net_config_t *cfg = net_config_get();
        memcpy(net_info.ip, cfg->ip, 4);
        memcpy(net_info.sn, cfg->sn, 4);
        memcpy(net_info.gw, cfg->gw, 4);
        net_info.dhcp = NETINFO_DHCP;
        wizchip_setnetinfo(&net_info);
        enter_state(W5500_ST_WAIT_LINK);
        break;
    }

    /* ── PHY link – no timeout, cable may be absent at power-up ── */
    case W5500_ST_WAIT_LINK:
        if (wizphy_getphylink() == PHY_LINK_ON) {
            enter_state(W5500_ST_LINK_SETTLE);
        }
        break;

    /* ── Link settle – wait for switch STP to converge before DHCP ── */
    case W5500_ST_LINK_SETTLE:
        if (!elapsed(1500)) {
            /* keep polling – bail out if link drops during settle */
            if (wizphy_getphylink() != PHY_LINK_ON) {
                enter_state(W5500_ST_WAIT_LINK);
            }
        } else {
            /* Skip DHCP entirely when use_dhcp==0 – go straight to static IP */
            if (net_config_get()->use_dhcp) {
                enter_state(W5500_ST_DHCP_START);
            } else {
                dhcp_done = false;   /* flag as not acquired → static fallback */
                enter_state(W5500_ST_APPLY_IP);
            }
        }
        break;

    /* ── DHCP acquisition ── */
    case W5500_ST_DHCP_START: {
        /* RFC 2131: DHCP DISCOVER must be sent from 0.0.0.0, not from our static IP.
         * wizchip_setnetinfo() already set SIPR to the static fallback – clear it now. */
        uint8_t zero[4] = {0, 0, 0, 0};
        setSIPR(zero);
        dhcp_done    = false;
        dhcp_tick_ms = HAL_GetTick();
        DHCP_init(DHCP_SOCKET, dhcp_buf);
        reg_dhcp_cbfunc(dhcp_ip_assigned, dhcp_ip_assigned, dhcp_ip_conflict);
        enter_state(W5500_ST_DHCP_RUN);
        break;
    }

    case W5500_ST_DHCP_RUN: {
        /* Rate-limit DHCP_run() to once every 50 ms.
         * Calling it every loop iteration causes thousands of getSn_SR() SPI reads
         * per second.  A single bad read (SPI noise) triggers socket re-open which
         * flushes the RX buffer and loses the DHCP OFFER. */
        static uint32_t dhcp_run_ms = 0;
        uint32_t now = HAL_GetTick();

        if (now - dhcp_tick_ms >= 1000u) {
            dhcp_tick_ms = now;
            DHCP_time_handler();
        }
        if (!dhcp_done && (now - dhcp_run_ms >= 50u)) {
            dhcp_run_ms = now;
            DHCP_run();
        }
        if (dhcp_done || elapsed(DHCP_TIMEOUT_MS)) {
            enter_state(W5500_ST_APPLY_IP);
        }
        break;
    }

    case W5500_ST_APPLY_IP:
        if (dhcp_done) {
            getIPfromDHCP(net_info.ip);
            getSNfromDHCP(net_info.sn);
            getGWfromDHCP(net_info.gw);
            net_info.dhcp = NETINFO_DHCP;
        } else {
            /* Static fallback: use net_config values */
            const net_config_t *cfg = net_config_get();
            memcpy(net_info.ip, cfg->ip, 4);
            memcpy(net_info.sn, cfg->sn, 4);
            memcpy(net_info.gw, cfg->gw, 4);
            net_info.dhcp = NETINFO_STATIC;
            close(DHCP_SOCKET);   /* release DHCP socket */
        }
        wizchip_setnetinfo(&net_info);
        dhcp_acquired = dhcp_done;

        /* ARP Announcement: Sender IP = Target IP = our IP.
         * Must be sent AFTER wizchip_setnetinfo() sets SIPR.
         * Uses socket 0 in MACRAW mode (W5500 restriction: MACRAW = socket 0 only). */
        send_gratuitous_arp();

        /* Reopen socket 0 for DHCP renewal if we acquired a lease.
         * socket() here re-opens it as UDP without resetting the DHCP state machine. */
        if (dhcp_done) {
            socket(DHCP_SOCKET, Sn_MR_UDP, DHCP_CLIENT_PORT, 0);
        }

        enter_state(W5500_ST_OPEN_SOCKETS);
        break;

    case W5500_ST_OPEN_SOCKETS:
        vxi11_server_init();
        dhcp_tick_ms = HAL_GetTick();
        enter_state(W5500_ST_RUNNING);
        break;

    /* ── Normal operation ── */
    case W5500_ST_RUNNING: {
        /* DHCP renewal (only when DHCP was acquired) */
        if (dhcp_acquired) {
            uint32_t now = HAL_GetTick();
            if (now - dhcp_tick_ms >= 1000u) {
                dhcp_tick_ms = now;
                DHCP_time_handler();
            }
            uint8_t res = DHCP_run();
            if (res == DHCP_IP_ASSIGN || res == DHCP_IP_CHANGED) {
                getIPfromDHCP(net_info.ip);
                getSNfromDHCP(net_info.sn);
                getGWfromDHCP(net_info.gw);
                wizchip_setnetinfo(&net_info);
            }
        }

        /* Link check every 500 ms, 3 consecutive failures required (1.5 s).
         * Prevents false link-down from SPI noise resetting SIPR to 0.0.0.0. */
        static uint32_t link_chk_ms = 0;
        static uint8_t  link_miss   = 0;
        if (HAL_GetTick() - link_chk_ms >= 500u) {
            link_chk_ms = HAL_GetTick();
            if (wizphy_getphylink() != PHY_LINK_ON) {
                if (++link_miss >= 3) {
                    link_miss = 0;
                    vxi11_server_close();
                    enter_state(W5500_ST_LINK_DOWN);
                    break;
                }
            } else {
                link_miss = 0;
            }
        }

        vxi11_server_task();
        break;
    }

    /* ── Link-loss recovery – LINK_SETTLE before DHCP for STP convergence ── */
    case W5500_ST_LINK_DOWN:
        if (wizphy_getphylink() == PHY_LINK_ON) {
            enter_state(W5500_ST_LINK_SETTLE);
        }
        break;

    /* ── Chip not responding – retry after 2 s ── */
    case W5500_ST_ERROR:
        if (elapsed(2000u)) {
            enter_state(W5500_ST_RESET_ASSERT);
        }
        break;

    default:
        break;
    }
}

bool w5500_net_is_linked(void)
{
    return (state == W5500_ST_RUNNING);
}

void w5500_net_get_ip(uint8_t ip[4])
{
    memcpy(ip, net_info.ip, 4);
}

void w5500_net_restart(void)
{
    /* Close all sockets and restart the state machine from reset.
     * Call after net_config fields have been updated. */
    vxi11_server_close();
    dhcp_acquired = false;
    enter_state(W5500_ST_RESET_ASSERT);
}
