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

static inline void set_template_hdr(char *template_hdr, const struct msg_info *info, uint32_t msgid);
static inline void send_msg(struct lcore_params *params, struct msg_send_record *send_record, struct linked_hash *hashtbl, uint32_t msg_id);
static inline void recv_ctrl_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct linked_hash *hashtbl);
static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr);
static inline void send_probes(struct lcore_params *params, struct linked_hash *hashtbl, uint64_t probe_before);

static inline void set_template_hdr(char *template_hdr, const struct msg_info *info, uint32_t msgid)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = (struct rte_ether_hdr *)&template_hdr[0];
    ip_hdr = (struct rte_ipv4_hdr *)&template_hdr[sizeof(struct rte_ether_hdr)];
    dpdk_hdr = (struct dpdk_transport_hdr *)&template_hdr[sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)];

    // Initialize DPDK Transport header.
    dpdk_hdr->msgid = rte_cpu_to_be_32(msgid);
    dpdk_hdr->msg_len = rte_cpu_to_be_32(info->length);
    dpdk_hdr->pktid = 0; //placeholder
    dpdk_hdr->type = DPDK_TRANSPORT_MSGDATA;

    // Initialize IP header.
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_DPDK_TRANSPORT;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + MAX_PKT_MSGDATA_LEN);
    ip_hdr->src_addr = rte_cpu_to_be_32(info->src_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(info->dst_ip);

    set_ipv4_cksum(ip_hdr);

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

static inline void send_msg(struct lcore_params *params, struct msg_send_record *send_record, struct linked_hash *hashtbl, uint32_t msgid)
{
    struct msg_info *info = &send_record->info;
    char template_hdr[TOTAL_HDR_SIZE];
    set_template_hdr(template_hdr, info, msgid);

    struct msg_key key = {.src_ip = info->src_ip, .dst_ip = info->dst_ip, .msgid = msgid};
    // store msg_buf in hash table
    if (unlikely(linked_hash_add_key_data(hashtbl, (void *)&key, (void *)send_record) < 0))
    {
        // this should never happen since the outstanding_sends field of params prevents the
        // user from sending too many messages
        RTE_LOG_DP(ERR, HASH,
                   "%s:Unexpected msg loss due to failed linked_hash_add_key_data\n", __func__);
        rte_free(send_record->msg);
        rte_free(send_record);
        return;
    }

    // break msg into pkts and burst send
    struct rte_mbuf *pkts[BURST_SIZE_TX];
    uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(info->length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;

    for (uint8_t pktid_base = 0; pktid_base < total_pkts; pktid_base += BURST_SIZE_TX)
    {
        uint8_t nb_to_send = RTE_MIN(BURST_SIZE_TX, total_pkts - pktid_base);
        if (unlikely(rte_pktmbuf_alloc_bulk(params->mbuf_pool, pkts, nb_to_send) < 0))
        {
            RTE_LOG_DP(INFO, RING,
                        "%s:Send pkt loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
            continue;
        }

        for (uint8_t pktid_offset = 0; pktid_offset < nb_to_send; pktid_offset++)
        {
            uint8_t pktid = pktid_base + pktid_offset;
            struct rte_mbuf *pkt = pkts[pktid_offset];
            uint16_t msgdata_len = RTE_MIN(MAX_PKT_MSGDATA_LEN, info->length - pktid * ((uint32_t) MAX_PKT_MSGDATA_LEN));

            pkt->data_len = TOTAL_HDR_SIZE + msgdata_len;
            pkt->pkt_len = TOTAL_HDR_SIZE + msgdata_len;
            pkt->port = info->portid;

            // copy over template header
            rte_memcpy(rte_pktmbuf_mtod(pkt, void *),
                        (void *)template_hdr,
                        TOTAL_HDR_SIZE);

            // edit ipv4 packet length and recompute checksum if necessary
            if (unlikely(msgdata_len != MAX_PKT_MSGDATA_LEN)) // optimized for longer messages
            {
                struct rte_ipv4_hdr *ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *,
                                                                        sizeof(struct rte_ether_hdr));
                ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + msgdata_len);
                set_ipv4_cksum(ip_hdr);
            }

            // set dpdk transport header pktid
            struct dpdk_transport_hdr *dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                                                            sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
            dpdk_hdr->pktid = pktid;

            // copy over packet msg data
            rte_memcpy(rte_pktmbuf_mtod_offset(pkt, void *, TOTAL_HDR_SIZE),
                        (void *)&(send_record->msg[pktid * MAX_PKT_MSGDATA_LEN]),
                        msgdata_len);
        }

        // enqueue packet buffers to tx_ring
        uint16_t sent;
        sent = rte_ring_enqueue_burst(params->tx_ring,
                                        (void *)pkts, nb_to_send, NULL);
        if (unlikely(sent < nb_to_send))
        {
            RTE_LOG_DP(INFO, RING,
                        "%s:Pkt loss due to full tx_ring\n", __func__);
            while (sent < nb_to_send)
                rte_pktmbuf_free(pkts[sent++]);
        }
    }

    send_record->time = rte_get_timer_cycles();
}

static inline void recv_ctrl_pkt(struct lcore_params *params, struct rte_mbuf *pkt, struct linked_hash *hashtbl){
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
    int32_t pos = linked_hash_lookup_data(hashtbl, &key, (void *)&send_record);
    if (unlikely(pos < 0))
    {
        rte_pktmbuf_free(pkt);
        return;
    }

    if (dpdk_hdr->type == DPDK_TRANSPORT_COMPLETE){
        rte_free(send_record->msg);
        rte_free(send_record);
        linked_hash_del_key(hashtbl, &key); 
        rte_atomic16_dec(&params->outstanding_sends);
    }
    else if (dpdk_hdr->type == DPDK_TRANSPORT_RESEND){
        struct msg_info *info = &send_record->info;
        char template_hdr[TOTAL_HDR_SIZE];
        set_template_hdr(template_hdr, info, key.msgid);

        uint8_t nb_resends = (uint8_t) rte_be_to_cpu_32(dpdk_hdr->msg_len);
        struct rte_mbuf *resend_pkts[BURST_SIZE_TX];
        uint8_t *pktids = rte_pktmbuf_mtod_offset(pkt, uint8_t *, TOTAL_HDR_SIZE);

        for(uint8_t i_base = 0; i_base < nb_resends; i_base+=BURST_SIZE_TX){
            uint8_t nb_to_send = RTE_MIN(BURST_SIZE_TX, nb_resends - i_base);
            if (unlikely(rte_pktmbuf_alloc_bulk(params->mbuf_pool, resend_pkts, nb_to_send) < 0))
            {
                RTE_LOG_DP(INFO, MBUF,
                           "%s:Resend packet loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
                continue;
            }

            for (uint8_t i_offset = 0; i_offset < nb_to_send; i_offset++)
            {
                uint8_t i = i_base + i_offset;
                uint8_t pktid = pktids[i];
                struct rte_mbuf *resend_pkt = resend_pkts[i_offset];
                uint16_t msgdata_len = RTE_MIN(MAX_PKT_MSGDATA_LEN, info->length - pktid * ((uint32_t) MAX_PKT_MSGDATA_LEN));

                resend_pkt->data_len = TOTAL_HDR_SIZE + msgdata_len;
                resend_pkt->pkt_len = TOTAL_HDR_SIZE + msgdata_len;
                resend_pkt->port = info->portid;

                // copy over template header
                rte_memcpy(rte_pktmbuf_mtod(resend_pkt, void *),
                           (void *)template_hdr,
                           TOTAL_HDR_SIZE);

                // edit ipv4 packet length and recompute checksum if necessary
                if (unlikely(msgdata_len != MAX_PKT_MSGDATA_LEN)) // optimized for longer messages
                {
                    struct rte_ipv4_hdr *resend_ip_hdr = rte_pktmbuf_mtod_offset(resend_pkt, struct rte_ipv4_hdr *,
                                                                          sizeof(struct rte_ether_hdr));
                    resend_ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + msgdata_len);
                    set_ipv4_cksum(resend_ip_hdr);
                }

                // set dpdk transport header pktid
                struct dpdk_transport_hdr *resend_dpdk_hdr = rte_pktmbuf_mtod_offset(resend_pkt, struct dpdk_transport_hdr *,
                                                                              sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
                resend_dpdk_hdr->pktid = pktid;

                // copy over packet msg data
                rte_memcpy(rte_pktmbuf_mtod_offset(resend_pkt, void *, TOTAL_HDR_SIZE),
                           (void *)&(send_record->msg[pktid * MAX_PKT_MSGDATA_LEN]),
                           msgdata_len);
            }

            // enqueue packet buffers to tx_ring
            uint16_t sent;
            sent = rte_ring_enqueue_burst(params->tx_ring,
                                          (void *)resend_pkts, nb_to_send, NULL);
            if (unlikely(sent < nb_to_send))
            {
                RTE_LOG_DP(INFO, RING,
                           "%s:Resend packet loss due to full tx_ring\n", __func__);
                while (sent < nb_to_send)
                    rte_pktmbuf_free(resend_pkts[sent++]);
            }
        }

        send_record->time = rte_get_timer_cycles();
        linked_hash_move_pos_to_back(hashtbl, pos);
    }
    rte_pktmbuf_free(pkt);
}

static inline void send_probes(struct lcore_params *params, struct linked_hash *hashtbl, uint64_t probe_before){
    struct msg_key *key;
    struct msg_send_record *send_record;
    int32_t next = 0;

    uint8_t nb_to_send = 0;
    struct rte_mbuf *pkts[BURST_SIZE_TX];
    while (1)
    {
        int32_t pos = linked_hash_iterate(hashtbl, (void *)&key, (void *)&send_record, &next);
        if (pos < 0 || send_record->time >= probe_before)
            break;

        if (nb_to_send == 0)
        {
            if (unlikely(rte_pktmbuf_alloc_bulk(params->mbuf_pool, pkts, BURST_SIZE_TX) < 0))
            {
                RTE_LOG_DP(INFO, MBUF,
                           "%s:Probe pkt loss due to failed rte_pktmbuf_alloc_bulk\n", __func__);
                continue;
            }
        }

        struct rte_mbuf *pkt = pkts[nb_to_send];
        pkt->data_len = TOTAL_HDR_SIZE;
        pkt->pkt_len = TOTAL_HDR_SIZE;
        pkt->port = send_record->info.portid;

        set_template_hdr(rte_pktmbuf_mtod(pkt, char*), &send_record->info, key->msgid);
        struct rte_ipv4_hdr *ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *,
                                                                          sizeof(struct rte_ether_hdr));
        ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr));
        set_ipv4_cksum(ip_hdr);

        struct dpdk_transport_hdr *dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                                                              sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
        dpdk_hdr->pktid = 0xFF;

        send_record->time = rte_get_timer_cycles();
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

int lcore_send(struct lcore_params *params)
{

    printf("\nCore %u doing send task.\n", rte_lcore_id());

    struct linked_hash *hashtbl = NULL;
    struct rte_hash_parameters hash_params = {
        .name = "send_table",
        .entries = MAX_OUTSTANDING_SENDS,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_EXT_TABLE
    };

    hashtbl = linked_hash_create(&hash_params);
    if (hashtbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create send linked hash\n");
    }

    uint32_t next_msgid = 0;
    uint64_t prev_probe = rte_get_timer_cycles();

    while (!params->quit_signal_send)
    {
        unsigned int available_send;
        unsigned int available_ctrl;

        // dequeue and send msg from send_ring
        struct msg_send_record *send_record;
        if (rte_ring_dequeue_burst(params->send_ring, (void *)&send_record, 1, &available_send) > 0)
            send_msg(params, send_record, hashtbl, next_msgid++);

        // process received control messages from rx_send_ring
        struct rte_mbuf *pkts[BURST_SIZE_RX];
        unsigned nb_rx = rte_ring_dequeue_burst(params->rx_send_ring, (void *)pkts, BURST_SIZE_RX, &available_ctrl);
        if (likely(nb_rx > 0))
        {
            rte_prefetch_non_temporal((void *)pkts[0]);
            rte_prefetch_non_temporal((void *)pkts[1]);
            rte_prefetch_non_temporal((void *)pkts[2]);

            for (unsigned i = 0; i < nb_rx; i++){
                rte_prefetch_non_temporal((void *)pkts[i+3]);
                recv_ctrl_pkt(params, pkts[i], hashtbl);
            }
        }

        uint64_t now = rte_get_timer_cycles();
        uint64_t probe_cycles = (PROBE_TIME_US * rte_get_timer_hz()) / 1e6;

        if ((available_send == 0 && available_ctrl == 0 && now > probe_cycles) || (prev_probe + probe_cycles < now))
        {
            // send resend requests for missing packets
            prev_probe = now;
            send_probes(params, hashtbl, now - probe_cycles);
        }
    }
    
    RTE_LOG(INFO, HASH,
            "%s:Number of elements in send linked hash: %u\n", __func__, linked_hash_count(hashtbl));

    linked_hash_free(hashtbl);
    printf("\nCore %u exiting send task.\n", rte_lcore_id());
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