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
#define NUM_MBUFS ((64 * 1024) - 1)
#define SCHED_TX_RING_SZ 65536
#define SCHED_RX_SEND_RING_SZ 65536
#define SCHED_RX_RECV_RING_SZ 65536
#define SCHED_SEND_RING_SZ 65536
#define SCHED_RECV_RING_SZ 16384
#define MBUF_CACHE_SIZE 128
#define RECV_RECORD_POOL_SIZE 8192-1
#define RECV_RECORD_CACHE_SIZE 512

struct lcore_params *params;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

static int port_init(uint16_t portid);


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

    /* Creates a new mempool in memory to hold the mbufs. */
    params->mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (params->mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    rte_atomic16_init(&params->outstanding_sends);

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

    if (params->recv_record_pool == NULL){
        rte_exit(EXIT_FAILURE, "Error: failed to create recv_record_pool\n");
    }

    /* initialize all rings */
    params->recv_ring = rte_ring_create("Recv_ring", SCHED_RECV_RING_SZ,
                                rte_socket_id(), RING_F_SP_ENQ);
    if (params->recv_ring == NULL){
        rte_exit(EXIT_FAILURE, "Error: failed to create recv_ring\n");
    }

    params->send_ring = rte_ring_create("Send_ring", SCHED_SEND_RING_SZ,
                                rte_socket_id(), RING_F_SC_DEQ);
    if (params->send_ring == NULL){
        rte_exit(EXIT_FAILURE, "Error: failed to create send_ring\n");
    }

    params->rx_recv_ring = rte_ring_create("Rx_recv_ring", SCHED_RX_RECV_RING_SZ,
                                   rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    if (params->rx_recv_ring == NULL){
        rte_exit(EXIT_FAILURE, "Error: failed to create rx_recv_ring\n");
    }

    params->rx_send_ring = rte_ring_create("Rx_send_ring", SCHED_RX_SEND_RING_SZ,
                                   rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
    if (params->rx_send_ring == NULL){
        rte_exit(EXIT_FAILURE, "Error: failed to create rx_send_ring\n");
    }

    params->tx_ring = rte_ring_create("Tx_ring", SCHED_TX_RING_SZ,
                              rte_socket_id(), RING_F_SC_DEQ);
    if (params->tx_ring == NULL){
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

    rte_mempool_free(params->mbuf_pool);
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
    if (length > MAX_MSG_SIZE)
        return -1;
    
    while (1){
        //atomically increment outstanding sends, ensuring it does not exceed the max amount
        int16_t sends = rte_atomic16_read(&params->outstanding_sends);
        if (sends >= MAX_OUTSTANDING_SENDS)
            return -1;

        if (rte_atomic16_cmpset((volatile uint16_t *) &params->outstanding_sends.cnt, sends, sends + 1) != 0)
            break;
    }

    struct msg_send_record *send_record = rte_malloc("msg_send_record", sizeof(struct msg_send_record), 0);
    send_record->msg = rte_malloc("msg_send_record_msg", length, 0);

    rte_memcpy(&send_record->info, info, sizeof(struct msg_info));
    rte_memcpy(send_record->msg, buffer, length);

    if (rte_ring_enqueue(params->send_ring, send_record) != 0)
    {
        rte_free(send_record->msg);
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
    for(uint8_t pktid = 0; pktid < total_pkts; pktid++){
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
                                        rte_eth_dev_socket_id(portid), NULL, params->mbuf_pool);
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