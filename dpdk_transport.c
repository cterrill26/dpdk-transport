#include "dpdk_transport.h"
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>
#include <rte_debug.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_common.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS ((64 * 1024) - 1)
#define SCHED_TX_RING_SZ 65536
#define SCHED_RX_SEND_RING_SZ 65536
#define SCHED_RX_RECV_RING_SZ 65536
#define SCHED_SEND_RING_SZ 65536
#define SCHED_RECV_RING_SZ 65536
#define MBUF_CACHE_SIZE 128
#define BURST_SIZE_RX 64
#define BURST_SIZE_TX 32
#define SEND_TABLE_ENTRIES 1024
#define RECV_TABLE_ENTRIES 1024

#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

#define IPPROTO_DPDK_TRANSPORT 200
#define DPDK_TRANSPORT_MSGDATA 0
#define DPDK_TRANSPORT_COMPLETE 1
#define DPDK_TRANSPORT_RESEND 2

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

struct msg_recv_record
{
    struct msg_buf *buf;
    uint64_t pkts_received_mask[4]; // mask of received pktids
    uint8_t nb_pkts_received;
};

struct msg_key
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t msgid;
};

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

struct output_buffer
{
    unsigned count;
    struct rte_mbuf *mbufs[BURST_SIZE_TX];
};

static int port_init(uint16_t portid);
static inline void flush_one_port(struct output_buffer *outbuf, uint16_t portid);
static inline void flush_all_ports(struct output_buffer *tx_buffers);
static int lcore_tx(void *arg);
static inline int is_control_pkt(const struct rte_mbuf *buf);
static int lcore_rx(void *arg);
static inline void set_ipv4_cksum(struct rte_ipv4_hdr *hdr);
static inline void set_template_hdr(char *template_hdr, const struct msg_buf *buf, uint32_t msgid);
static inline void send_msg(struct msg_buf *buf, struct rte_hash *hashtbl, uint32_t msg_id);
static int lcore_send(void *arg);
static inline void recv_msg(struct msg_buf *buf, struct rte_hash *hashtbl, struct msg_key *key);
static inline void recv_pkt(struct rte_mbuf *pkt, struct rte_hash *hashtbl);
static int lcore_recv(void *arg);

struct rte_ring *recv_ring;
struct rte_ring *send_ring;
struct rte_ring *tx_ring;
struct rte_ring *rx_recv_ring;
struct rte_ring *rx_send_ring;
struct rte_mempool *mbuf_pool;
volatile int quit_signal_tx;
volatile int quit_signal_rx;
volatile int quit_signal_send;
volatile int quit_signal_recv;

int init(int argc, char *argv[])
{
    /* Initialize the Environment Abstraction Layer (EAL). */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    unsigned nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "Error: no ethernet ports detected\n");

    printf("rte_eth_dev_count_avail()=%d\n", nb_ports);

    /* Creates a new mempool in memory to hold the mbufs. */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    /* initialize all ports */
    uint16_t portid;
    RTE_ETH_FOREACH_DEV(portid)
    {
        printf("Initializing port %u... done\n", portid);

        if (port_init(portid) != 0)
            rte_exit(EXIT_FAILURE, "Cannot initialize port %u\n",
                     portid);
    }

    /* initialize all rings */
    recv_ring = rte_ring_create("Recv_ring", SCHED_RECV_RING_SZ,
                                rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    send_ring = rte_ring_create("Send_ring", SCHED_SEND_RING_SZ,
                                rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    rx_recv_ring = rte_ring_create("Rx_recv_ring", SCHED_RX_RECV_RING_SZ,
                                   rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    rx_send_ring = rte_ring_create("Rx_send_ring", SCHED_RX_SEND_RING_SZ,
                                   rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    tx_ring = rte_ring_create("Tx_ring", SCHED_TX_RING_SZ,
                              rte_socket_id(), RING_F_SC_DEQ);

    if (rte_lcore_count() < 5)
        rte_exit(EXIT_FAILURE, "Error, This application needs at "
                               "least 5 logical cores to run:\n"
                               "1 lcore for user application\n"
                               "1 lcore for packet RX\n"
                               "1 lcore for packet TX\n"
                               "1 lcore for send processing\n"
                               "1 lcore for recv processing\n");

    /* assign each thread to a core */
    unsigned int lcore_id;
    int rx_core_id = -1;
    int tx_core_id = -1;
    int send_core_id = -1;
    int recv_core_id = -1;

    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        if (rx_core_id < 0)
        {
            rx_core_id = lcore_id;
            printf("RX on core %d\n", lcore_id);
        }
        else if (tx_core_id < 0)
        {
            tx_core_id = lcore_id;
            printf("TX on core %d\n", lcore_id);
        }
        else if (send_core_id < 0)
        {
            send_core_id = lcore_id;
            printf("Send on core %d\n", lcore_id);
        }
        else if (recv_core_id < 0)
        {
            recv_core_id = lcore_id;
            printf("Recv on core %d\n", lcore_id);
        }
        else
            break;
    }

    /* launch threads */
    quit_signal_tx = 0;
    quit_signal_rx = 0;

    rte_eal_remote_launch((lcore_function_t *)lcore_rx, NULL, rx_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_tx, NULL, tx_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_send, NULL, send_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_recv, NULL, recv_core_id);

    return ret;
}

int terminate(void)
{
    quit_signal_rx = 1;
    quit_signal_tx = 1;
    quit_signal_recv = 1;
    quit_signal_send = 1;

    unsigned int lcore_id;
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        if (rte_eal_wait_lcore(lcore_id) < 0)
            return -1;
    }

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}

int send_dpdk(const void *buffer, const struct msginfo *info)
{
    uint32_t length = info->length;

    struct msg_buf *buf = rte_malloc("msg_buf", sizeof(struct msg_buf), 0);
    buf->info = rte_malloc("msg_buf_info", sizeof(struct msginfo), 0);
    buf->msg = rte_malloc("msg_buf_msg", length, 0);

    rte_memcpy(buf->info, info, sizeof(struct msginfo));
    rte_memcpy(buf->msg, buffer, length);

    if (rte_ring_enqueue(send_ring, buf) != 0)
    {
        return -1;
    }

    return 0;
}

uint32_t recv_dpdk(void *buffer, struct msginfo *info)
{
    struct msg_buf *buf;
    if (rte_ring_dequeue(recv_ring, (void *)&buf) != 0)
    {
        info->length = 0;
        return 0;
    }

    uint32_t length = buf->info->length;
    rte_memcpy(buffer, buf->msg, length);
    rte_memcpy(info, buf->info, sizeof(struct msginfo));

    rte_free(buf->info);
    rte_free(buf->msg);
    rte_free(buf);
    return length;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static int port_init(uint16_t portid)
{
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(portid))
        return -1;

    retval = rte_eth_dev_info_get(portid, &dev_info);
    if (retval != 0)
    {
        printf("Error during getting device (port %u) info: %s\n",
               portid, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(portid, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++)
    {
        retval = rte_eth_rx_queue_setup(portid, q, nb_rxd,
                                        rte_eth_dev_socket_id(portid), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++)
    {
        retval = rte_eth_tx_queue_setup(portid, q, nb_txd,
                                        rte_eth_dev_socket_id(portid), &txconf);
        if (retval < 0)
            return retval;
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(portid);
    if (retval < 0)
        return retval;

    /* Display the port MAC address. */
    struct rte_ether_addr addr;
    rte_eth_macaddr_get(portid, &addr);
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           portid,
           addr.addr_bytes[0], addr.addr_bytes[1],
           addr.addr_bytes[2], addr.addr_bytes[3],
           addr.addr_bytes[4], addr.addr_bytes[5]);

    /* Enable RX in promiscuous mode for the Ethernet device. */
    retval = rte_eth_promiscuous_enable(portid);

    return 0;
}

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

static int lcore_tx(__attribute__((unused)) void *arg)
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

    while (!quit_signal_tx)
    {
        struct rte_mbuf *bufs[BURST_SIZE_TX];
        const uint16_t nb_rx = rte_ring_dequeue_burst(tx_ring,
                                                      (void *)bufs, BURST_SIZE_TX, NULL);

        /* if we get no traffic, flush anything we have */
        if (unlikely(nb_rx == 0))
        {
            flush_all_ports(tx_buffers);
            continue;
        }
        rte_ring_enqueue_burst(rx_recv_ring,
                               (void *)bufs, nb_rx, NULL);
        continue;

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

            if (outbuf->count == BURST_SIZE_TX)
                flush_one_port(outbuf, portid);
        }
    }

    printf("\nCore %u exiting tx task.\n", rte_lcore_id());
    return 0;
}

static inline int is_control_pkt(const struct rte_mbuf *buf)
{
    struct dpdk_transport_hdr *hdr;
    hdr = rte_pktmbuf_mtod_offset(buf, struct dpdk_transport_hdr *,
                                  sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    if (hdr->type == DPDK_TRANSPORT_MSGDATA)
        return 0;

    return 1;
}

static int lcore_rx(__attribute__((unused)) void *arg)
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

    while (!quit_signal_rx)
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
            for (i = 0; i < nb_rx; i++)
            {
                if (is_control_pkt(bufs[i]))
                    rx_send_bufs[nb_rx_send++] = bufs[i];
                else
                    rx_recv_bufs[nb_rx_recv++] = bufs[i];
            }

            uint16_t sent;
            sent = rte_ring_enqueue_burst(rx_send_ring,
                                          (void *)rx_send_bufs, nb_rx_send, NULL);
            if (unlikely(sent < nb_rx_send))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Packet loss due to full rx_send_ring\n", __func__);
                while (sent < nb_rx_send)
                    rte_pktmbuf_free(rx_send_bufs[sent++]);
            }

            sent = rte_ring_enqueue_burst(rx_recv_ring,
                                          (void *)rx_recv_bufs, nb_rx_recv, NULL);
            if (unlikely(sent < nb_rx_recv))
            {
                RTE_LOG_DP(DEBUG, RING,
                           "%s:Packet loss due to full rx_recv_ring\n", __func__);
                while (sent < nb_rx_recv)
                    rte_pktmbuf_free(rx_recv_bufs[sent++]);
            }
        }
    }

    printf("\nCore %u exiting rx task.\n", rte_lcore_id());
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

static inline void send_msg(struct msg_buf *buf, struct rte_hash *hashtbl, uint32_t msgid)
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
            if (unlikely(rte_pktmbuf_alloc_bulk(mbuf_pool, bufs, nb_to_send) < 0))
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
            sent = rte_ring_enqueue_burst(tx_ring,
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

static int lcore_send(__attribute__((unused)) void *arg)
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
    while (!quit_signal_send)
    {
        // dequeue and send msg from send_ring
        struct msg_buf *buf;
        if (likely(rte_ring_dequeue(send_ring, (void *)&buf) == 0))
            send_msg(buf, hashtbl, next_msgid++);

        // process received control messages from rx_send_ring
    }

    rte_hash_free(hashtbl);
    printf("\nCore %u exiting send task.\n", rte_lcore_id());
    return 0;
}

static inline void recv_msg(struct msg_buf *buf, struct rte_hash *hashtbl, struct msg_key *key)
{
    // full packet received, pass on to recv_ring
    rte_hash_del_key(hashtbl, key);
    if (rte_ring_enqueue(recv_ring, buf) != 0)
    {
        RTE_LOG_DP(DEBUG, HASH,
                   "%s:Msg loss due to failed recv_ring enqueue\n", __func__);
        rte_free(buf->info);
        rte_free(buf->msg);
        rte_free(buf);
    }

    // TODO send completed msg
}

static inline void recv_pkt(struct rte_mbuf *pkt, struct rte_hash *hashtbl)
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
        buf->msg = rte_malloc("recv msgdata", dpdk_hdr->msg_len, 0);

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
        buf->info->src_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
        buf->info->portid = pkt->port;
        buf->info->length = rte_be_to_cpu_32(dpdk_hdr->msg_len);

        uint8_t pktid = dpdk_hdr->pktid;
        // copy over packet msg data
        rte_memcpy((void *)&(buf->msg[pktid * max_pkt_msgdata_len]),
                   rte_pktmbuf_mtod_offset(pkt, void *, total_hdr_size),
                   pkt->pkt_len);

    printf("received packet\n");
        if (rte_be_to_cpu_32(dpdk_hdr->msg_len) <= max_pkt_msgdata_len)
        {
            recv_msg(buf, hashtbl, &key);
    printf("received packet\n");
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
        if (likely((recv_record->pkts_received_mask[pktid / 64] & (1 << (pktid % 64))) == 0))
        {
            // packet is not a duplicate
            recv_record->nb_pkts_received++;
            recv_record->pkts_received_mask[pktid / 64] |= (1 << (pktid % 64));

            // copy over packet msg data
            rte_memcpy((void *)&(recv_record->buf->msg[pktid * max_pkt_msgdata_len]),
                       rte_pktmbuf_mtod_offset(pkt, void *, total_hdr_size),
                       pkt->pkt_len);

            uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(recv_record->buf->info->length, max_pkt_msgdata_len) / max_pkt_msgdata_len;
            if (recv_record->nb_pkts_received == total_pkts)
            {
                recv_msg(recv_record->buf, hashtbl, &key);
                rte_free(recv_record);
            }
        }
        rte_pktmbuf_free(pkt);
    }
}

static int lcore_recv(__attribute__((unused)) void *arg)
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

    while (!quit_signal_recv)
    {
        // dequeue pkts from rx_recv_ring
        struct rte_mbuf *pkts[BURST_SIZE_RX];
        unsigned nb_rx = rte_ring_dequeue_burst(rx_recv_ring, (void *)pkts, BURST_SIZE_RX, NULL);
        if (likely(nb_rx > 0))
        {
            for (unsigned i = 0; i < nb_rx; i++)
                recv_pkt(pkts[i], hashtbl);
        }

        // send resend requests for missing packets
    }

    rte_hash_free(hashtbl);
    printf("\nCore %u exiting recv task.\n", rte_lcore_id());
    return 0;
}

// get the uint64_t MAC address for a given portid
uint64_t port_to_mac(uint16_t portid)
{
    if (!rte_eth_dev_is_valid_port(portid))
    {
        fprintf(stderr, "invalid portid: %u\n", portid);
        exit(1);
    }

    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;

    rte_eth_macaddr_get(portid, &mac_addr.as_addr);

    return mac_addr.as_int;
}

// convert a quad-dot IP string to uint32_t IP address
uint32_t string_to_ip(char *s)
{
    unsigned char a[4];
    int rc = sscanf(s, "%hhd.%hhd.%hhd.%hhd", a + 0, a + 1, a + 2, a + 3);
    if (rc != 4)
    {
        fprintf(stderr, "bad IP address format: %s\n", s);
        exit(1);
    }

    return (uint32_t)(a[0]) << 24 |
           (uint32_t)(a[1]) << 16 |
           (uint32_t)(a[2]) << 8 |
           (uint32_t)(a[3]);
}

// convert six colon separated hex bytes string to uint64_t Ethernet MAC address
uint64_t string_to_mac(char *s)
{
    unsigned char a[6];
    int rc = sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                    a + 0, a + 1, a + 2, a + 3, a + 4, a + 5);
    if (rc != 6)
    {
        fprintf(stderr, "bad MAC address format: %sn", s);
        exit(1);
    }

    return (uint64_t)(a[0]) << 40 |
           (uint64_t)(a[1]) << 32 |
           (uint64_t)(a[2]) << 24 |
           (uint64_t)(a[3]) << 16 |
           (uint64_t)(a[4]) << 8 |
           (uint64_t)(a[5]);
}