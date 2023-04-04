#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_common.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include "dpdk_transport.h"
#include "dpdk_send.h"
#include "dpdk_common.h"
#include "linked_hash.h"

#define PROBE_TIME_US 50000LL
#define NB_PROBE_MBUFS ((1 * 1024) - 1)
#define MBUF_CACHE_SIZE 128

struct send_objs
{
    struct linked_hash *active_sends_tbl;
    struct rte_mempool *probe_mbuf_pool;
    struct lcore_params *params;
};

static inline void set_probe_hdr(char *template_hdr, const struct msg_info *info, uint32_t msgid);
static inline void send_msg(struct send_objs *objs, struct msg_send_record *send_record);
static inline void recv_ctrl_pkt(struct send_objs *objs, struct rte_mbuf *pkt);
static inline void send_probes(struct send_objs *objs, uint64_t probe_before);

static inline void set_probe_hdr(char *template_hdr, const struct msg_info *info, uint32_t msgid)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = (struct rte_ether_hdr *)&template_hdr[0];
    ip_hdr = (struct rte_ipv4_hdr *)&template_hdr[sizeof(struct rte_ether_hdr)];
    dpdk_hdr = (struct dpdk_transport_hdr *)&template_hdr[sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)];

    // Set DPDK Transport header.
    dpdk_hdr->msgid = rte_cpu_to_be_32(msgid);
    dpdk_hdr->msg_len = rte_cpu_to_be_32(info->length);
    dpdk_hdr->pktid = 0xFF;
    dpdk_hdr->type = DPDK_TRANSPORT_MSGDATA;

    // Set IP header.
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_DPDK_TRANSPORT;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr));
    ip_hdr->src_addr = rte_cpu_to_be_32(info->src_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(info->dst_ip);

    set_ipv4_cksum(ip_hdr);

    // Set ethernet header.
    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;
    mac_addr.as_int = rte_cpu_to_be_64(info->src_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->s_addr);
    mac_addr.as_int = rte_cpu_to_be_64(info->dst_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->d_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static inline void send_msg(struct send_objs *objs, struct msg_send_record *send_record)
{
    send_record->time = rte_get_timer_cycles();
    struct msg_info *info = &send_record->info;
    struct msg_key key = {.src_ip = info->src_ip, .dst_ip = info->dst_ip, .msgid = send_record->msgid};
    uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(info->length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;

    // store send_record in hash table
    if (unlikely(linked_hash_add_key_data(objs->active_sends_tbl, (void *)&key, (void *)send_record) < 0))
    {
        // this should never happen since the outstanding_sends field of params prevents the
        // user from sending too many messages
        RTE_LOG_DP(ERR, HASH,
                   "%s:Unexpected msg loss due to failed linked_hash_add_key_data\n", __func__);
        for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
            rte_pktmbuf_free(send_record->pkts[pktid]);
        rte_free(send_record);
        return;
    }

    // burst send pkts
    for (uint8_t pktid_base = 0; pktid_base < total_pkts; pktid_base += BURST_SIZE_TX)
    {
        struct rte_mbuf **pkts = &send_record->pkts[pktid_base];
        uint8_t nb_to_send = RTE_MIN(BURST_SIZE_TX, total_pkts - pktid_base);

        for (uint8_t pktid_offset = 0; pktid_offset < nb_to_send; pktid_offset++)
            rte_pktmbuf_refcnt_update(pkts[pktid_offset], 1);

        // enqueue packet buffers to tx_ring
        uint16_t sent;
        sent = rte_ring_enqueue_burst(objs->params->tx_ring,
                                      (void *)pkts, nb_to_send, NULL);
        if (unlikely(sent < nb_to_send))
        {
            RTE_LOG_DP(INFO, RING,
                       "%s:Pkt loss due to full tx_ring\n", __func__);
            while (sent < nb_to_send)
                rte_pktmbuf_refcnt_update(pkts[sent++], -1);

            return;
        }
    }
}

static inline void recv_ctrl_pkt(struct send_objs *objs, struct rte_mbuf *pkt)
{
    struct msg_key key;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                       sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    key.src_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    key.dst_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    key.msgid = rte_be_to_cpu_32(dpdk_hdr->msgid);
    struct msg_send_record *send_record;
    int32_t pos = linked_hash_lookup_data(objs->active_sends_tbl, &key, (void *)&send_record);
    if (unlikely(pos < 0))
    {
        rte_pktmbuf_free(pkt);
        return;
    }

    struct msg_info *info = &send_record->info;
    if (dpdk_hdr->type == DPDK_TRANSPORT_COMPLETE)
    {
        uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(info->length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
        for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
            rte_pktmbuf_free(send_record->pkts[pktid]);

        rte_free(send_record);
        linked_hash_del_key(objs->active_sends_tbl, &key);
        rte_atomic16_dec(&objs->params->outstanding_sends);
    }
    else if (dpdk_hdr->type == DPDK_TRANSPORT_RESEND)
    {
        send_record->time = rte_get_timer_cycles();
        linked_hash_move_pos_to_back(objs->active_sends_tbl, pos);
        uint8_t nb_resends = (uint8_t)rte_be_to_cpu_32(dpdk_hdr->msg_len);
        struct rte_mbuf *pkts[BURST_SIZE_TX];
        uint8_t *pktids = rte_pktmbuf_mtod_offset(pkt, uint8_t *, TOTAL_HDR_SIZE);

        for (uint8_t i_base = 0; i_base < nb_resends; i_base += BURST_SIZE_TX)
        {
            uint8_t nb_to_send = RTE_MIN(BURST_SIZE_TX, nb_resends - i_base);

            for (uint8_t i_offset = 0; i_offset < nb_to_send; i_offset++)
            {
                uint8_t i = i_base + i_offset;
                uint8_t pktid = pktids[i];
                pkts[i_offset] = send_record->pkts[pktid];
                rte_pktmbuf_refcnt_update(pkts[i_offset], 1);
            }

            // enqueue packet buffers to tx_ring
            uint16_t sent;
            sent = rte_ring_enqueue_burst(objs->params->tx_ring,
                                          (void *)pkts, nb_to_send, NULL);
            if (unlikely(sent < nb_to_send))
            {
                RTE_LOG_DP(INFO, RING,
                           "%s:Resend packet loss due to full tx_ring\n", __func__);
                while (sent < nb_to_send)
                    rte_pktmbuf_refcnt_update(pkts[sent++], -1);

                break;
            }
        }
    }
    rte_pktmbuf_free(pkt);
}

static inline void send_probes(struct send_objs *objs, uint64_t probe_before)
{
    struct msg_key *key;
    struct msg_send_record *send_record;
    int32_t next = 0;

    uint8_t nb_to_send = 0;
    struct rte_mbuf *pkts[BURST_SIZE_TX];
    while (1)
    {
        int32_t pos = linked_hash_iterate(objs->active_sends_tbl, (void *)&key, (void *)&send_record, &next);
        if (pos < 0 || send_record->time >= probe_before)
            break;

        if (nb_to_send == 0)
        {
            if (unlikely(rte_pktmbuf_alloc_bulk(objs->probe_mbuf_pool, pkts, BURST_SIZE_TX) < 0))
            {
                RTE_LOG_DP(INFO, MBUF,
                           "%s:Probe pkt loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
                return;
            }
        }

        struct rte_mbuf *pkt = pkts[nb_to_send++];
        pkt->data_len = TOTAL_HDR_SIZE;
        pkt->pkt_len = TOTAL_HDR_SIZE;
        pkt->port = send_record->info.portid;

        set_probe_hdr(rte_pktmbuf_mtod(pkt, char *), &send_record->info, key->msgid);

        send_record->time = rte_get_timer_cycles();
        linked_hash_move_pos_to_back(objs->active_sends_tbl, pos);

        if (nb_to_send == BURST_SIZE_TX)
        {
            uint16_t sent;
            sent = rte_ring_enqueue_burst(objs->params->tx_ring,
                                          (void *)pkts, BURST_SIZE_TX, NULL);
            if (unlikely(sent < BURST_SIZE_TX))
            {
                RTE_LOG_DP(INFO, RING,
                           "%s:Resend request pkt loss due to full tx_ring\n", __func__);
                while (sent < BURST_SIZE_TX)
                    rte_pktmbuf_free(pkts[sent++]);

                return;
            }
            nb_to_send = 0;
        }
    }

    if (nb_to_send > 0)
    {
        uint16_t sent;
        sent = rte_ring_enqueue_burst(objs->params->tx_ring,
                                      (void *)pkts, nb_to_send, NULL);
        if (unlikely(sent < nb_to_send))
        {
            RTE_LOG_DP(INFO, RING,
                       "%s:Resend request pkt loss due to full tx_ring\n", __func__);
            while (sent < nb_to_send)
                rte_pktmbuf_free(pkts[sent++]);
        }

        while (sent < BURST_SIZE_TX)
            rte_pktmbuf_free(pkts[sent++]);
    }
}

int lcore_send(struct lcore_params *params)
{

    printf("\nCore %u doing send task.\n", rte_lcore_id());

    struct send_objs objs;
    objs.params = params;

    struct rte_hash_parameters active_sends_params = {
        .name = "active_sends_tbl",
        .entries = MAX_ACTIVE_SENDS,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE};

    objs.active_sends_tbl = linked_hash_create(&active_sends_params);
    if (objs.active_sends_tbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create active sends linked hash\n");
    }

    objs.probe_mbuf_pool = rte_pktmbuf_pool_create("PROBE_MBUF_POOL", NB_PROBE_MBUFS,
                                                   MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (objs.probe_mbuf_pool == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create probe mbuf pool\n");
    }

    uint64_t prev_probe = rte_get_timer_cycles();

    while (!params->quit_signal_send)
    {
        unsigned int available_send;
        unsigned int available_ctrl;

        // dequeue and send msg from send_ring
        struct msg_send_record *send_record;
        if (rte_ring_dequeue_burst(params->send_ring, (void *)&send_record, 1, &available_send) > 0)
            send_msg(&objs, send_record);

        // process received control messages from rx_send_ring
        struct rte_mbuf *pkts[BURST_SIZE_RX];
        unsigned nb_rx = rte_ring_dequeue_burst(params->rx_send_ring, (void *)pkts, BURST_SIZE_RX, &available_ctrl);
        if (likely(nb_rx > 0))
        {
            rte_prefetch_non_temporal((void *)pkts[0]);
            rte_prefetch_non_temporal((void *)pkts[1]);
            rte_prefetch_non_temporal((void *)pkts[2]);

            for (unsigned i = 0; i < nb_rx; i++)
            {
                rte_prefetch_non_temporal((void *)pkts[i + 3]);
                recv_ctrl_pkt(&objs, pkts[i]);
            }
        }

        uint64_t now = rte_get_timer_cycles();
        uint64_t probe_cycles = (PROBE_TIME_US * rte_get_timer_hz()) / 1e6;

        if (unlikely((available_send == 0 && available_ctrl == 0 && now > probe_cycles) || (prev_probe + probe_cycles < now)))
        {
            // send resend requests for missing packets
            prev_probe = now;
            send_probes(&objs, now - probe_cycles);
        }
    }

    RTE_LOG(INFO, HASH,
            "%s:Number of elements in active sends linked hash: %u\n", __func__, linked_hash_count(objs.active_sends_tbl));
    linked_hash_free(objs.active_sends_tbl);

    RTE_LOG(INFO, MEMPOOL,
            "%s:Number of elements in probe mbuf pool: %u\n", __func__, rte_mempool_in_use_count(objs.probe_mbuf_pool));
    rte_mempool_free(objs.probe_mbuf_pool);

    printf("\nCore %u exiting send task.\n", rte_lcore_id());
    return 0;
}