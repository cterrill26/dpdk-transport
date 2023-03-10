#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_cycles.h>
#include "dpdk_transport.h"
#include "dpdk_recv.h"
#include "dpdk_common.h"
#include "linked_hash.h"

#define RESEND_TIME_US 1000

static inline void set_headers(struct rte_mbuf *pkt, struct msg_buf *buf, uint32_t msgid, uint8_t type, uint32_t data_len);
static inline void recv_msg(struct lcore_params *params, struct msg_recv_record *recv_record, struct linked_hash *hashtbl, struct msg_key *key, int32_t pos);
static inline void recv_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct linked_hash *hashtbl);
static inline void request_resends(struct lcore_params *params, struct linked_hash *hashtbl, uint64_t resend_before);
static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr);

static inline void set_headers(struct rte_mbuf *pkt, struct msg_buf *buf, uint32_t msgid, uint8_t type, uint32_t data_len)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *, sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    // Initialize DPDK Transport header.
    dpdk_hdr->msgid = rte_cpu_to_be_32(msgid);
    dpdk_hdr->msg_len = rte_cpu_to_be_32(data_len);
    dpdk_hdr->pktid = 0;
    dpdk_hdr->type = type;

    // Initialize IP header.
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_DPDK_TRANSPORT;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + ((uint16_t)data_len));
    // flipped since we are sending a pkt from receiver to sender
    ip_hdr->src_addr = rte_cpu_to_be_32(buf->info.dst_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(buf->info.src_ip);

    set_ipv4_cksum(ip_hdr);

    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;
    // flipped since we are sending a pkt from receiver to sender
    mac_addr.as_int = rte_cpu_to_be_64(buf->info.src_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->d_addr);
    mac_addr.as_int = rte_cpu_to_be_64(buf->info.dst_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static inline void recv_msg(struct lcore_params *params, struct msg_recv_record *recv_record, struct linked_hash *hashtbl, struct msg_key *key, int32_t pos)
{
    // send completed pkt
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(params->mbuf_pool);
    if (unlikely(pkt == NULL))
    {
        RTE_LOG_DP(INFO, MBUF,
                   "%s:Completed pkt loss due to filled rte_pktmbuf_alloc\n", __func__);
    }
    else
    {
        pkt->data_len = TOTAL_HDR_SIZE;
        pkt->pkt_len = TOTAL_HDR_SIZE;
        pkt->port = recv_record->buf.info.portid;
        set_headers(pkt, &recv_record->buf, key->msgid, DPDK_TRANSPORT_COMPLETE, 0);
        if (rte_ring_enqueue(params->tx_ring, (void *)pkt) != 0)
        {
            RTE_LOG_DP(INFO, RING,
                       "%s:Completed pkt loss due to full tx_ring\n", __func__);
            rte_pktmbuf_free(pkt);
        }
    }

    // pass full msg on to recv_ring
    if (likely(rte_ring_enqueue(params->recv_ring, recv_record) == 0))
        linked_hash_del_key(hashtbl, key);
    else{
        // in the case the enqueue fails, the entry will remain in the hashtbl, and the resend_request function
        // will attempt to enqueue the recv_record later on
        linked_hash_move_pos_to_front(hashtbl, pos);
    }
    
}

static inline void recv_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct linked_hash *hashtbl)
{
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
    int32_t pos = linked_hash_lookup_data(hashtbl, &key, (void *)&recv_record);
    if (pos < 0)
    {
        // first packet of a new msg
        // TODO need to take care of repeated pkts from completed msg

        recv_record = rte_zmalloc("recv record", sizeof(struct msg_recv_record), 0);
        recv_record->buf.msg = rte_malloc("recv msgdata", rte_be_to_cpu_32(dpdk_hdr->msg_len), 0);

        union
        {
            uint64_t as_int;
            struct rte_ether_addr as_addr;
        } mac_addr;
        mac_addr.as_int = 0LL;
        rte_ether_addr_copy(&eth_hdr->s_addr, &mac_addr.as_addr);
        recv_record->buf.info.src_mac = rte_be_to_cpu_64(mac_addr.as_int);
        mac_addr.as_int = 0LL;
        rte_ether_addr_copy(&eth_hdr->d_addr, &mac_addr.as_addr);
        recv_record->buf.info.dst_mac = rte_be_to_cpu_64(mac_addr.as_int);
        recv_record->buf.info.src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
        recv_record->buf.info.dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
        recv_record->buf.info.portid = pkt->port;
        recv_record->buf.info.length = rte_be_to_cpu_32(dpdk_hdr->msg_len);

        pos = linked_hash_add_key_data(hashtbl, (void *)&key, (void *)recv_record);
        if (unlikely(pos < 0))
        {
            RTE_LOG_DP(INFO, HASH,
                        "%s:Recv pkt loss due to failed linked_hash_add_key_data\n", __func__);
            rte_free(recv_record->buf.msg);
            rte_free(recv_record);
            rte_pktmbuf_free(pkt);
            return;
        }
    }

    uint8_t pktid = dpdk_hdr->pktid;
    if (likely((recv_record->pkts_received_mask[pktid / 64] & (1LL << (pktid % 64))) == 0))
    {
        recv_record->nb_pkts_received++;
        recv_record->pkts_received_mask[pktid / 64] |= (1LL << (pktid % 64));
        recv_record->time = rte_get_timer_cycles();

        // copy over packet msg data
        rte_memcpy((void *)&(recv_record->buf.msg[pktid * MAX_PKT_MSGDATA_LEN]),
                    rte_pktmbuf_mtod_offset(pkt, void *, TOTAL_HDR_SIZE),
                    pkt->pkt_len - TOTAL_HDR_SIZE);

        uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->buf.info.length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
        if (recv_record->nb_pkts_received == total_pkts)
            recv_msg(params, recv_record, hashtbl, &key, pos);
        else
            linked_hash_move_pos_to_back(hashtbl, pos);
    }

    rte_pktmbuf_free(pkt);
}

static inline void request_resends(struct lcore_params *params, struct linked_hash *hashtbl, uint64_t resend_before)
{
    struct msg_key *key;
    struct msg_recv_record *recv_record;
    int32_t next = 0;

    uint8_t nb_to_send = 0;
    struct rte_mbuf *pkts[BURST_SIZE_TX];
    while (1)
    {
        int32_t pos = linked_hash_iterate(hashtbl, (void *)&key, (void *)&recv_record, &next);
        if (pos < 0)
            break;

        uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->buf.info.length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
        uint8_t nb_pkts_missing = total_pkts - recv_record->nb_pkts_received;

        // completed msgs will be at the front of the linked hash
        if (unlikely(nb_pkts_missing == 0)){
            // recv_record must have failed to be enqueud in recv_ring earlier, try again
            if (likely(rte_ring_enqueue(params->recv_ring, recv_record) == 0))
                linked_hash_del_key(hashtbl, key);
            else
                continue;
        }

        // passed all the completed msgs in the linked hash
        if (recv_record->time >= resend_before)
            break;

        if (nb_to_send == 0)
        {
            if (unlikely(rte_pktmbuf_alloc_bulk(params->mbuf_pool, pkts, BURST_SIZE_TX) < 0))
            {
                RTE_LOG_DP(INFO, MBUF,
                           "%s:Resend request pkt loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
                continue;
            }
        }

        pkts[nb_to_send]->data_len = TOTAL_HDR_SIZE + nb_pkts_missing;
        pkts[nb_to_send]->pkt_len = TOTAL_HDR_SIZE + nb_pkts_missing;
        pkts[nb_to_send]->port = recv_record->buf.info.portid;
        set_headers(pkts[nb_to_send], &recv_record->buf, key->msgid, DPDK_TRANSPORT_RESEND, nb_pkts_missing);

        //fill pkt data contents with missing pktids
        uint8_t *resend_list = rte_pktmbuf_mtod_offset(pkts[nb_to_send], uint8_t *, TOTAL_HDR_SIZE);
        uint8_t resend_idx = 0;
        uint64_t bitmask = 1;
        uint64_t recv_mask = recv_record->pkts_received_mask[0];

        for (uint8_t pktid = 0; pktid < RTE_MIN(total_pkts, 64); pktid++)
        {
            if ((recv_mask & bitmask) == 0)
                resend_list[resend_idx++] = pktid;

            bitmask <<= 1;
        }

        bitmask = 1;
        recv_mask = recv_record->pkts_received_mask[1];
        for (uint8_t pktid = 64; pktid < total_pkts; pktid++)
        {
            if ((recv_mask & bitmask) == 0)
                resend_list[resend_idx++] = pktid;

            bitmask <<= 1;
        }

        recv_record->time = rte_get_timer_cycles();
        linked_hash_move_pos_to_back(hashtbl, pos);
        nb_to_send++;

        if (nb_to_send == BURST_SIZE_TX)
        {
            uint16_t sent;
            sent = rte_ring_enqueue_burst(params->tx_ring,
                                          (void *)pkts, nb_to_send, NULL);
            if (unlikely(sent < nb_to_send))
            {
                RTE_LOG_DP(INFO, RING,
                           "%s:Resend request pkt loss due to full tx_ring\n", __func__);
                while (sent < nb_to_send)
                    rte_pktmbuf_free(pkts[sent++]);
            }
            nb_to_send = 0;
        }
    }

    if (nb_to_send > 0)
    {
        uint16_t sent;
        sent = rte_ring_enqueue_burst(params->tx_ring,
                                      (void *)pkts, nb_to_send, NULL);
        if (unlikely(sent < nb_to_send))
        {
            RTE_LOG_DP(INFO, RING,
                       "%s:Resend request pkt loss due to full tx_ring\n", __func__);
            while (sent < nb_to_send)
                rte_pktmbuf_free(pkts[sent++]);
        }

        if (sent < BURST_SIZE_TX)
        {
            while (sent < BURST_SIZE_TX)
                rte_pktmbuf_free(pkts[sent++]);
        }
    }
}

int lcore_recv(struct lcore_params *params)
{
    printf("\nCore %u doing recv task.\n", rte_lcore_id());

    struct linked_hash *hashtbl = NULL;
    struct rte_hash_parameters hash_params = {
        .name = "recv_table",
        .entries = MAX_OUTSTANDING_RECVS,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE
    };

    hashtbl = linked_hash_create(&hash_params);
    if (hashtbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create recv linked hash\n");
    }

    uint64_t prev_resend_request = rte_get_timer_cycles();

    while (!params->quit_signal_recv)
    {
        // dequeue pkts from rx_recv_ring
        struct rte_mbuf *pkts[BURST_SIZE_RX];
        unsigned int available;
        unsigned nb_rx = rte_ring_dequeue_burst(params->rx_recv_ring, (void *)pkts, BURST_SIZE_RX, &available);

        if (likely(nb_rx > 0))
        {
            rte_prefetch_non_temporal((void *)pkts[0]);
            rte_prefetch_non_temporal((void *)pkts[1]);
            rte_prefetch_non_temporal((void *)pkts[2]);

            for (unsigned int i = 0; i < nb_rx; i++)
            {
                rte_prefetch_non_temporal((void *)pkts[i + 3]);
                recv_pkt(params, pkts[i], hashtbl);
            }
        }

        uint64_t now = rte_get_timer_cycles();
        uint64_t resend_cycles = (RESEND_TIME_US * rte_get_timer_hz()) / 1e6;

        // only send resend requests if there are no pkts to receive, or we have not sent
        // resend requests in a while
        if ((available == 0 && now > resend_cycles) || prev_resend_request + resend_cycles < now)
        {
            // send resend requests for missing packets
            prev_resend_request = now;
            request_resends(params, hashtbl, now - resend_cycles);
        }
    }

    RTE_LOG(INFO, HASH,
            "%s:Number of elements in recv linked hash table: %u\n", __func__, linked_hash_count(hashtbl));

    linked_hash_free(hashtbl);
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