#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include "dpdk_transport.h"
#include "dpdk_recv.h"
#include "dpdk_common.h"

#define RECV_TABLE_ENTRIES 1024

struct msg_recv_record
{
    struct msg_buf *buf;
    uint64_t pkts_received_mask[4]; // mask of received pktids
    uint8_t nb_pkts_received;
};

static inline void set_headers(struct rte_mbuf *pkt, struct msg_buf *buf, uint32_t msgid, uint8_t type, uint32_t msg_len);
static inline void recv_msg(struct lcore_params *params, struct msg_buf *buf, struct rte_hash *hashtbl, struct msg_key *key);
static inline void recv_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct rte_hash *hashtbl);
static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr);

static inline void set_headers(struct rte_mbuf *pkt, struct msg_buf *buf, uint32_t msgid, uint8_t type, uint32_t msg_len){
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr *));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    // Initialize DPDK Transport header.
    dpdk_hdr->msgid = rte_cpu_to_be_32(msgid);
    dpdk_hdr->msg_len = rte_cpu_to_be_32(msg_len);
    dpdk_hdr->pktid = 0;
    dpdk_hdr->type = type;

    // Initialize IP header.
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_DPDK_TRANSPORT;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + ((uint16_t) msg_len));
    ip_hdr->src_addr = rte_cpu_to_be_32(buf->info->dst_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(buf->info->src_ip);

    set_ipv4_cksum(ip_hdr);

    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;
    mac_addr.as_int = rte_cpu_to_be_64(buf->info->src_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->d_addr);
    mac_addr.as_int = rte_cpu_to_be_64(buf->info->dst_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static inline void recv_msg(struct lcore_params *params, struct msg_buf *buf, struct rte_hash *hashtbl, struct msg_key *key)
{
    uint16_t total_hdr_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr);

    // send completed pkt
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(params->mbuf_pool);
    if (unlikely(pkt == NULL))
    {
        RTE_LOG_DP(DEBUG, HASH,
            "%s:Completed pkt loss due to filled rte_pktmbuf_alloc\n", __func__);
    }
    else{
        pkt->data_len = total_hdr_size;
        pkt->pkt_len = total_hdr_size;
        pkt->port = buf->info->portid;
        set_headers(pkt, buf, key->msgid, DPDK_TRANSPORT_COMPLETE, 0);
        if (rte_ring_enqueue(params->tx_ring, (void *)pkt) != 0){
            RTE_LOG_DP(DEBUG, RING,
                           "%s:Completed pkt loss due to full tx_ring\n", __func__);
            rte_pktmbuf_free(pkt);
        }
    }

    // pass full msg on to recv_ring
    rte_hash_del_key(hashtbl, key);
    if (rte_ring_enqueue(params->recv_ring, buf) != 0)
    {
        RTE_LOG_DP(DEBUG, HASH,
                   "%s:Msg loss due to failed recv_ring enqueue\n", __func__);
        rte_free(buf->info);
        rte_free(buf->msg);
        rte_free(buf);
    }
}

static inline void recv_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct rte_hash *hashtbl)
{
    uint16_t total_hdr_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr);
    uint16_t max_pkt_msgdata_len = RTE_ETHER_MAX_LEN - total_hdr_size;

    struct msg_key key;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                       sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    key.src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    key.dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    key.msgid = rte_be_to_cpu_32(dpdk_hdr->msgid);
    struct msg_recv_record *recv_record;
    if (rte_hash_lookup_data(hashtbl, &key, (void *)&recv_record) < 0)
    {
        // first packet of a new msg
        // TODO need to take care of repeated pkts

        struct msg_buf *buf = rte_malloc("recv msgbuf", sizeof(struct msg_buf), 0);
        buf->info = rte_malloc("recv msginfo", sizeof(struct msginfo), 0);
        buf->msg = rte_malloc("recv msgdata", rte_be_to_cpu_32(dpdk_hdr->msg_len), 0);

        union
        {
            uint64_t as_int;
            struct rte_ether_addr as_addr;
        } mac_addr;
        rte_ether_addr_copy(&eth_hdr->s_addr, &mac_addr.as_addr);
        buf->info->src_mac = rte_be_to_cpu_64(mac_addr.as_int);
        rte_ether_addr_copy(&eth_hdr->d_addr, &mac_addr.as_addr);
        buf->info->dst_mac = rte_be_to_cpu_64(mac_addr.as_int);
        buf->info->src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
        buf->info->dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
        buf->info->portid = pkt->port;
        buf->info->length = rte_be_to_cpu_32(dpdk_hdr->msg_len);

        uint8_t pktid = dpdk_hdr->pktid;
        // copy over packet msg data
        rte_memcpy((void *)&(buf->msg[pktid * max_pkt_msgdata_len]),
                   rte_pktmbuf_mtod_offset(pkt, void *, total_hdr_size),
                   pkt->pkt_len - total_hdr_size);

        if (rte_be_to_cpu_32(dpdk_hdr->msg_len) <= max_pkt_msgdata_len)
        {
            recv_msg(params, buf, hashtbl, &key);
        }
        else
        {
            // add msg record to hash table
            recv_record = rte_zmalloc("recv_record", sizeof(struct msg_recv_record), 0);
            recv_record->buf = buf;
            recv_record->nb_pkts_received = 1;
            recv_record->pkts_received_mask[pktid / 64] |= (1 << (pktid % 64));

            if (unlikely(rte_hash_add_key_data(hashtbl, (void *)&key, (void *)recv_record) < 0))
            {
                RTE_LOG_DP(DEBUG, HASH,
                           "%s:Pkt loss due to failed rte_hash_add_key_data\n", __func__);
                rte_free(buf->info);
                rte_free(buf->msg);
                rte_free(buf);
                rte_free(recv_record);
            }
        }
    }
    else
    {
        uint8_t pktid = dpdk_hdr->pktid;
        if (likely((recv_record->pkts_received_mask[pktid / 64] & (1l << (pktid % 64))) == 0))
        {
            // packet is not a duplicate
            recv_record->nb_pkts_received++;
            recv_record->pkts_received_mask[pktid / 64] |= (1LL << (pktid % 64));

            // copy over packet msg data
            rte_memcpy((void *)&(recv_record->buf->msg[pktid * max_pkt_msgdata_len]),
                       rte_pktmbuf_mtod_offset(pkt, void *, total_hdr_size),
                       pkt->pkt_len - total_hdr_size);

            uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->buf->info->length, max_pkt_msgdata_len) / max_pkt_msgdata_len;
            if (recv_record->nb_pkts_received == total_pkts)
            {
                recv_msg(params, recv_record->buf, hashtbl, &key);
                rte_free(recv_record);
            }
        }
    }
    
    rte_pktmbuf_free(pkt);
}

int lcore_recv(struct lcore_params *params)
{
    printf("\nCore %u doing recv task.\n", rte_lcore_id());

    struct rte_hash *hashtbl = NULL;
    struct rte_hash_parameters hash_params = {
        .name = "recv_table",
        .entries = RECV_TABLE_ENTRIES,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
    };

    hashtbl = rte_hash_create(&hash_params);
    if (hashtbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create recv hash table\n");
    }

    while (!params->quit_signal_recv)
    {
        // dequeue pkts from rx_recv_ring
        struct rte_mbuf *pkts[BURST_SIZE_RX];
        unsigned nb_rx = rte_ring_dequeue_burst(params->rx_recv_ring, (void *)pkts, BURST_SIZE_RX, NULL);
        if (likely(nb_rx > 0))
        {
            for (unsigned i = 0; i < nb_rx; i++)
                recv_pkt(params, pkts[i], hashtbl);
        }

        // send resend requests for missing packets
    }

    rte_hash_free(hashtbl);
    printf("\nCore %u exiting recv task.\n", rte_lcore_id());
    return 0;
}

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