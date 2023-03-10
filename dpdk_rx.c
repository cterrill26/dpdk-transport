#include <rte_ethdev.h>
#include "dpdk_rx.h"
#include "dpdk_common.h"

static inline int is_control_pkt(const struct rte_mbuf *buf);
static inline int is_dpdk_transport_pkt(const struct rte_mbuf *buf);

static inline int is_control_pkt(const struct rte_mbuf *buf)
{
    struct dpdk_transport_hdr *hdr;
    hdr = rte_pktmbuf_mtod_offset(buf, struct dpdk_transport_hdr *,
                                  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    if (hdr->type == DPDK_TRANSPORT_MSGDATA)
        return 0;

    return 1;
}

static inline int is_dpdk_transport_pkt(const struct rte_mbuf *buf)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;

    eth_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));

    if (rte_be_to_cpu_16(eth_hdr->ether_type) == RTE_ETHER_TYPE_IPV4 && ip_hdr->next_proto_id == IPPROTO_DPDK_TRANSPORT)
        return 1;

    return 0;
}

int lcore_rx(struct lcore_params *params)
{
    const int socket_id = rte_socket_id();
    uint16_t portid;
    struct rte_mbuf *bufs[BURST_SIZE_RX];
    struct rte_mbuf *rx_send_bufs[BURST_SIZE_RX];
    struct rte_mbuf *rx_recv_bufs[BURST_SIZE_RX];

    RTE_ETH_FOREACH_DEV(portid)
    {
        if (rte_eth_dev_socket_id(portid) > 0 &&
            rte_eth_dev_socket_id(portid) != socket_id)
            printf("WARNING, port %u is on remote NUMA node to "
                   "RX thread.\n\tPerformance will not "
                   "be optimal.\n",
                   portid);
    }

    printf("\nCore %u doing packet RX.\n", rte_lcore_id());

    while (!params->quit_signal_rx)
    {
        RTE_ETH_FOREACH_DEV(portid)
        {
            const uint16_t nb_rx = rte_eth_rx_burst(portid, 0, bufs,
                                                    BURST_SIZE_RX);
            if (unlikely(nb_rx == 0))
                continue;

            uint16_t i;
            uint16_t nb_rx_send = 0;
            uint16_t nb_rx_recv = 0;

            rte_prefetch_non_temporal((void *)bufs[0]);
            rte_prefetch_non_temporal((void *)bufs[1]);
            rte_prefetch_non_temporal((void *)bufs[2]);
            for (i = 0; i < nb_rx; i++)
            {
                rte_prefetch_non_temporal((void *)bufs[i+3]);

                if (!is_dpdk_transport_pkt(bufs[i])){
                    rte_pktmbuf_free(bufs[i]);
                    continue;
                }
                //printf("Received pkt\n");
                //DumpHex(rte_pktmbuf_mtod(bufs[i], char *), bufs[i]->pkt_len);
                if (is_control_pkt(bufs[i]))
                    rx_send_bufs[nb_rx_send++] = bufs[i];
                else
                    rx_recv_bufs[nb_rx_recv++] = bufs[i];
            }

            uint16_t sent;
            sent = rte_ring_enqueue_burst(params->rx_send_ring,
                                          (void *)rx_send_bufs, nb_rx_send, NULL);
            if (unlikely(sent < nb_rx_send))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Rx pkt loss due to full rx_send_ring\n", __func__);
                while (sent < nb_rx_send)
                    rte_pktmbuf_free(rx_send_bufs[sent++]);
            }

            sent = rte_ring_enqueue_burst(params->rx_recv_ring,
                                          (void *)rx_recv_bufs, nb_rx_recv, NULL);
            if (unlikely(sent < nb_rx_recv))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Rx pkt loss due to full rx_recv_ring\n", __func__);
                while (sent < nb_rx_recv)
                    rte_pktmbuf_free(rx_recv_bufs[sent++]);
            }
        }
    }

    printf("\nCore %u exiting rx task.\n", rte_lcore_id());
    return 0;
}
