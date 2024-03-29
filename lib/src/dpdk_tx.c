#include <rte_ethdev.h>
#include "dpdk_tx.h"
#include "dpdk_common.h"
#include "dpdk_transport.h"

struct output_buffer
{
    unsigned count;
    struct rte_mbuf *mbufs[BURST_SIZE_TX];
};

static inline void flush_one_port(struct output_buffer *outbuf, uint16_t portid);
static inline void flush_all_ports(struct output_buffer *tx_buffers);
static inline uint16_t process_ring(struct rte_ring *ring, struct output_buffer *tx_buffers);

static inline void
flush_one_port(struct output_buffer *outbuf, uint16_t portid)
{
    unsigned int nb_tx = rte_eth_tx_burst(portid, 0,
                                          outbuf->mbufs, outbuf->count);

    if (unlikely(nb_tx < outbuf->count))
    {
        do
        {
            rte_pktmbuf_free(outbuf->mbufs[nb_tx]);
        } while (++nb_tx < outbuf->count);
    }
    outbuf->count = 0;
}

static inline void
flush_all_ports(struct output_buffer *tx_buffers)
{
    uint16_t portid;

    RTE_ETH_FOREACH_DEV(portid)
    {
        if (tx_buffers[portid].count == 0)
            continue;

        flush_one_port(&tx_buffers[portid], portid);
    }
}

static inline uint16_t process_ring(struct rte_ring *ring, struct output_buffer *tx_buffers){
    struct rte_mbuf *bufs[BURST_SIZE_TX];
    const uint16_t nb_rx = rte_ring_dequeue_burst(ring,
                                                    (void *)bufs, BURST_SIZE_TX, NULL);

    if (unlikely(nb_rx == 0))
        return 0;

    /* for traffic we receive, queue it up for transmit */
    uint16_t i;
    rte_prefetch_non_temporal((void *)bufs[0]);
    rte_prefetch_non_temporal((void *)bufs[1]);
    rte_prefetch_non_temporal((void *)bufs[2]);
    for (i = 0; i < nb_rx; i++)
    {
        rte_prefetch_non_temporal((void *)bufs[i + 3]);

        uint16_t portid = bufs[i]->port;
        struct output_buffer *outbuf = &tx_buffers[portid];
        outbuf->mbufs[outbuf->count++] = bufs[i];
        // printf("Sending pkt\n");
        // DumpHex(rte_pktmbuf_mtod(bufs[i], char *), bufs[i]->pkt_len);

        if (outbuf->count == BURST_SIZE_TX)
            flush_one_port(outbuf, portid);
    }

    return nb_rx;
}

int lcore_tx(struct lcore_params *params)
{
    static struct output_buffer tx_buffers[RTE_MAX_ETHPORTS];
    const int socket_id = rte_socket_id();
    uint16_t portid;

    RTE_ETH_FOREACH_DEV(portid)
    {
        if (rte_eth_dev_socket_id(portid) > 0 &&
            rte_eth_dev_socket_id(portid) != socket_id)
            printf("WARNING, port %u is on remote NUMA node to "
                   "TX thread.\n\tPerformance will not "
                   "be optimal.\n",
                   portid);
    }

    printf("\nCore %u doing packet TX.\n", rte_lcore_id());

    while (!params->quit_signal_tx)
    {
        uint16_t nb_send_tx = process_ring(params->send_tx_ring, tx_buffers);
        uint16_t nb_recv_tx = process_ring(params->recv_tx_ring, tx_buffers);
        /* if we get no traffic, flush anything we have */
        if (unlikely(nb_send_tx == 0 && nb_recv_tx == 0))
            flush_all_ports(tx_buffers);
    }

    printf("\nCore %u exiting tx task.\n", rte_lcore_id());
    return 0;
}