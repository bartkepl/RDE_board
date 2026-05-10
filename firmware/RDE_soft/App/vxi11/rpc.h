/*
 * rpc.h – minimal ONC-RPC / XDR helpers for VXI-11 server
 *
 * Only the subset needed for VXI-11 Core and portmapper is implemented.
 * Big-endian XDR encoding, TCP Record Mark framing.
 */

#ifndef APP_VXI11_RPC_H_
#define APP_VXI11_RPC_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ===== XDR buffer ===== */
typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t len;
} xdr_t;

void     xdr_init_read (xdr_t *x, uint8_t *buf, uint32_t len);
void     xdr_init_write(xdr_t *x, uint8_t *buf, uint32_t len);
bool     xdr_read_u32 (xdr_t *x, uint32_t *out);
bool     xdr_read_opaque(xdr_t *x, uint8_t *out, uint32_t max, uint32_t *got);
bool     xdr_read_string(xdr_t *x, char *out, uint32_t max);
void     xdr_write_u32(xdr_t *x, uint32_t val);
void     xdr_write_opaque(xdr_t *x, const uint8_t *data, uint32_t len);
void     xdr_write_string(xdr_t *x, const char *str);
uint32_t xdr_written(xdr_t *x);

/* ===== RPC message structures ===== */
typedef struct {
    uint32_t xid;
    uint32_t prog;
    uint32_t vers;
    uint32_t proc;
} rpc_call_t;

typedef enum {
    RPC_ACCEPT_SUCCESS   = 0,
    RPC_ACCEPT_PROG_UNAVAIL = 1,
    RPC_ACCEPT_PROG_MISMATCH = 2,
    RPC_ACCEPT_PROC_UNAVAIL = 3,
    RPC_ACCEPT_GARBAGE_ARGS = 4,
} rpc_accept_stat_t;

/* Parse incoming RPC CALL from buf[0..len-1].
 * tcp_framing=true  → skip the 4-byte TCP Record Mark first (TCP sockets)
 * tcp_framing=false → parse from byte 0, no Record Mark (UDP sockets)
 * Returns false if the message is not a valid CALL. */
bool rpc_parse_call(const uint8_t *buf, uint32_t len, rpc_call_t *call, xdr_t *args,
                    bool tcp_framing);

/* Write RPC REPLY with SUCCESS into out_buf.
 * tcp_framing=true  → prepend 4-byte TCP Record Mark (TCP sockets)
 * tcp_framing=false → no Record Mark, write XID directly (UDP sockets)
 * Returns total bytes written. */
uint32_t rpc_build_reply(uint8_t *out_buf, uint32_t out_size,
                         uint32_t xid, const uint8_t *payload, uint32_t payload_len,
                         bool tcp_framing);

/* Write RPC REPLY with error status */
uint32_t rpc_build_error(uint8_t *out_buf, uint32_t out_size,
                         uint32_t xid, rpc_accept_stat_t stat);

#endif /* APP_VXI11_RPC_H_ */
