
#include "tt.h"

#if TT_USE_STD_FUNC
#include <stdio.h>
#include <string.h>

#define tt_memcpy   rt_memcpy
#define tt_memmove  rt_memmove
#define tt_memset   rt_memset

#define tt_println(fmt, ...) //\
            printf("[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define tt_memcpy   _tt_memcpy
#define tt_memmove  _tt_memcpy
#define tt_memset   _tt_memset

#define tt_println(fmt, ...)
#endif

#define TT_FMASK    0b11111100
#define TT_FTAG     0b11001100

#define TT_ACK      0b01
#define TT_FIN      0b10

#define TT_SET_FLG(p, x)    p[0] = (x)
#define TT_SET_SEQ(p, x)    p[1] = (x) >> 8, p[2] = (x) & 0xff
#define TT_SET_ACK(p, x)    p[3] = (x) >> 8, p[4] = (x) & 0xff
#define TT_SET_LEN(p, x)    p[5] = (x) >> 8, p[6] = (x) & 0xff
#define TT_SET_CRC(p, l, x) p[7 + (l)] = (x) >> 8, p[8 + (l)] = (x) & 0xff
#define TT_SET_PLD(p, l, x) tt_memcpy(&p[7], x, l)

#define TT_GET_FLG(p)       (p[0])
#define TT_GET_SEQ(p)       (p[1] << 8 | p[2])
#define TT_GET_ACK(p)       (p[3] << 8 | p[4])
#define TT_GET_LEN(p)       (p[5] << 8 | p[6])
#define TT_GET_CRC(p, l)    (p[7 + (l)] << 8 | p[8 + (l)])
#define TT_GET_PLD(p)       (&p[7])


#if !TT_USE_STD_FUNC
static void _tt_memcpy(u8_t* dst, const u8_t* src, u32_t len)
{
    while (len-- > 0) {
        dst[len] = src[len];
    }
}
static void _tt_memset(u8_t* dst, u8_t val, u32_t len)
{
    while (len-- > 0) {
        dst[len] = val;
    }
}
#endif

static u16_t crc16(const u8_t* data, u32_t len)
{
    u16_t crc = 0;

#if 1
    while (len--) {
        crc = crc >> 1 | crc << 15;
        crc += *data++;
    }
#else
    u32_t i, j;
    u16_t t;

    for (i = 0; i < len; i++) {
        for (j = 0; j < 8; j++) {
            t = ((data[i] << j) & 0x80) ^ ((crc & 0x8000) >> 8);
            crc <<= 1;

            if(t != 0) {
                crc ^= 0x1021;
            }
        }
    }
#endif

    return crc;
}

s32_t tt_init(tt_t* tt, tt_cb rcb, tt_cb wcb, u8_t nsend, u8_t nrecv, void* user)
{
    tt->seq = 0;
    tt->ack = 0;
    tt->rcb = rcb;
    tt->wcb = wcb;
    tt->wnd = 0;
    tt->closed = 0;
    tt->nsend = nsend;
    tt->nrecv = nrecv;
    tt->usr = user;

    tt_memset((void*) tt->blen, 0, sizeof(tt->blen));
    return 0;
}

s32_t tt_send(tt_t* tt, const u8_t* buf, s32_t len)
{
    u8_t tmp[TT_SZPKT];
    u8_t* pkt;
    s32_t rt;
    s32_t sz;
    s32_t rmn = len;/* ???????????? */
    u32_t msk = 0;  /* mask????????????????????????????????????????????????ACK */
    u32_t i;
    u16_t pl;
    u16_t crc;
    u8_t nrecv;     /* ?????????????????? */
    u8_t nsend = 0; /* ?????????????????? */

    tt_println("tt_send len %d", len);

    if (tt->closed) {
        tt_println("connection is closed");
        return TT_ERRFINAL;
    }

    while (rmn > 0) {
        sz = rmn;
        rt = 0;

        /* ?????????????????? */
        for (i = 0; i < TT_SZWND && sz > 0; ++i, sz -= rt) {

            if ((1 << i) & msk) continue; /* ???????????????ACK????????????????????? */

            rt = sz > TT_SZPL ? TT_SZPL : sz;

            tt_println("send packet %d, pl %d", tt->seq + i, rt);

            TT_SET_FLG(tmp, TT_FTAG);
            TT_SET_SEQ(tmp, tt->seq + i);
            TT_SET_ACK(tmp, tt->ack);

            TT_SET_LEN(tmp, rt);
            TT_SET_PLD(tmp, rt, buf + (i * TT_SZPL));

            crc = crc16(tmp, rt + TT_SZHDR - 2);
            TT_SET_CRC(tmp, rt, crc);

            if (tt->wcb(tt->usr, tmp, TT_SZHDR + rt) < 0) {
                tt_println("writecb (data) failed, return");
                return TT_ERRSEND; // TODO
            }
        }
        /* ??????i????????????????????????????????? */

        nsend = nsend + 1;
        nrecv = 0;
        sz = 0;
        /* ??????ACK */
        while (1) {
            rt = tt->rcb(tt->usr, tmp + sz, TT_SZPKT - sz);
            if (rt < 0) {
                tt_println("readcb (ACK) failed");
                return TT_ERRRECV;
            }

            if (!rt) {
                if (++nrecv >= tt->nrecv) {
                    /* ??????????????????????????????tt->nrecv???????????????????????? */
                    tt_println("readcb (ACK) timeout count reach max, resend");
                    break;
                }
                tt_println("readcb (ACK) timeout");
                continue;
            }

            sz += rt;
            /* ??????????????????????????? */
            if (sz < TT_SZHDR) {
                tt_println("packet need more (header)");
                continue;
            }

            pkt = tmp;
            /* ?????????????????????????????? */
            do {
                /* ????????????????????????flag?????????????????? */
                if ((TT_GET_FLG(pkt) & TT_FMASK) != TT_FTAG) {
                    tt_println("got an error packet (flag)");
                    sz = 0; break;
                }

                pl = TT_GET_LEN(pkt);
                /* ???????????????????????????????????????????????? */
                if (pl > TT_SZPL) {
                    tt_println("got an error packet (payload)");
                    sz = 0; break;
                }

                /* ??????????????????????????? */
                if (sz < TT_SZHDR + pl) {
                    tt_println("packet need more (payload)");
                    break;
                }

                crc = crc16(pkt, pl + TT_SZHDR - 2);
                /* CRC????????????????????? */
                if (TT_GET_CRC(pkt, pl) != crc) {
                    tt_println("got an error packet (crc)");
                    sz = 0; break;
                }

                if ((TT_GET_FLG(pkt) & TT_ACK)) {
                    /* ?????????ACK??? */
                    rt = TT_GET_ACK(pkt);

                    if (rt >= tt->seq && rt < tt->seq + i) {
                        /* ???mask?????? rt - tt->seq ??????1 */
                        msk |= 1 << (rt - tt->seq);
                        tt_println("ACK %d recved", rt);
                    } else {
                        tt_println("ACK %d recved (out of range)", rt);
                    }

                } else if (TT_GET_FLG(pkt) & TT_FIN) {
                    /* ?????????FIN????????????TT_GET_ACK(pkt)??????????????????????????? */
                    rt = TT_GET_ACK(pkt);

                    if (rt > tt->seq && rt <= tt->seq + i) {
                        /* ???mask?????? rt - tt->seq + 1 ????????????1 */
                        msk |= (1 << (rt - tt->seq)) - 1;
                        tt_println("FIN %d recved", rt);
                    } else {
                        tt_println("FIN %d recved (out of range)", rt);
                    }

                    /* ???????????????FIN??? */
                    TT_SET_SEQ(pkt, tt->seq);
                    TT_SET_ACK(pkt, tt->ack);
                    TT_SET_LEN(pkt, 0);

                    crc = crc16(pkt, TT_SZHDR - 2);
                    TT_SET_CRC(pkt, 0, crc);

                    tt_println("send FIN");

                    if (tt->wcb(tt->usr, pkt, TT_SZHDR) < 0) {
                        tt_println("writecb (FIN) failed");
                        // TODO
                    }

                    tt->closed = 1;

                } else {
                    rt = TT_GET_SEQ(pkt);
#if 1
                    tt_println("data packet %d recved (unaccepted), pl %d", rt, pl);
#else
                    /* ????????????????????????????????????????????????????????????ACK??????????????? */

                    if (rt < tt->ack) {
                        tt_println("data packet %d recved (duplicate), pl %d, send ACK", rt, pl);

                        /* ??????ACK??? */
                        TT_SET_FLG(pkt, TT_FTAG | TT_ACK);
                        TT_SET_SEQ(pkt, tt->seq);
                        TT_SET_ACK(pkt, rt);
                        TT_SET_LEN(pkt, 0);

                        crc = crc16(pkt, TT_SZHDR - 2);
                        TT_SET_CRC(pkt, 0, crc);

                        /* ??????ACK */
                        if (tt->wcb(tt->usr, pkt, TT_SZHDR) < 0) {
                            tt_println("writecb (ACK) failed");
                            // TODO
                        }

                    } else {
                        tt_println("data packet %d recved (unaccepted), pl %d", rt, pl);
                    }
#endif
                }

                /* ?????????????????? */
                // nrecv = 0;
                nsend = 0;

                pkt += TT_SZHDR + pl;
                sz -= TT_SZHDR + pl;
            }
            while (sz > 0);

            if (msk == (1 << i) - 1) {
                /* ?????????????????????????????????ACK */
                tt_println("all ACK recved");
                break;
            }

            if (sz > 0 && pkt != tmp) {
                tt_println("recv buf left");
                tt_memmove(tmp, pkt, sz);
            }
        }

        if (nsend >= tt->nsend) {
            tt_println("resend count reach max, break");
            break;
        }

        for (i = 0; (1 << i) & msk; ++i) ;

        /* ??????i???????????????ACK????????? */
        if (i > 0) {
            tt_println("send window >> %d", i);

            rt = i * TT_SZPL;
            /* ??????????????????i????????? */
            rmn -= rt;
            buf += rt;
            msk >>= i;

            tt->seq += i;
        }

        if (tt->closed) {
            tt_println("connection closed by peer");
            break;
        }
    }

    return rmn > 0 ? len - rmn : len;
}

s32_t tt_recv(tt_t* tt, u8_t* buf, s32_t len)
{
    u8_t tmp[TT_SZPKT];
    u8_t* pkt;
    s32_t rt;
    s32_t sz = 0;
    s32_t rcv = 0;  /* ??????buf?????????????????? */
    u32_t i;
    u16_t iwnd;
    u16_t pl;
    u16_t crc;
    u8_t nrecv = 0; /* ?????????????????? */

    tt_println("tt_recv expect len %d", len);

    /* ?????????????????????????????????????????????????????? */
    for (i = 0; i < TT_SZWND; ++i) {
        iwnd = tt->wnd % TT_SZWND;

        if (!tt->blen[iwnd]) break;

        if (len >= tt->blen[iwnd]) {
            tt_memcpy(buf, tt->buf[iwnd], tt->blen[iwnd]);

            rcv += tt->blen[iwnd];
            buf += tt->blen[iwnd];
            len -= tt->blen[iwnd];

            tt_println("copy to user %d bytes", tt->blen[iwnd]);

            /* ???????????????????????? */
            ++tt->ack;
            ++tt->wnd;
            tt->blen[iwnd] = 0;

            /* ?????????????????????0??????????????? */
            if (!len) return rcv;

        } else {
            /* ????????????????????????????????????????????????????????? */
            tt_memcpy(buf, tt->buf[iwnd], len);

            rcv += len;
            buf += len;

            tt_println("copy to user %d bytes", len);

            tt->blen[iwnd] -= len;
            tt_memmove(tt->buf[iwnd], tt->buf[iwnd] + len, tt->blen[iwnd]);
            return rcv;
        }
    }

    if (tt->closed) {
        tt_println("connection is closed");
        return TT_ERRFINAL;
    }

    while (1) {
        rt = tt->rcb(tt->usr, tmp + sz, TT_SZPKT - sz);
        if (rt < 0) {
            tt_println("readcb (data) failed, return");
            return TT_ERRRECV;
        }

        if (!rt) {
            if (++nrecv >= tt->nrecv) {
                tt_println("readcb (data) timeout count reach max, break");
                break;
            }
            tt_println("readcb (data) timeout");
            continue;
        }

        sz += rt;
        /* ??????????????????????????? */
        if (sz < TT_SZHDR) {
            tt_println("packet need more (header)");
            continue;
        }

        pkt = tmp;
        /* ????????? */
        do {
            /* ????????????????????????flag?????????????????? */
            if ((TT_GET_FLG(pkt) & TT_FMASK) != TT_FTAG) {
                tt_println("got an error packet (flag)");
                sz = 0; break;
            }

            pl = TT_GET_LEN(pkt);
            /* ???????????????????????????????????????????????? */
            if (pl > TT_SZPL) {
                tt_println("got an error packet (payload)");
                sz = 0; break;
            }

            /* ??????????????????????????? */
            if (sz < TT_SZHDR + pl) {
                tt_println("packet need more (payload)");
                break;
            }

            crc = crc16(pkt, pl + TT_SZHDR - 2);
            /* CRC????????????????????? */
            if (TT_GET_CRC(pkt, pl) != crc) {
                tt_println("got an error packet (crc)");
                sz = 0; break;
            }

            if (TT_GET_FLG(pkt) & TT_ACK) {
                /* ?????????ACK???????????????????????? */
                tt_println("ACK recved, drop it");
            } else if (TT_GET_FLG(pkt) & TT_FIN) {
                /* ?????????FIN????????????FIN??? */
                tt_println("FIN recved");

                /* ??????FIN??? */
                TT_SET_SEQ(pkt, tt->seq);
                TT_SET_ACK(pkt, tt->ack);
                TT_SET_LEN(pkt, 0);

                crc = crc16(pkt, TT_SZHDR - 2);
                TT_SET_CRC(pkt, 0, crc);

                tt_println("send FIN");

                /* ??????FIN????????????tt->ack?????????????????????????????? */
                if (tt->wcb(tt->usr, pkt, TT_SZHDR) < 0) {
                    tt_println("writecb (FIN) failed");
                    // TODO
                }

                tt->closed = 1;
                break;

            } else {
                /* ??????????????????????????? */
                rt = TT_GET_SEQ(pkt);

                if (rt < tt->ack + TT_SZWND) {

                    if (rt >= tt->ack && pl > 0) {
                        i = (rt - tt->ack + tt->wnd) % TT_SZWND;

                        if (!tt->blen[i]) {
                            /* ???????????????????????????????????? */
                            tt_memcpy(tt->buf[i], TT_GET_PLD(pkt), pl);
                            tt->blen[i] = pl;
                            tt_println("data packet %d recved, pl %d", rt, pl);

                            /* ?????????????????????tt->buf?????????????????????????????????buf??? */
                            for (i = 0; i < TT_SZWND; ++i) {
                                iwnd = tt->wnd % TT_SZWND;

                                if (!tt->blen[iwnd]) break;

                                if (len >= tt->blen[iwnd]) {
                                    tt_memcpy(buf, tt->buf[iwnd], tt->blen[iwnd]);

                                    rcv += tt->blen[iwnd];
                                    buf += tt->blen[iwnd];
                                    len -= tt->blen[iwnd];

                                    tt_println("copy to user %d bytes", tt->blen[iwnd]);

                                    /* ???????????????????????? */
                                    ++tt->ack;
                                    ++tt->wnd;
                                    tt->blen[iwnd] = 0;

                                    /* ?????????????????????0??????????????? */
                                    if (!len) break;
                                } else {
                                    /* ??????????????????????????????????????????????????????????????? */
                                    tt_memcpy(buf, tt->buf[iwnd], len);

                                    rcv += len;
                                    buf += len;

                                    tt_println("copy to user %d bytes", len);

                                    tt->blen[iwnd] -= len;
                                    tt_memmove(tt->buf[iwnd], tt->buf[iwnd] + len, tt->blen[iwnd]);
                                    len = 0;
                                    break;
                                }
                            }

                        } else {
                            /* ?????????????????? */
                            tt_println("data packet %d recved (duplicate), pl %d", rt, pl);
                        }

                    } else {
                        /* ??????????????????????????????????????????????????? */
                        tt_println("data packet %d recved (duplicate and out of range), pl %d", rt, pl);
                    }

                    tt_println("send ACK %d", rt);

                    /* ??????ACK??? */
                    TT_SET_FLG(pkt, TT_FTAG | TT_ACK);
                    TT_SET_SEQ(pkt, tt->seq);
                    TT_SET_ACK(pkt, rt);
                    TT_SET_LEN(pkt, 0);

                    crc = crc16(pkt, TT_SZHDR - 2);
                    TT_SET_CRC(pkt, 0, crc);

                    /* ??????ACK */
                    if (tt->wcb(tt->usr, pkt, TT_SZHDR) < 0) {
                        tt_println("writecb (ACK) failed");
                        // TODO
                    }

                    /* ??????????????????????????????????????? */
                    if (!len) break;

                } else {
                    tt_println("data packet %d recved (out of range), pl %d", rt, pl);
                }

                /* ?????????????????? */
                nrecv = 0;
            }

            pkt += TT_SZHDR + pl;
            sz -= TT_SZHDR + pl;
        }
        while (sz > 0);

        if (tt->closed) {
            tt_println("connection closed by peer");
            break;
        }

        if (!len) break;

        if (sz > 0 && pkt != tmp) {
            tt_println("recv buf left");
            tt_memmove(tmp, pkt, sz);
        }
    }

    tt_println("tt_recv actual len %d", rcv);
    return rcv;
}

s32_t tt_close(tt_t* tt)
{
    u8_t tmp[TT_SZPKT];
    u8_t* pkt;
    s32_t rt;
    s32_t sz;
    u16_t pl;
    u16_t crc;
    u8_t nrecv;
    u8_t nsend = 0;

    if (tt->closed) {
        tt_println("connection is already closed");
        return 0;
    }

    while (nsend++ < tt->nsend) {
        /* ??????FIN??? */
        TT_SET_FLG(tmp, TT_FTAG | TT_FIN);
        TT_SET_SEQ(tmp, tt->seq);
        TT_SET_ACK(tmp, tt->ack);
        TT_SET_LEN(tmp, 0);

        crc = crc16(tmp, TT_SZHDR - 2);
        TT_SET_CRC(tmp, 0, crc);

        tt_println("send FIN");

        /* ??????FIN????????????tt->ack?????????????????????????????? */
        if (tt->wcb(tt->usr, tmp, TT_SZHDR) < 0) {
            tt_println("writecb (FIN) failed");
            return TT_ERRSEND;
        }

        nrecv = 0;
        sz = 0;
        /* ?????????????????????FIN??? */
        while (1) {
            rt = tt->rcb(tt->usr, tmp + sz, TT_SZPKT - sz);
            if (rt < 0) {
                tt_println("readcb (FIN) failed, return");
                return TT_ERRRECV;
            }

            if (!rt) {
                if (++nrecv >= tt->nrecv) {
                    tt_println("readcb (FIN) timeout count reach max, break");
                    break;
                }
                tt_println("readcb (FIN) timeout");
                continue;
            }

            sz += rt;
            /* ??????????????????????????? */
            if (sz < TT_SZHDR) {
                tt_println("packet need more (header)");
                continue;
            }

            pkt = tmp;
            /* ????????? */
            do {
                /* ????????????????????????flag?????????????????? */
                if ((TT_GET_FLG(pkt) & TT_FMASK) != TT_FTAG) {
                    tt_println("got an error packet (flag)");
                    sz = 0; break;
                }

                pl = TT_GET_LEN(pkt);
                /* ???????????????????????????????????????????????? */
                if (pl > TT_SZPL) {
                    tt_println("got an error packet (payload)");
                    sz = 0; break;
                }

                /* ??????????????????????????? */
                if (sz < TT_SZHDR + pl) {
                    tt_println("packet need more (payload)");
                    break;
                }

                crc = crc16(pkt, pl + TT_SZHDR - 2);
                /* CRC????????????????????? */
                if (TT_GET_CRC(pkt, pl) != crc) {
                    tt_println("got an error packet (crc)");
                    sz = 0; break;
                }

                if (TT_GET_FLG(pkt) & TT_FIN) {
                    /* ?????????FIN??? */
                    tt_println("FIN recved, return");
                    tt->closed = 1;
                    return 0;
                }

                pkt += TT_SZHDR + pl;
                sz -= TT_SZHDR + pl;
            }
            while (sz > 0);

            if (sz > 0 && pkt != tmp) {
                tt_println("recv buf left");
                tt_memmove(tmp, pkt, sz);
            }
        }
    }

    return 0;
}

s32_t tt_wait(tt_t* tt)
{
    u8_t tmp[TT_SZPKT];
    u8_t* pkt;
    s32_t rt;
    s32_t sz = 0;
    u16_t pl;
    u16_t crc;
    u32_t mrecv =  2 * tt->nsend * tt->nrecv;

    tt_println("tt_wait...");

    while (mrecv-- > 0) {
        rt = tt->rcb(tt->usr, tmp + sz, TT_SZPKT - sz);
        if (rt < 0) {
            tt_println("readcb (FIN) failed, return");
            return TT_ERRRECV;
        }

        if (!rt) continue;

        sz += rt;
        /* ??????????????????????????? */
        if (sz < TT_SZHDR) {
            tt_println("packet need more (header)");
            continue;
        }

        pkt = tmp;
        /* ????????? */
        do {
            /* ????????????????????????flag?????????????????? */
            if ((TT_GET_FLG(pkt) & TT_FMASK) != TT_FTAG) {
                tt_println("got an error packet (flag)");
                sz = 0; break;
            }

            pl = TT_GET_LEN(pkt);
            /* ???????????????????????????????????????????????? */
            if (pl > TT_SZPL) {
                tt_println("got an error packet (payload)");
                sz = 0; break;
            }

            /* ??????????????????????????? */
            if (sz < TT_SZHDR + pl) {
                tt_println("packet need more (payload)");
                break;
            }

            crc = crc16(pkt, pl + TT_SZHDR - 2);
            /* CRC????????????????????? */
            if (TT_GET_CRC(pkt, pl) != crc) {
                tt_println("got an error packet (crc)");
                sz = 0; break;
            }

            /* ??????????????????????????????FIN??????ACK????????????FIN */
            TT_SET_FLG(tmp, TT_FTAG | TT_FIN);
            TT_SET_SEQ(tmp, tt->seq);
            TT_SET_ACK(tmp, tt->ack);
            TT_SET_LEN(tmp, 0);

            crc = crc16(tmp, TT_SZHDR - 2);
            TT_SET_CRC(tmp, 0, crc);

            tt_println("send FIN");

            /* ??????FIN????????????tt->ack?????????????????????????????? */
            if (tt->wcb(tt->usr, tmp, TT_SZHDR) < 0) {
                tt_println("writecb (FIN) failed");
                return TT_ERRSEND;
            }

            pkt += TT_SZHDR + pl;
            sz -= TT_SZHDR + pl;
        }
        while (sz > 0);

        if (sz > 0 && pkt != tmp) {
            tt_println("recv buf left");
            tt_memmove(tmp, pkt, sz);
        }
    }

    tt_println("tt_wait done");
    return 0;
}
