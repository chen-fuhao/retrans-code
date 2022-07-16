#ifndef _TT_H_
#define _TT_H_

#ifndef TT_USE_STD_FUNC
#define TT_USE_STD_FUNC 1
#endif

/* header
------------------------------------------
| flag | seq | ack | len | payload | crc |
|  1B  |  2B |  2B |  2B |  <len>  |  2B |
------------------------------------------
flag
----------------------------------
| version | reserved | FIN | ACK |
|   4b    |    2b    | 1b  | 1b  |
----------------------------------
*/

#define TT_SZWND        8       /* 窗口大小，最大32 */
#define TT_SZPKT        185     /* MTU，最大32767（0x7fff） */
#define TT_SZHDR        9       /* 包头长度（包含2字节的CRC） */
#define TT_SZPL         (TT_SZPKT - TT_SZHDR)   /* 单包最大负载长度 */

#define TT_ERRRECV      -1
#define TT_ERRSEND      -2
#define TT_ERRFINAL     -3

typedef unsigned char   u8_t;
typedef char            s8_t;
typedef unsigned short  u16_t;
typedef short           s16_t;
typedef unsigned int    u32_t;
typedef int             s32_t;

/* 回调该函数时len最大值为TT_SZPKT */
typedef s16_t (*tt_cb)(void* usr, u8_t* buf, s16_t len);

typedef struct {
    u16_t   seq;
    u16_t   ack;
    tt_cb   rcb;
    tt_cb   wcb;

    u8_t    buf[TT_SZWND][TT_SZPL]; /* 接收缓存 */
    u16_t   blen[TT_SZWND];         /* buf数组对应数据长度 */
    u8_t    wnd;                    /* 窗口位置偏移 */
    u8_t    closed;                 /* 是否已接收/发送完毕 */

    u16_t   mackr;   /* 接收ACK的最大次数，超过此值后会进入重发流程 */
    void*   usr;
} tt_t;

/* 初始化tt_t结构体，mackr（接收ACK的最大次数）
 */
void tt_init(tt_t* tt, tt_cb rcb, tt_cb wcb, u16_t mackr, void* usr);

/* 重置内部状态（序列号、关闭状态等）
 */
void tt_reset(tt_t* tt);

/* 返回成功发送的字节数（可能小于len，也可能等于0，小于0表示出错）。
 * 连续发送数据包次数达到msend且无有效ACK时该函数会返回（返回当前已成功发送的字节数）。
 * 发送数据包后，连续接收ACK次数达到tt->mackr且无有效ACK时会重发。
 */
s32_t tt_send(tt_t* tt, const u8_t* buf, s32_t len, s32_t msend);

/* 返回成功接收的字节数（可能小于len，也可能等于0，小于0表示出错）
 * 连续接收数据包mrecv次无有效数据包时该函数会返回（返回当前已成功收到的字节数）。
 */
s32_t tt_recv(tt_t* tt, u8_t* buf, s32_t len, s32_t mrecv);

/* 发送FIN包。发送方（tt_send）可调用该接口，以告知对方已无后续数据。
 * 接收方（tt_recv）也可调用该接口，以告知对方不会再接收发过来的数据。
 * 当发送FIN包次数达到msend且未收到回复时该函数返回0，有收到回复也会返回0但tt_is_closed()会返回1.
 */
s32_t tt_close(tt_t* tt, s32_t msend);

/* 等待2*tt->nsend*tt->nrecv个接收超时周期，在此期间收到对方的FIN都为其响应FIN，当收到数据包时该函数会提前返回。
 * 被close的一方响应的FIN可能会丢失，所以被动方需要调用该接口来尽量避免这种情况。
 */
s32_t tt_wait(tt_t* tt, s32_t mrecv);

/* 是否已接收/发送完毕
 */
#define tt_is_closed(ptt)    ((ptt)->closed)

#endif // _TT_H_
