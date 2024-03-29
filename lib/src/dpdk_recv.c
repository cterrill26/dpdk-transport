#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_cycles.h>
#include <string.h>
#include "dpdk_transport.h"
#include "dpdk_recv.h"
#include "dpdk_common.h"
#include "linked_hash.h"

#define RESEND_TIME_US 5000LL
#define MAX_UNANSWERED_RESEND_REQUESTS 100
#define NB_CTRL_MBUFS MAX_ACTIVE_RECVS
#define MBUF_CACHE_SIZE 128

struct recv_objs
{
    struct linked_hash *active_recvs_tbl;
    struct linked_hash *completed_recvs_tbl;
    struct rte_mempool *ctrl_mbuf_pool;
    struct lcore_params *params;
};

static inline void set_headers(struct rte_mbuf *pkt, struct msg_info *info, uint32_t msgid, uint8_t type, uint32_t data_len);
static inline void send_completed_pkt(struct recv_objs *objs, struct msg_info *info, uint32_t msgid);
static inline void recv_msg(struct recv_objs *objs, struct msg_recv_record *recv_record, struct msg_key *key, int32_t pos);
static inline void rte_mbuf_to_msg_info(struct rte_mbuf *pkt, struct msg_info *info);
static inline void recv_pkt(struct recv_objs *objs, struct rte_mbuf *pkt);
static inline void request_resends(struct recv_objs *objs, uint64_t resend_before);

static inline void set_headers(struct rte_mbuf *pkt, struct msg_info *info, uint32_t msgid, uint8_t type, uint32_t data_len)
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
    ip_hdr->src_addr = rte_cpu_to_be_32(info->dst_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(info->src_ip);

    set_ipv4_cksum(ip_hdr);

    // Initialize ethernet header.
    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;
    // flipped since we are sending a pkt from receiver to sender
    mac_addr.as_int = rte_cpu_to_be_64(info->src_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->d_addr);
    mac_addr.as_int = rte_cpu_to_be_64(info->dst_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->s_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static inline void send_completed_pkt(struct recv_objs *objs, struct msg_info *info, uint32_t msgid)
{
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(objs->ctrl_mbuf_pool);
    if (unlikely(pkt == NULL))
    {
        RTE_LOG_DP(DEBUG, MBUF,
                   "%s:Completed pkt loss due to failed rte_pktmbuf_alloc\n", __func__);
        return;
    }

    pkt->data_len = TOTAL_HDR_SIZE;
    pkt->pkt_len = TOTAL_HDR_SIZE;
    pkt->port = info->portid;
    set_headers(pkt, info, msgid, DPDK_TRANSPORT_COMPLETE, 0);

    if (unlikely(rte_ring_enqueue(objs->params->recv_tx_ring, (void *)pkt) != 0))
    {
        RTE_LOG_DP(DEBUG, RING,
                   "%s:Completed pkt loss due to full recv_tx_ring\n", __func__);
        rte_pktmbuf_free(pkt);
    }
}

static inline void recv_msg(struct recv_objs *objs, struct msg_recv_record *recv_record, struct msg_key *key, int32_t pos)
{
    send_completed_pkt(objs, &recv_record->info, key->msgid);

    if (linked_hash_add_key_data(objs->completed_recvs_tbl, key, NULL) < 0)
    {
        // completed table is full, remove oldest element
        struct msg_key *front_key;
        void *d;
        linked_hash_front(objs->completed_recvs_tbl, (void *)&front_key, &d);
        linked_hash_del_key(objs->completed_recvs_tbl, front_key);
        if (unlikely(linked_hash_add_key_data(objs->completed_recvs_tbl, key, NULL) < 0))
        {
            RTE_LOG_DP(ERR, HASH,
                       "%s:Failed to add msg to completed table\n", __func__);
        }
    }

    // pass full msg on to recv_ring
    if (likely(rte_ring_enqueue(objs->params->recv_ring, recv_record) == 0))
    {
        linked_hash_del_key(objs->active_recvs_tbl, key);
    }
    else
    {
        // in the case the enqueue fails, the entry will remain in the hashtbl, and the resend_request function
        // will attempt to enqueue the recv_record later on
        linked_hash_move_pos_to_front(objs->active_recvs_tbl, pos);
    }
}

static inline void rte_mbuf_to_msg_info(struct rte_mbuf *pkt, struct msg_info *info)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                       sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;

    mac_addr.as_int = 0LL;
    rte_ether_addr_copy(&eth_hdr->s_addr, &mac_addr.as_addr);
    info->src_mac = rte_be_to_cpu_64(mac_addr.as_int);
    mac_addr.as_int = 0LL;
    rte_ether_addr_copy(&eth_hdr->d_addr, &mac_addr.as_addr);
    info->dst_mac = rte_be_to_cpu_64(mac_addr.as_int);
    info->src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    info->dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    info->portid = pkt->port;
    info->length = rte_be_to_cpu_32(dpdk_hdr->msg_len);
}

static inline void recv_pkt(struct recv_objs *objs, struct rte_mbuf *pkt)
{
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                       sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    struct msg_key key;
    key.src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    key.dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    key.msgid = rte_be_to_cpu_32(dpdk_hdr->msgid);

    struct msg_recv_record *recv_record;
    int32_t pos = linked_hash_lookup_data(objs->active_recvs_tbl, &key, (void *)&recv_record);
    if (pos < 0)
    {
        void *d;
        if (unlikely(linked_hash_lookup_data(objs->completed_recvs_tbl, &key, &d) >= 0))
        {
            // this is a pkt from a completed message
            uint8_t pktid = dpdk_hdr->pktid;
            if ((pktid & 0xFF) == 0xFF)
            {
                // msg probe, sender never received completed pkt
                struct msg_info info;
                rte_mbuf_to_msg_info(pkt, &info);
                send_completed_pkt(objs, &info, key.msgid);
            }

            rte_pktmbuf_free(pkt);
            return;
        }

        // this is the first packet of a new msg
        if (unlikely(rte_mempool_get(objs->params->recv_record_pool, (void *)&recv_record) < 0))
        {
            RTE_LOG_DP(DEBUG, MEMPOOL,
                       "%s:Recv pkt loss due to failed rte_mempool_get\n", __func__);
            rte_pktmbuf_free(pkt);
            return;
        }

        // init recv_record with zeroed values
        recv_record->nb_pkts_received = 0;
        recv_record->nb_resend_requests = 0;
        recv_record->time = rte_get_timer_cycles();
        for (uint8_t i = 0; i < MAX_PKTS_IN_MSG; i++)
            recv_record->pkts[i] = NULL;
        for (uint8_t i = 0; i < RTE_ALIGN_MUL_CEIL(MAX_PKTS_IN_MSG, 8) / 8; i++)
            recv_record->pkts_received_mask[i] = 0;

        rte_mbuf_to_msg_info(pkt, &recv_record->info);

        pos = linked_hash_add_key_data(objs->active_recvs_tbl, (void *)&key, (void *)recv_record);
        if (unlikely(pos < 0))
        {
            RTE_LOG_DP(DEBUG, HASH,
                       "%s:Recv pkt loss due to failed linked_hash_add_key_data\n", __func__);
            rte_mempool_put(objs->params->recv_record_pool, recv_record);
            rte_pktmbuf_free(pkt);
            return;
        }
    }

    uint8_t pktid = dpdk_hdr->pktid;
    if (unlikely((pktid & 0xFF) == 0xFF || (recv_record->pkts_received_mask[pktid / 8] & (1 << (pktid % 8))) != 0))
    {
        // is a probe or duplicated pkt
        rte_pktmbuf_free(pkt);
        return;
    }

    recv_record->pkts[pktid] = pkt;
    recv_record->nb_pkts_received++;
    recv_record->pkts_received_mask[pktid / 8] |= (1 << (pktid % 8));
    recv_record->time = rte_get_timer_cycles();
    recv_record->nb_resend_requests = 0;

    uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->info.length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
    if (recv_record->nb_pkts_received == total_pkts)
        recv_msg(objs, recv_record, &key, pos);
    else
        linked_hash_move_pos_to_back(objs->active_recvs_tbl, pos);
}

static inline void request_resends(struct recv_objs *objs, uint64_t resend_before)
{
    struct msg_key *key;
    struct msg_recv_record *recv_record;
    int32_t next = 0;

    uint8_t nb_to_send = 0;
    struct rte_mbuf *pkts[BURST_SIZE_TX];
    while (1)
    {
        int32_t pos = linked_hash_iterate(objs->active_recvs_tbl, (void *)&key, (void *)&recv_record, &next);
        if (unlikely(pos < 0))
            break;

        uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->info.length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
        uint8_t nb_pkts_missing = total_pkts - recv_record->nb_pkts_received;

        // completed msgs will be at the front of the linked hash
        if (unlikely(nb_pkts_missing == 0))
        {
            // recv_record must have failed to be enqueud in recv_ring earlier, try again
            if (likely(rte_ring_enqueue(objs->params->recv_ring, recv_record) == 0))
                linked_hash_del_key(objs->active_recvs_tbl, key);

            continue;
        }

        // passed all the completed msgs in the linked hash
        if (recv_record->time >= resend_before)
            break;

        if (unlikely(recv_record->nb_resend_requests >= MAX_UNANSWERED_RESEND_REQUESTS))
        {
            RTE_LOG_DP(DEBUG, MBUF,
                       "%s:Deleted recv_record after %d unasnwered resend requests\n", __func__, MAX_UNANSWERED_RESEND_REQUESTS);
            linked_hash_del_key(objs->active_recvs_tbl, key);
            for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
                rte_pktmbuf_free(recv_record->pkts[pktid]);
            rte_mempool_put(objs->params->recv_record_pool, recv_record);
            continue;
        }

        if (nb_to_send == 0)
        {
            if (unlikely(rte_pktmbuf_alloc_bulk(objs->ctrl_mbuf_pool, pkts, BURST_SIZE_TX) < 0))
            {
                RTE_LOG_DP(DEBUG, MBUF,
                           "%s:Resend request pkt loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
                return;
            }
        }

        struct rte_mbuf *pkt = pkts[nb_to_send++];
        pkt->data_len = TOTAL_HDR_SIZE + nb_pkts_missing;
        pkt->pkt_len = TOTAL_HDR_SIZE + nb_pkts_missing;
        pkt->port = recv_record->info.portid;
        set_headers(pkt, &recv_record->info, key->msgid, DPDK_TRANSPORT_RESEND, nb_pkts_missing);

        // fill pkt data contents with missing pktids
        uint8_t *resend_list = rte_pktmbuf_mtod_offset(pkt, uint8_t *, TOTAL_HDR_SIZE);
        uint8_t resend_idx = 0;
        for (uint8_t pktid_base = 0; pktid_base < total_pkts; pktid_base += 8)
        {
            uint8_t recv_mask = recv_record->pkts_received_mask[pktid_base / 8];
            for (uint8_t pktid_offset = 0; pktid_base + pktid_offset < RTE_MIN(total_pkts, pktid_base + 8); pktid_offset++)
            {
                uint8_t pktid = pktid_base + pktid_offset;
                if ((recv_mask & (1 << pktid_offset)) == 0)
                    resend_list[resend_idx++] = pktid;
            }
        }

        recv_record->time = rte_get_timer_cycles();
        recv_record->nb_resend_requests++;
        linked_hash_move_pos_to_back(objs->active_recvs_tbl, pos);

        if (nb_to_send == BURST_SIZE_TX)
        {
            uint16_t sent;
            sent = rte_ring_enqueue_burst(objs->params->recv_tx_ring,
                                          (void *)pkts, BURST_SIZE_TX, NULL);
            if (unlikely(sent < BURST_SIZE_TX))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Resend request pkt loss due to full recv_tx_ring\n", __func__);
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
        sent = rte_ring_enqueue_burst(objs->params->recv_tx_ring,
                                      (void *)pkts, nb_to_send, NULL);
        if (unlikely(sent < nb_to_send))
        {
            RTE_LOG_DP(DEBUG, RING,
                       "%s:Resend request pkt loss due to full recv_tx_ring\n", __func__);
        }

        while (sent < BURST_SIZE_TX)
            rte_pktmbuf_free(pkts[sent++]);
    }
}

int lcore_recv(struct lcore_params *params)
{
    printf("\nCore %u doing recv task.\n", rte_lcore_id());

    struct recv_objs objs;
    objs.params = params;

    struct rte_hash_parameters active_recvs_params = {
        .name = "active_recvs_tbl",
        .entries = MAX_ACTIVE_RECVS,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE};

    objs.active_recvs_tbl = linked_hash_create(&active_recvs_params);
    if (objs.active_recvs_tbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create active recvs linked hash\n");
    }

    struct rte_hash_parameters completed_recvs_params = {
        .name = "compl_recvs_tbl",
        .entries = MAX_COMPLETED_RECVS,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE};

    objs.completed_recvs_tbl = linked_hash_create(&completed_recvs_params);
    if (objs.completed_recvs_tbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create completed recvs linked hash\n");
    }

    objs.ctrl_mbuf_pool = rte_pktmbuf_pool_create("CTRL_MBUF_POOL", NB_CTRL_MBUFS,
                                                  MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (objs.ctrl_mbuf_pool == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create ctrl mbuf pool\n");
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
                recv_pkt(&objs, pkts[i]);
            }
        }

        uint64_t now = rte_get_timer_cycles();
        uint64_t resend_cycles = (RESEND_TIME_US * rte_get_timer_hz()) / 1e6;

        // only send resend requests if there are no pkts to receive, or we have not sent
        // resend requests in a while
        if (unlikely((available == 0 && now > resend_cycles) || prev_resend_request + resend_cycles < now))
        {
            // send resend requests for missing packets
            prev_resend_request = now;
            request_resends(&objs, now - resend_cycles);
        }
    }

    RTE_LOG(INFO, HASH,
            "%s:Number of elements in active recvs linked hash: %u\n", __func__, linked_hash_count(objs.active_recvs_tbl));
    linked_hash_free(objs.active_recvs_tbl);

    RTE_LOG(INFO, HASH,
            "%s:Number of elements in completed recvs linked hash: %u\n", __func__, linked_hash_count(objs.completed_recvs_tbl));
    linked_hash_free(objs.completed_recvs_tbl);

    RTE_LOG(INFO, MEMPOOL,
            "%s:Number of elements in ctrl mbuf pool: %u\n", __func__, rte_mempool_in_use_count(objs.ctrl_mbuf_pool));
    rte_mempool_free(objs.ctrl_mbuf_pool);

    printf("\nCore %u exiting recv task.\n", rte_lcore_id());
    return 0;
}