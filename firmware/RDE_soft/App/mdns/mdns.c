/*
 * mdns.c – minimal mDNS responder, VXI-11 service discovery
 *
 * Implements just enough mDNS (RFC 6762) to allow NI-MAX "Find Instruments"
 * to locate the RDE device on the local network via service type _vxi-11._tcp.
 *
 * Only responds to PTR queries. Sends a multicast response with PTR + SRV + A + TXT.
 *
 * W5500 multicast socket setup (Socket 4):
 *   setSn_DIPR / setSn_DHAR → multicast group
 *   socket(..., Sn_MR_UDP | Sn_MR_MULTI, 5353, 0) → IGMP join is automatic
 *
 * DNS wire format used:
 *   Labels: length-prefixed (e.g. "\x04_tcp\x05local\x00")
 *   Pointers: 0xC0 | offset  (compression)
 *   All multi-byte values: big-endian
 */

#include "mdns.h"
#include "utils/utils.h"
#include "socket.h"
#include "wizchip_conf.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

/* ── Socket & addressing ────────────────────────────────────────────────── */

#define SOCK_MDNS        4u
#define PORT_MDNS        5353u

static const uint8_t MDNS_IP[4]  = {224, 0, 0, 251};
static const uint8_t MDNS_MAC[6] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB};

/* ── State ──────────────────────────────────────────────────────────────── */

static uint8_t  g_device_ip[4];
static char     g_name[16];          /* "RDE-XXXXXXXX\0" */
static bool     g_open = false;

/* ── DNS constants ──────────────────────────────────────────────────────── */

#define DNS_FLAG_QR_RESPONSE   0x8400u  /* response, authoritative */
#define DNS_TYPE_PTR           0x000Cu
#define DNS_TYPE_SRV           0x0021u
#define DNS_TYPE_A             0x0001u
#define DNS_TYPE_TXT           0x0010u
#define DNS_CLASS_IN           0x0001u
#define DNS_CLASS_IN_FLUSH     0x8001u  /* cache-flush bit for unique records */
#define DNS_TTL_SHORT          120u     /* 2 minutes */

/* ── Buffer helpers ─────────────────────────────────────────────────────── */

static uint8_t tx_buf[512];
static uint8_t rx_buf[512];

static uint16_t buf_pos;

static void buf_init(void)  { buf_pos = 0; }
static void put_u8(uint8_t v)  { if (buf_pos < sizeof(tx_buf)) tx_buf[buf_pos++] = v; }
static void put_u16(uint16_t v) { put_u8((uint8_t)(v >> 8)); put_u8((uint8_t)v); }
static void put_u32(uint32_t v) { put_u16((uint16_t)(v >> 16)); put_u16((uint16_t)v); }

/* Write a DNS name label (ASCII string, no dots, max 63 chars) */
static void put_label(const char *s)
{
    uint8_t len = (uint8_t)strlen(s);
    put_u8(len);
    for (uint8_t i = 0; i < len; i++) put_u8((uint8_t)s[i]);
}

/* Write a DNS name pointer (2-byte compressed ref to offset) */
static void put_ptr(uint16_t offset) { put_u16((uint16_t)(0xC000u | offset)); }

/* Write a complete FQDN for the service type: _vxi-11._tcp.local */
static void put_service_fqdn(void)
{
    put_label("_vxi-11");
    put_label("_tcp");
    put_label("local");
    put_u8(0);   /* root */
}

/* Write instance FQDN: <name>._vxi-11._tcp.local */
static void put_instance_fqdn(void)
{
    put_label(g_name);
    put_label("_vxi-11");
    put_label("_tcp");
    put_label("local");
    put_u8(0);
}

/* Write host FQDN: <name>.local */
static void put_host_fqdn(void)
{
    put_label(g_name);
    put_label("local");
    put_u8(0);
}

/* ── Query parser ───────────────────────────────────────────────────────── */

/* Returns true if the packet contains a PTR query for "_vxi-11._tcp.local" */
static bool query_wants_vxi11(const uint8_t *pkt, uint16_t len)
{
    if (len < 12) return false;

    /* DNS header: ID(2) FLAGS(2) QDCOUNT(2) ANCOUNT(2) NSCOUNT(2) ARCOUNT(2) */
    uint16_t flags   = (uint16_t)((pkt[2] << 8) | pkt[3]);
    uint16_t qdcount = (uint16_t)((pkt[4] << 8) | pkt[5]);

    if (flags & 0x8000u) return false;  /* ignore responses */
    if (qdcount == 0) return false;

    /* Walk questions looking for _vxi-11._tcp.local PTR */
    uint16_t pos = 12;
    for (uint16_t q = 0; q < qdcount && pos < len; q++) {
        /* Decode name labels */
        char name[64];
        uint16_t np = 0;
        while (pos < len) {
            uint8_t llen = pkt[pos++];
            if (llen == 0) break;
            if ((llen & 0xC0) == 0xC0) { pos++; break; }  /* pointer – skip */
            if (llen > 63 || pos + llen > len) return false;
            if (np > 0 && np < (uint16_t)(sizeof(name) - 1)) name[np++] = '.';
            for (uint8_t i = 0; i < llen && np < (uint16_t)(sizeof(name) - 1); i++)
                name[np++] = (char)pkt[pos++];
        }
        name[np] = '\0';

        if (pos + 4 > len) return false;
        uint16_t qtype  = (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
        pos += 4;   /* skip QTYPE + QCLASS */

        if (qtype == DNS_TYPE_PTR &&
            (strcmp(name, "_vxi-11._tcp.local") == 0 ||
             strcmp(name, "_vxi-11._tcp") == 0))
            return true;
    }
    return false;
}

/* ── Response builder ───────────────────────────────────────────────────── */

static uint16_t build_response(uint16_t xid)
{
    buf_init();

    /* ── DNS header ── */
    put_u16(xid);
    put_u16(DNS_FLAG_QR_RESPONSE);
    put_u16(0);    /* QDCOUNT = 0 (mDNS responses have no question section) */
    put_u16(1);    /* ANCOUNT = 1  (PTR) */
    put_u16(0);    /* NSCOUNT = 0 */
    put_u16(3);    /* ARCOUNT = 3  (SRV + A + TXT) */

    /* ─── Answer: PTR record ───────────────────────────────────────────────
     * _vxi-11._tcp.local  PTR  <name>._vxi-11._tcp.local  TTL=120
     * Record the offset of the service FQDN for later compression. */

    uint16_t off_service = buf_pos;      /* offset of "_vxi-11._tcp.local" */
    put_service_fqdn();
    put_u16(DNS_TYPE_PTR);
    put_u16(DNS_CLASS_IN);
    put_u32(DNS_TTL_SHORT);
    /* RDLENGTH placeholder */
    uint16_t rdlen_pos = buf_pos;
    put_u16(0);
    uint16_t off_instance = buf_pos;     /* offset of "<name>._vxi-11._tcp.local" */
    put_instance_fqdn();
    uint16_t rdlen = (uint16_t)(buf_pos - (rdlen_pos + 2));
    tx_buf[rdlen_pos]     = (uint8_t)(rdlen >> 8);
    tx_buf[rdlen_pos + 1] = (uint8_t)(rdlen);

    /* ─── Additional: SRV record ──────────────────────────────────────────
     * <name>._vxi-11._tcp.local  SRV  0 0 703  <name>.local  TTL=120 */

    put_ptr(off_instance);               /* compressed instance FQDN */
    put_u16(DNS_TYPE_SRV);
    put_u16(DNS_CLASS_IN_FLUSH);
    put_u32(DNS_TTL_SHORT);
    uint16_t srv_rdlen_pos = buf_pos;
    put_u16(0);
    put_u16(0);                          /* priority */
    put_u16(0);                          /* weight   */
    put_u16(703u);                       /* port     */
    uint16_t off_host = buf_pos;
    put_host_fqdn();
    uint16_t srv_rdlen = (uint16_t)(buf_pos - (srv_rdlen_pos + 2));
    tx_buf[srv_rdlen_pos]     = (uint8_t)(srv_rdlen >> 8);
    tx_buf[srv_rdlen_pos + 1] = (uint8_t)(srv_rdlen);

    /* ─── Additional: A record ────────────────────────────────────────────
     * <name>.local  A  <device_ip>  TTL=120 */

    put_ptr(off_host);
    put_u16(DNS_TYPE_A);
    put_u16(DNS_CLASS_IN_FLUSH);
    put_u32(DNS_TTL_SHORT);
    put_u16(4);                          /* RDLENGTH */
    put_u8(g_device_ip[0]);
    put_u8(g_device_ip[1]);
    put_u8(g_device_ip[2]);
    put_u8(g_device_ip[3]);

    /* ─── Additional: TXT record ──────────────────────────────────────────
     * <name>._vxi-11._tcp.local  TXT  "txtvers=1"  TTL=120 */

    put_ptr(off_instance);
    put_u16(DNS_TYPE_TXT);
    put_u16(DNS_CLASS_IN_FLUSH);
    put_u32(DNS_TTL_SHORT);
    const char *txt = "\x09txtvers=1";  /* 1-byte length prefix + value */
    uint16_t txt_rdlen = (uint16_t)strlen(txt);
    put_u16(txt_rdlen);
    for (uint16_t i = 0; i < txt_rdlen; i++) put_u8((uint8_t)txt[i]);

    (void)off_service;   /* used only for documentation of offsets */
    return buf_pos;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void mdns_init(const uint8_t device_ip[4])
{
    memcpy(g_device_ip, device_ip, 4);
    snprintf(g_name, sizeof(g_name), "RDE-%s", serial_get());

    setSn_DIPR(SOCK_MDNS, (uint8_t *)MDNS_IP);
    setSn_DHAR(SOCK_MDNS, (uint8_t *)MDNS_MAC);
    socket(SOCK_MDNS, Sn_MR_UDP | Sn_MR_MULTI, PORT_MDNS, 0);
    g_open = true;
}

void mdns_task(void)
{
    if (!g_open) return;

    /* Do NOT reopen here – socket() has an internal busy-wait that blocks the
     * main loop indefinitely if the W5500 multicast socket fails to open.
     * If the socket closed unexpectedly, mDNS is silently disabled until the
     * next full network restart (vxi11_server_close → vxi11_server_init). */
    if (getSn_SR(SOCK_MDNS) != SOCK_UDP) return;

    int32_t rx_len = getSn_RX_RSR(SOCK_MDNS);
    if (rx_len <= 0) return;
    if (rx_len > (int32_t)sizeof(rx_buf)) rx_len = sizeof(rx_buf);

    uint8_t  peer_ip[4];
    uint16_t peer_port;
    recvfrom(SOCK_MDNS, rx_buf, (uint16_t)rx_len, peer_ip, &peer_port);

    if (!query_wants_vxi11(rx_buf, (uint16_t)rx_len)) return;

    uint16_t xid = (uint16_t)((rx_buf[0] << 8) | rx_buf[1]);
    uint16_t rlen = build_response(xid);
    sendto(SOCK_MDNS, tx_buf, rlen, (uint8_t *)MDNS_IP, PORT_MDNS);
}

void mdns_close(void)
{
    if (g_open) {
        close(SOCK_MDNS);
        g_open = false;
    }
}
