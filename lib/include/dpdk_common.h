#ifndef DPDK_COMMON_H
#define DPDK_COMMON_H

#include <inttypes.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_atomic.h>
#include "dpdk_transport.h"

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

#define MAX_ACTIVE_SENDS ((2 * 1024) - 1)
#define MAX_ACTIVE_RECVS ((2 * 1024) - 1)
#define MAX_COMPLETED_RECVS ((2 * 1024) - 1)

struct lcore_params
{
    struct rte_ring *recv_ring;
    struct rte_ring *send_ring;
    struct rte_ring *send_tx_ring;
    struct rte_ring *recv_tx_ring;
    struct rte_ring *rx_recv_ring;
    struct rte_ring *rx_send_ring;
    struct rte_mempool *send_mbuf_pool;
    struct rte_mempool *recv_mbuf_pool;
    struct rte_mempool *send_record_pool;
    struct rte_mempool *recv_record_pool;
    rte_atomic16_t outstanding_sends;
    rte_atomic32_t next_msgid;
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

#define TOTAL_HDR_SIZE (sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr))
#define MAX_PKT_MSGDATA_LEN (RTE_ETHER_MAX_LEN - TOTAL_HDR_SIZE)
#define MAX_PKTS_IN_MSG (RTE_ALIGN_MUL_CEIL(MAX_MSG_SIZE, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN)

struct msg_key
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t msgid;
};

struct msg_send_record
{
    struct rte_mbuf *pkts[MAX_PKTS_IN_MSG];
    uint64_t time;
    struct msg_info info;
    uint32_t msgid;
};

struct msg_recv_record
{
    struct rte_mbuf *pkts[MAX_PKTS_IN_MSG];
    uint64_t time;
    struct msg_info info;
    uint8_t pkts_received_mask[RTE_ALIGN_MUL_CEIL(MAX_PKTS_IN_MSG, 8) / 8]; // mask of received pktids
    uint8_t nb_pkts_received;
    uint8_t nb_resend_requests;
};

void DumpHex(const void *data, size_t size);

static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr)
{
    uint16_t *ptr16 = (unaligned_uint16_t *)hdr;
    uint32_t ip_cksum = 0;
    ip_cksum += ptr16[0];
    ip_cksum += ptr16[1];
    ip_cksum += ptr16[2];
    ip_cksum += ptr16[3];
    ip_cksum += ptr16[4];
    ip_cksum += ptr16[6];
    ip_cksum += ptr16[7];
    ip_cksum += ptr16[8];
    ip_cksum += ptr16[9];

    // Reduce 32 bit checksum to 16 bits and complement it.
    ip_cksum = ((ip_cksum & 0xFFFF0000) >> 16) +
               (ip_cksum & 0x0000FFFF);
    if (ip_cksum > 65535)
        ip_cksum -= 65535;
    ip_cksum = (~ip_cksum) & 0x0000FFFF;
    if (ip_cksum == 0)
        ip_cksum = 0xFFFF;
    hdr->hdr_checksum = (uint16_t)ip_cksum;
}

#endif /* DPDK_COMMON_H */