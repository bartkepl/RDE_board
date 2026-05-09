/*
 * rpc.c – minimal ONC-RPC / XDR implementation for VXI-11 server
 */

#include "rpc.h"
#include <string.h>

/* ===== XDR helpers ===== */

void xdr_init_read(xdr_t *x, uint8_t *buf, uint32_t len)
{
    x->buf = buf;
    x->pos = 0;
    x->len = len;
}

void xdr_init_write(xdr_t *x, uint8_t *buf, uint32_t len)
{
    x->buf = buf;
    x->pos = 0;
    x->len = len;
}

bool xdr_read_u32(xdr_t *x, uint32_t *out)
{
    if (x->pos + 4 > x->len) return false;
    uint8_t *p = x->buf + x->pos;
    *out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    x->pos += 4;
    return true;
}

bool xdr_read_opaque(xdr_t *x, uint8_t *out, uint32_t max, uint32_t *got)
{
    uint32_t len;
    if (!xdr_read_u32(x, &len)) return false;
    uint32_t padded = (len + 3) & ~3u;
    if (x->pos + padded > x->len) return false;
    if (len > max) return false;
    memcpy(out, x->buf + x->pos, len);
    x->pos += padded;
    if (got) *got = len;
    return true;
}

bool xdr_read_string(xdr_t *x, char *out, uint32_t max)
{
    uint32_t got;
    /* max-1 to leave room for NUL */
    if (!xdr_read_opaque(x, (uint8_t *)out, max - 1, &got)) return false;
    out[got] = '\0';
    return true;
}

void xdr_write_u32(xdr_t *x, uint32_t val)
{
    if (x->pos + 4 > x->len) return;
    x->buf[x->pos++] = (val >> 24) & 0xFF;
    x->buf[x->pos++] = (val >> 16) & 0xFF;
    x->buf[x->pos++] = (val >>  8) & 0xFF;
    x->buf[x->pos++] =  val        & 0xFF;
}

void xdr_write_opaque(xdr_t *x, const uint8_t *data, uint32_t len)
{
    xdr_write_u32(x, len);
    uint32_t padded = (len + 3) & ~3u;
    if (x->pos + padded > x->len) return;
    if (len > 0 && data != NULL) memcpy(x->buf + x->pos, data, len);
    if (padded > len) memset(x->buf + x->pos + len, 0, padded - len);
    x->pos += padded;
}

void xdr_write_string(xdr_t *x, const char *str)
{
    xdr_write_opaque(x, (const uint8_t *)str, (uint32_t)strlen(str));
}

uint32_t xdr_written(xdr_t *x) { return x->pos; }

/* ===== RPC framing ===== */

/* RPC message type */
#define RPC_CALL  0u
#define RPC_REPLY 1u
/* Reply stat */
#define RPC_MSG_ACCEPTED 0u
/* Auth flavor – AUTH_NULL */
#define AUTH_NULL 0u

bool rpc_parse_call(const uint8_t *buf, uint32_t len, rpc_call_t *call, xdr_t *args)
{
    if (len < 4) return false;

    /* Skip TCP record mark (4 bytes) */
    uint32_t mark = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                    ((uint32_t)buf[2] << 8)  |  (uint32_t)buf[3];
    uint32_t frag_len = mark & 0x7FFFFFFFu;
    bool last_frag    = (mark >> 31) != 0;
    (void)last_frag;
    if (frag_len + 4 > len) return false;

    xdr_t x;
    xdr_init_read(&x, (uint8_t *)buf + 4, frag_len);

    uint32_t xid, msg_type, rpc_vers, cred_flavor, cred_len, verf_flavor, verf_len;

    if (!xdr_read_u32(&x, &xid))       return false;
    if (!xdr_read_u32(&x, &msg_type))  return false;
    if (msg_type != RPC_CALL)           return false;
    if (!xdr_read_u32(&x, &rpc_vers))  return false;  /* should be 2 */
    if (!xdr_read_u32(&x, &call->prog)) return false;
    if (!xdr_read_u32(&x, &call->vers)) return false;
    if (!xdr_read_u32(&x, &call->proc)) return false;
    /* credentials */
    if (!xdr_read_u32(&x, &cred_flavor)) return false;
    if (!xdr_read_u32(&x, &cred_len))    return false;
    if (cred_len > 0) x.pos += ((cred_len + 3) & ~3u);
    /* verifier */
    if (!xdr_read_u32(&x, &verf_flavor)) return false;
    if (!xdr_read_u32(&x, &verf_len))    return false;
    if (verf_len > 0) x.pos += ((verf_len + 3) & ~3u);

    call->xid = xid;

    /* args = rest of the buffer */
    xdr_init_read(args, (uint8_t *)buf + 4 + x.pos, frag_len - x.pos);
    return true;
}

uint32_t rpc_build_reply(uint8_t *out_buf, uint32_t out_size,
                         uint32_t xid, const uint8_t *payload, uint32_t payload_len)
{
    /* Header: xid(4) + REPLY(4) + ACCEPTED(4) + verf(8) + SUCCESS(4) = 24 bytes */
    uint32_t total = 24 + payload_len;
    if (total + 4 > out_size) return 0;

    xdr_t x;
    xdr_init_write(&x, out_buf, out_size);

    /* Record Mark: last fragment */
    xdr_write_u32(&x, 0x80000000u | total);

    xdr_write_u32(&x, xid);
    xdr_write_u32(&x, RPC_REPLY);
    xdr_write_u32(&x, RPC_MSG_ACCEPTED);
    /* verifier: AUTH_NULL, 0 bytes */
    xdr_write_u32(&x, AUTH_NULL);
    xdr_write_u32(&x, 0);
    /* accept stat: SUCCESS */
    xdr_write_u32(&x, RPC_ACCEPT_SUCCESS);

    /* payload */
    if (x.pos + payload_len <= out_size) {
        memcpy(out_buf + x.pos, payload, payload_len);
        x.pos += payload_len;
    }

    return x.pos;
}

uint32_t rpc_build_error(uint8_t *out_buf, uint32_t out_size,
                         uint32_t xid, rpc_accept_stat_t stat)
{
    if (4 + 24 > out_size) return 0;
    xdr_t x;
    xdr_init_write(&x, out_buf, out_size);

    xdr_write_u32(&x, 0x80000000u | 20u);  /* record mark: 20 bytes payload */
    xdr_write_u32(&x, xid);
    xdr_write_u32(&x, RPC_REPLY);
    xdr_write_u32(&x, RPC_MSG_ACCEPTED);
    xdr_write_u32(&x, AUTH_NULL);
    xdr_write_u32(&x, 0);
    xdr_write_u32(&x, (uint32_t)stat);

    return x.pos;
}
