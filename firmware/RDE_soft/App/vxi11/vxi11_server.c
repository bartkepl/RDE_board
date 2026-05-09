/*
 * vxi11_server.c – VXI-11 Core server for RDE_board
 *
 * Implements the minimum VXI-11 procedures needed for NI-VISA / NI-MAX:
 *   - ONC-RPC portmapper (program 100000) on port 111 (TCP + UDP)
 *   - VXI-11 Core (program 0x0607AF) on port 703 (TCP)
 *
 * The portmapper only responds to GETPORT for the VXI-11 Core program.
 * VXI-11 Core handles create_link, device_write, device_read, device_readstb,
 * and destroy_link.
 *
 * Responses are routed through scpi_def.c shared buffer.
 */

#include "vxi11_server.h"
#include "rpc.h"
#include "scpi_def.h"
#include "stm32g4xx_hal.h"

#include "socket.h"       /* WIZnet ioLibrary */
#include <string.h>

/* ===== Socket numbers ===== */
#define SOCK_PMAP_TCP   1
#define SOCK_PMAP_UDP   2
#define SOCK_VXI11_TCP  3

#define PORT_PMAP       111u
#define PORT_VXI11_CORE 703u

/* ===== ONC-RPC program numbers ===== */
#define PROG_PMAP       100000u     /* portmapper */
#define PROG_VXI11_CORE 0x0607AFu   /* VXI-11 Core */
#define PROG_VXI11_ABRT 0x0607B0u   /* VXI-11 Abort (not implemented) */

/* ===== VXI-11 Core procedure numbers ===== */
#define VXI11_CREATE_LINK    10u
#define VXI11_DEVICE_WRITE   11u
#define VXI11_DEVICE_READ    12u
#define VXI11_DEVICE_READSTB 13u
#define VXI11_DEVICE_TRIGGER 14u
#define VXI11_DEVICE_CLEAR   15u
#define VXI11_DESTROY_LINK   23u

/* ===== Portmapper procedure numbers ===== */
#define PMAP_GETPORT    3u

/* ===== IEEE 488.2 status byte bits ===== */
#define STB_MAV  (1u << 4)   /* Message Available */

/* ===== VXI-11 error codes ===== */
#define VXI11_ERR_NO_ERROR   0u
#define VXI11_ERR_IO_TIMEOUT 15u

/* ===== Buffers ===== */
#define BUF_SIZE 1024

static uint8_t rx_buf[BUF_SIZE];
static uint8_t tx_buf[BUF_SIZE + 128];

/* ===== VXI-11 state ===== */
typedef struct {
    bool     link_active;
    uint32_t link_id;
} vxi11_state_t;

static vxi11_state_t vxi11;

/* ===== Portmapper handler ===== */

static void handle_pmap_call(uint8_t sock, const rpc_call_t *call, xdr_t *args,
                              const uint8_t *peer_ip, uint16_t peer_port, bool is_udp)
{
    uint8_t payload[4];
    xdr_t   out;
    xdr_init_write(&out, payload, sizeof(payload));

    if (call->proc == PMAP_GETPORT) {
        uint32_t prog, vers, proto, port_hint;
        xdr_read_u32(args, &prog);
        xdr_read_u32(args, &vers);
        xdr_read_u32(args, &proto);
        xdr_read_u32(args, &port_hint);
        (void)port_hint;

        uint32_t result = 0;
        if (prog == PROG_VXI11_CORE && vers == 1 && proto == 6 /* IPPROTO_TCP */)
            result = PORT_VXI11_CORE;

        xdr_write_u32(&out, result);
    } else {
        /* Unknown procedure – return 0 */
        xdr_write_u32(&out, 0);
    }

    uint32_t rlen = rpc_build_reply(tx_buf, sizeof(tx_buf),
                                     call->xid, payload, xdr_written(&out));
    if (rlen == 0) return;

    if (is_udp) {
        sendto(sock, tx_buf, rlen, (uint8_t *)peer_ip, peer_port);
    } else {
        send(sock, tx_buf, rlen);
    }
}

/* ===== VXI-11 Core handler ===== */

static void handle_vxi11_call(uint8_t sock, const rpc_call_t *call, xdr_t *args)
{
    uint8_t payload[BUF_SIZE];
    xdr_t   out;
    xdr_init_write(&out, payload, sizeof(payload));

    switch (call->proc) {

    case VXI11_CREATE_LINK: {
        /* Input: clientId(u32), lockDevice(bool), lock_timeout(u32), device(string) */
        uint32_t client_id, lock_dev, lock_timeout;
        char     device_str[32];
        xdr_read_u32(args, &client_id);
        xdr_read_u32(args, &lock_dev);
        xdr_read_u32(args, &lock_timeout);
        xdr_read_string(args, device_str, sizeof(device_str));

        vxi11.link_active = true;
        vxi11.link_id     = 1;

        /* Reply: error(u32), lid(u32), abort_port(u16→u32), max_recv_size(u32) */
        xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
        xdr_write_u32(&out, vxi11.link_id);
        xdr_write_u32(&out, 0);       /* abort_port – not used */
        xdr_write_u32(&out, 1024);    /* max_recv_size */
        break;
    }

    case VXI11_DEVICE_WRITE: {
        /* Input: lid(u32), io_timeout(u32), lock_timeout(u32), flags(u32), data(opaque) */
        uint32_t lid, io_timeout, lock_timeout, flags;
        uint8_t  data[512];
        uint32_t data_len = 0;
        xdr_read_u32(args, &lid);
        xdr_read_u32(args, &io_timeout);
        xdr_read_u32(args, &lock_timeout);
        xdr_read_u32(args, &flags);
        xdr_read_opaque(args, data, sizeof(data) - 1, &data_len);
        data[data_len] = '\0';

        /* Pass to SCPI parser */
        scpi_reply_ready = false;
        SCPI_Main_Input((char *)data, data_len);

        /* Reply: error(u32), size(u32) */
        xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
        xdr_write_u32(&out, data_len);
        break;
    }

    case VXI11_DEVICE_READ: {
        /* Input: lid, requestSize, io_timeout, lock_timeout, flags, termChar */
        uint32_t lid, req_size, io_timeout, lock_timeout, flags, term_char;
        xdr_read_u32(args, &lid);
        xdr_read_u32(args, &req_size);
        xdr_read_u32(args, &io_timeout);
        xdr_read_u32(args, &lock_timeout);
        xdr_read_u32(args, &flags);
        xdr_read_u32(args, &term_char);

        /* Wait for SCPI reply (polling with timeout) */
        uint32_t t0 = HAL_GetTick();
        while (!scpi_reply_ready && (HAL_GetTick() - t0) < io_timeout) {
            /* spin – no RTOS */
        }

        /* Reply: error(u32), reason(u32), data(opaque) */
        if (scpi_reply_ready) {
            uint32_t send_len = scpi_reply_len;
            if (send_len > req_size) send_len = req_size;
            xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
            xdr_write_u32(&out, 0x04u);  /* reason: END */
            xdr_write_opaque(&out, (const uint8_t *)scpi_reply_buf, send_len);
            scpi_reply_ready = false;
        } else {
            /* Timeout */
            xdr_write_u32(&out, VXI11_ERR_IO_TIMEOUT);
            xdr_write_u32(&out, 0);
            xdr_write_opaque(&out, NULL, 0);
        }
        break;
    }

    case VXI11_DEVICE_READSTB: {
        /* Input: lid, flags, lock_timeout, io_timeout */
        uint32_t lid, flags, lock_timeout, io_timeout;
        xdr_read_u32(args, &lid);
        xdr_read_u32(args, &flags);
        xdr_read_u32(args, &lock_timeout);
        xdr_read_u32(args, &io_timeout);

        uint8_t stb = scpi_reply_ready ? STB_MAV : 0u;

        /* Reply: error(u32), stb(u32) */
        xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
        xdr_write_u32(&out, stb);
        break;
    }

    case VXI11_DEVICE_TRIGGER:
    case VXI11_DEVICE_CLEAR: {
        /* Acknowledge but do nothing */
        uint32_t lid, flags, lock_timeout, io_timeout;
        xdr_read_u32(args, &lid);
        xdr_read_u32(args, &flags);
        xdr_read_u32(args, &lock_timeout);
        xdr_read_u32(args, &io_timeout);
        xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
        break;
    }

    case VXI11_DESTROY_LINK: {
        uint32_t lid;
        xdr_read_u32(args, &lid);
        vxi11.link_active = false;
        xdr_write_u32(&out, VXI11_ERR_NO_ERROR);
        break;
    }

    default:
        /* Unknown procedure */
        rpc_build_error(tx_buf, sizeof(tx_buf), call->xid, RPC_ACCEPT_PROC_UNAVAIL);
        send(sock, tx_buf, 4 + 24);
        return;
    }

    uint32_t rlen = rpc_build_reply(tx_buf, sizeof(tx_buf),
                                     call->xid, payload, xdr_written(&out));
    if (rlen > 0) send(sock, tx_buf, rlen);
}

/* ===== Public API ===== */

void vxi11_server_init(void)
{
    memset(&vxi11, 0, sizeof(vxi11));
    socket(SOCK_PMAP_TCP,  Sn_MR_TCP, PORT_PMAP,       0);
    socket(SOCK_PMAP_UDP,  Sn_MR_UDP, PORT_PMAP,       0);
    socket(SOCK_VXI11_TCP, Sn_MR_TCP, PORT_VXI11_CORE, 0);
    listen(SOCK_PMAP_TCP);
    listen(SOCK_VXI11_TCP);
}

void vxi11_server_close(void)
{
    close(SOCK_PMAP_TCP);
    close(SOCK_PMAP_UDP);
    close(SOCK_VXI11_TCP);
    vxi11.link_active = false;
}

void vxi11_server_task(void)
{
    /* ─── Portmapper TCP ─── */
    uint8_t status = getSn_SR(SOCK_PMAP_TCP);
    if (status == SOCK_ESTABLISHED) {
        int32_t rx_len = getSn_RX_RSR(SOCK_PMAP_TCP);
        if (rx_len > 0) {
            if (rx_len > (int32_t)sizeof(rx_buf)) rx_len = sizeof(rx_buf);
            recv(SOCK_PMAP_TCP, rx_buf, (uint16_t)rx_len);
            rpc_call_t call;
            xdr_t args;
            if (rpc_parse_call(rx_buf, (uint32_t)rx_len, &call, &args)) {
                handle_pmap_call(SOCK_PMAP_TCP, &call, &args, NULL, 0, false);
            }
        }
    } else if (status == SOCK_CLOSE_WAIT) {
        disconnect(SOCK_PMAP_TCP);
    } else if (status == SOCK_CLOSED) {
        socket(SOCK_PMAP_TCP, Sn_MR_TCP, PORT_PMAP, 0);
        listen(SOCK_PMAP_TCP);
    }

    /* ─── Portmapper UDP ─── */
    {
        int32_t rx_len = getSn_RX_RSR(SOCK_PMAP_UDP);
        if (rx_len > 0) {
            if (rx_len > (int32_t)sizeof(rx_buf)) rx_len = sizeof(rx_buf);
            uint8_t peer_ip[4];
            uint16_t peer_port;
            recvfrom(SOCK_PMAP_UDP, rx_buf, (uint16_t)rx_len, peer_ip, &peer_port);
            rpc_call_t call;
            xdr_t args;
            if (rpc_parse_call(rx_buf, (uint32_t)rx_len, &call, &args)) {
                handle_pmap_call(SOCK_PMAP_UDP, &call, &args, peer_ip, peer_port, true);
            }
        }
        if (getSn_SR(SOCK_PMAP_UDP) == SOCK_CLOSED) {
            socket(SOCK_PMAP_UDP, Sn_MR_UDP, PORT_PMAP, 0);
        }
    }

    /* ─── VXI-11 Core TCP ─── */
    status = getSn_SR(SOCK_VXI11_TCP);
    if (status == SOCK_ESTABLISHED) {
        int32_t rx_len = getSn_RX_RSR(SOCK_VXI11_TCP);
        if (rx_len > 0) {
            if (rx_len > (int32_t)sizeof(rx_buf)) rx_len = sizeof(rx_buf);
            recv(SOCK_VXI11_TCP, rx_buf, (uint16_t)rx_len);
            rpc_call_t call;
            xdr_t args;
            if (rpc_parse_call(rx_buf, (uint32_t)rx_len, &call, &args)) {
                handle_vxi11_call(SOCK_VXI11_TCP, &call, &args);
            }
        }
    } else if (status == SOCK_CLOSE_WAIT) {
        disconnect(SOCK_VXI11_TCP);
        vxi11.link_active = false;
    } else if (status == SOCK_CLOSED) {
        socket(SOCK_VXI11_TCP, Sn_MR_TCP, PORT_VXI11_CORE, 0);
        listen(SOCK_VXI11_TCP);
    }
}
