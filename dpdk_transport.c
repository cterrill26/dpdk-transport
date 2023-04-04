#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_debug.h>
#include "dpdk_transport.h"
#include "dpdk_tx.h"
#include "dpdk_rx.h"
#include "dpdk_recv.h"
#include "dpdk_send.h"
#include "dpdk_common.h"

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NB_SEND_MBUFS ((64 * 1024) - 1)
#define NB_RECV_MBUFS ((64 * 1024) - 1)
#define SCHED_TX_RING_SZ 65536
#define SCHED_RX_SEND_RING_SZ 65536
#define SCHED_RX_RECV_RING_SZ 65536
#define SCHED_SEND_RING_SZ 65536
#define SCHED_RECV_RING_SZ 16384
#define MBUF_CACHE_SIZE 128
#define RECV_RECORD_POOL_SIZE ((8 * 1024) - 1)
#define RECV_RECORD_CACHE_SIZE 128

struct lcore_params *params;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

static int port_init(uint16_t portid);
static inline void set_template_hdr(char *template_hdr, const struct msg_info *info, uint32_t msgid);

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

    params = rte_zmalloc("params", sizeof(struct lcore_params), 0);


    /* Create mempools in memory to hold the mbufs. */
    params->send_mbuf_pool = rte_pktmbuf_pool_create("SEND_MBUF_POOL", NB_SEND_MBUFS * nb_ports,
                                                MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (params->send_mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create send mbuf pool\n");

    params->recv_mbuf_pool = rte_pktmbuf_pool_create("RECV_MBUF_POOL", NB_RECV_MBUFS * nb_ports,
                                                MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (params->recv_mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create recv mbuf pool\n");


    rte_atomic16_init(&params->outstanding_sends);
    rte_atomic32_init(&params->next_msgid);

    /* initialize all ports */
    uint16_t portid;
    RTE_ETH_FOREACH_DEV(portid)
    {
        printf("Initializing port %u... done\n", portid);

        if (port_init(portid) != 0)
            rte_exit(EXIT_FAILURE, "Cannot initialize port %u\n",
                     portid);
    }

    params->recv_record_pool = rte_mempool_create("Recv record mempool", RECV_RECORD_POOL_SIZE, sizeof(struct msg_recv_record),
                                                  RECV_RECORD_CACHE_SIZE, 0, NULL, NULL, NULL, NULL, rte_socket_id(), MEMPOOL_F_SC_GET);

    if (params->recv_record_pool == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create recv_record_pool\n");
    }

    /* initialize all rings */
    params->recv_ring = rte_ring_create("Recv_ring", SCHED_RECV_RING_SZ,
                                        rte_socket_id(), RING_F_SP_ENQ);
    if (params->recv_ring == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create recv_ring\n");
    }

    params->send_ring = rte_ring_create("Send_ring", SCHED_SEND_RING_SZ,
                                        rte_socket_id(), RING_F_SC_DEQ);
    if (params->send_ring == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create send_ring\n");
    }

    params->rx_recv_ring = rte_ring_create("Rx_recv_ring", SCHED_RX_RECV_RING_SZ,
                                           rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    if (params->rx_recv_ring == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create rx_recv_ring\n");
    }

    params->rx_send_ring = rte_ring_create("Rx_send_ring", SCHED_RX_SEND_RING_SZ,
                                           rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    if (params->rx_send_ring == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create rx_send_ring\n");
    }

    params->tx_ring = rte_ring_create("Tx_ring", SCHED_TX_RING_SZ,
                                      rte_socket_id(), RING_F_SC_DEQ);
    if (params->tx_ring == NULL)
    {
        rte_exit(EXIT_FAILURE, "Error: failed to create tx_ring\n");
    }

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
    rte_eal_remote_launch((lcore_function_t *)lcore_rx, params, rx_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_tx, params, tx_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_send, params, send_core_id);
    rte_eal_remote_launch((lcore_function_t *)lcore_recv, params, recv_core_id);

    return ret;
}

int terminate(void)
{
    params->quit_signal_rx = 1;
    params->quit_signal_tx = 1;
    params->quit_signal_recv = 1;
    params->quit_signal_send = 1;

    unsigned int lcore_id;
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        if (rte_eal_wait_lcore(lcore_id) < 0)
            return -1;
    }

    rte_mempool_free(params->send_mbuf_pool);
    rte_mempool_free(params->recv_mbuf_pool);
    rte_mempool_free(params->recv_record_pool);
    rte_ring_free(params->recv_ring);
    rte_ring_free(params->send_ring);
    rte_ring_free(params->rx_send_ring);
    rte_ring_free(params->rx_recv_ring);
    rte_ring_free(params->tx_ring);

    rte_free(params);

    /* clean up the EAL */
    rte_eal_cleanup();

    return 0;
}

int send_dpdk(const void *buffer, const struct msg_info *info)
{
    uint32_t length = info->length;
    if (unlikely(length > MAX_MSG_SIZE))
        return -1;

    while (1)
    {
        // atomically increment outstanding sends, ensuring it does not exceed the max amount
        int16_t sends = rte_atomic16_read(&params->outstanding_sends);
        if (unlikely(sends >= MAX_ACTIVE_SENDS))
            return -1;

        if (likely(rte_atomic16_cmpset((volatile uint16_t *)&params->outstanding_sends.cnt, sends, sends + 1) != 0))
            break;
    }

    struct msg_send_record *send_record = rte_zmalloc("msg_send_record", sizeof(struct msg_send_record), 0);
    rte_memcpy(&send_record->info, info, sizeof(struct msg_info));

    uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(info->length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;

    if (unlikely(rte_pktmbuf_alloc_bulk(params->send_mbuf_pool, send_record->pkts, total_pkts) < 0))
    {
        rte_atomic16_dec(&params->outstanding_sends);
        rte_free(send_record);
        return -1;
    }

    uint32_t msgid = (uint32_t)(rte_atomic32_add_return(&params->next_msgid, 1));
    send_record->msgid = msgid;

    // set pkt headers and copy over msg contents
    char template_hdr[TOTAL_HDR_SIZE];
    set_template_hdr(template_hdr, info, msgid);

    const char *buf = (const char *)buffer;
    for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
    {
        struct rte_mbuf *pkt = send_record->pkts[pktid];
        uint16_t msgdata_len = RTE_MIN(MAX_PKT_MSGDATA_LEN, info->length - pktid * ((uint32_t)MAX_PKT_MSGDATA_LEN));

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

        // copy over packet's msg data
        rte_memcpy(rte_pktmbuf_mtod_offset(pkt, void *, TOTAL_HDR_SIZE),
                   (const void *)&(buf[pktid * MAX_PKT_MSGDATA_LEN]),
                   msgdata_len);
    }

    if (unlikely(rte_ring_enqueue(params->send_ring, send_record) != 0))
    {
        rte_atomic16_dec(&params->outstanding_sends);
        for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
        {
            rte_pktmbuf_free(send_record->pkts[pktid]);
        }
        rte_free(send_record);
        return -1;
    }

    return 0;
}

uint32_t recv_dpdk(void *buffer, struct msg_info *info, unsigned int *available)
{
    struct msg_recv_record *recv_record;
    if (rte_ring_dequeue_burst(params->recv_ring, (void *)&recv_record, 1, available) == 0)
    {
        info->length = 0;
        return 0;
    }

    rte_memcpy(info, &recv_record->info, sizeof(struct msg_info));

    uint32_t length = recv_record->info.length;
    uint8_t total_pkts = RTE_ALIGN_MUL_CEIL(length, MAX_PKT_MSGDATA_LEN) / MAX_PKT_MSGDATA_LEN;
    char *buf = (char *)buffer;

    rte_prefetch_non_temporal((void *)recv_record->pkts[0]);
    rte_prefetch_non_temporal((void *)recv_record->pkts[1]);
    rte_prefetch_non_temporal((void *)recv_record->pkts[2]);
    for (uint8_t pktid = 0; pktid < total_pkts; pktid++)
    {
        rte_prefetch_non_temporal((void *)recv_record->pkts[pktid + 3]);

        struct rte_mbuf *pkt = recv_record->pkts[pktid];
        rte_memcpy((void *)&(buf[pktid * MAX_PKT_MSGDATA_LEN]),
                   rte_pktmbuf_mtod_offset(pkt, void *, TOTAL_HDR_SIZE),
                   pkt->pkt_len - TOTAL_HDR_SIZE);

        rte_pktmbuf_free(pkt);
    }

    rte_mempool_put(params->recv_record_pool, recv_record);
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
                                        rte_eth_dev_socket_id(portid), NULL, params->recv_mbuf_pool);
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

    mac_addr.as_int = 0LL;
    rte_eth_macaddr_get(portid, &mac_addr.as_addr);

    return rte_be_to_cpu_64(mac_addr.as_int);
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
        fprintf(stderr, "bad MAC address format: %s\n", s);
        exit(1);
    }

    return (uint64_t)(a[0]) << 40 |
           (uint64_t)(a[1]) << 32 |
           (uint64_t)(a[2]) << 24 |
           (uint64_t)(a[3]) << 16 |
           (uint64_t)(a[4]) << 8 |
           (uint64_t)(a[5]);
}

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
    dpdk_hdr->pktid = 0; // placeholder
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

    // Initialize ethernet header.
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