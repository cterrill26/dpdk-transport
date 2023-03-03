#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include "dpdk_transport.h"
#include "dpdk_send.h"
#include "dpdk_common.h"

#define SEND_TABLE_ENTRIES 1024

static inline void set_template_hdr(char *template_hdr, const struct msg_buf *buf, uint32_t msgid);
static inline void send_msg(struct lcore_params *params, struct msg_buf *buf, struct rte_hash *hashtbl, uint32_t msg_id);
static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr);

static inline void set_template_hdr(char *template_hdr, const struct msg_buf *buf, uint32_t msgid)
{
    uint16_t total_hdr_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr);
    uint16_t max_pkt_msgdata_len = RTE_ETHER_MAX_LEN - total_hdr_size;

    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct dpdk_transport_hdr *dpdk_hdr;

    eth_hdr = (struct rte_ether_hdr *)&template_hdr[0];
    ip_hdr = (struct rte_ipv4_hdr *)&template_hdr[sizeof(struct rte_ether_hdr)];
    dpdk_hdr = (struct dpdk_transport_hdr *)&template_hdr[sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)];

    // Initialize DPDK Transport header.
    dpdk_hdr->msgid = rte_cpu_to_be_32(msgid);
    dpdk_hdr->msg_len = rte_cpu_to_be_32(buf->info->length);
    dpdk_hdr->pktid = 0;
    dpdk_hdr->type = DPDK_TRANSPORT_MSGDATA;

    // Initialize IP header.
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_DPDK_TRANSPORT;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16((uint16_t)(sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr) + max_pkt_msgdata_len));
    ip_hdr->src_addr = rte_cpu_to_be_32(buf->info->src_ip);
    ip_hdr->dst_addr = rte_cpu_to_be_32(buf->info->dst_ip);

    set_ipv4_cksum(ip_hdr);

    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;
    mac_addr.as_int = rte_cpu_to_be_64(buf->info->src_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->s_addr);
    mac_addr.as_int = rte_cpu_to_be_64(buf->info->dst_mac);
    rte_ether_addr_copy(&mac_addr.as_addr, &eth_hdr->d_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

static inline void send_msg(struct lcore_params *params, struct msg_buf *buf, struct rte_hash *hashtbl, uint32_t msgid)
{

    uint16_t total_hdr_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct dpdk_transport_hdr);
    uint16_t max_pkt_msgdata_len = RTE_ETHER_MAX_LEN - total_hdr_size;

    char *template_hdr = rte_malloc("template_hdr", total_hdr_size, 0);
    set_template_hdr(template_hdr, buf, msgid);

    struct msg_key key = {.src_ip = buf->info->src_ip, .dst_ip = buf->info->dst_ip, .msgid = msgid};
    // store msg_buf in hash table
    if (unlikely(rte_hash_add_key_data(hashtbl, (void *)&key, (void *)buf) < 0))
    {
        RTE_LOG_DP(DEBUG, HASH,
                   "%s:Msg loss due to failed rte_hash_add_key_data\n", __func__);
        rte_free(buf->info);
        rte_free(buf->msg);
        rte_free(buf);
    }
    else
    {
        // break msg into pkts and burst send
        struct rte_mbuf *bufs[BURST_SIZE_TX];
        uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(buf->info->length, max_pkt_msgdata_len) / max_pkt_msgdata_len;

        for (uint8_t pktid_base = 0; pktid_base < total_pkts; pktid_base += BURST_SIZE_TX)
        {
            uint8_t nb_to_send = RTE_MIN(BURST_SIZE_TX, total_pkts - pktid_base);
            if (unlikely(rte_pktmbuf_alloc_bulk(params->mbuf_pool, bufs, nb_to_send) < 0))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Packet loss due to failed rte_mbuf_raw_alloc\n", __func__);
                continue;
            }

            for (uint8_t pktid_offset = 0; pktid_offset < nb_to_send; pktid_offset++)
            {
                uint8_t pktid = pktid_base + pktid_offset;
                struct rte_mbuf *pkt = bufs[pktid_offset];
                uint16_t msgdata_len = RTE_MIN(max_pkt_msgdata_len, buf->info->length - pktid * max_pkt_msgdata_len);
                pkt->data_len = total_hdr_size + msgdata_len;
                pkt->pkt_len = total_hdr_size + msgdata_len;
                pkt->port = buf->info->portid;

                // copy over template header
                rte_memcpy(rte_pktmbuf_mtod(pkt, void *),
                           (void *)template_hdr,
                           total_hdr_size);

                // edit ipv4 packet length and recompute checksum if necessary
                if (msgdata_len != max_pkt_msgdata_len)
                {
                    struct rte_ipv4_hdr *ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *,
                                                                          sizeof(struct rte_ether_hdr));
                    ip_hdr->total_length = rte_cpu_to_be_16(msgdata_len);
                    set_ipv4_cksum(ip_hdr);
                }

                // set dpdk transport header pktid
                struct dpdk_transport_hdr *dpdk_hdr = rte_pktmbuf_mtod_offset(pkt, struct dpdk_transport_hdr *,
                                                                              sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));
                dpdk_hdr->pktid = pktid;

                // copy over packet msg data
                rte_memcpy(rte_pktmbuf_mtod_offset(pkt, void *, total_hdr_size),
                           (void *)&(buf->msg[pktid * max_pkt_msgdata_len]),
                           msgdata_len);
            }

            // enqueue packet buffers to tx_ring
            uint16_t sent;
            sent = rte_ring_enqueue_burst(params->tx_ring,
                                          (void *)bufs, nb_to_send, NULL);
            if (unlikely(sent < nb_to_send))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Packet loss due to full tx_ring\n", __func__);
                while (sent < nb_to_send)
                    rte_pktmbuf_free(bufs[sent++]);
            }
        }
    }

    rte_free(template_hdr);
}

static int lcore_send(struct lcore_params *params)
{

    printf("\nCore %u doing send task.\n", rte_lcore_id());

    struct rte_hash *hashtbl = NULL;
    struct rte_hash_parameters hash_params = {
        .name = "send_table",
        .entries = SEND_TABLE_ENTRIES,
        .key_len = sizeof(struct msg_key),
        .hash_func = rte_hash_crc,
        .socket_id = rte_socket_id(),
    };

    hashtbl = rte_hash_create(&hash_params);
    if (hashtbl == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create send hash table\n");
    }

    uint32_t next_msgid = 0;
    while (!params->quit_signal_send)
    {
        // dequeue and send msg from send_ring
        struct msg_buf *buf;
        if (likely(rte_ring_dequeue(params->send_ring, (void *)&buf) == 0))
            send_msg(params, buf, hashtbl, next_msgid++);

        // process received control messages from rx_send_ring
    }

    rte_hash_free(hashtbl);
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