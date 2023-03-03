#include <inttypes.h>
#include <rte_byteorder.h>

#define BURST_SIZE_RX 64
#define BURST_SIZE_TX 32
#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

#define IPPROTO_DPDK_TRANSPORT 200
#define DPDK_TRANSPORT_MSGDATA 0
#define DPDK_TRANSPORT_COMPLETE 1
#define DPDK_TRANSPORT_RESEND 2

struct lcore_params {
    struct rte_ring *recv_ring;
    struct rte_ring *send_ring;
    struct rte_ring *tx_ring;
    struct rte_ring *rx_recv_ring;
    struct rte_ring *rx_send_ring;
    struct rte_mempool *mbuf_pool;
    int quit_signal_tx;
    int quit_signal_rx;
    int quit_signal_send;
    int quit_signal_recv;
};

struct dpdk_transport_hdr
{
    rte_be32_t msgid;
    rte_be32_t msg_len;
    uint8_t pktid;
    uint8_t type;
} __attribute__((__packed__));

struct msg_buf
{
    struct msginfo *info;
    char *msg;
};

struct msg_key
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t msgid;
};