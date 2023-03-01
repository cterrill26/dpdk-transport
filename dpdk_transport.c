#include "dpdk_transport.h"
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>
#include <rte_debug.h>

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

#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

struct msgbuf
{
    struct msginfo *info;
    char *msg;
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
static int lcore_send(void *arg);
static int lcore_recv(void *arg);

struct rte_ring *recv_ring;
struct rte_ring *send_ring;
struct rte_ring *tx_ring;
struct rte_ring *rx_recv_ring;
struct rte_ring *rx_send_ring;
struct rte_mempool *mbuf_pool;
volatile int quit_signal_tx;
volatile int quit_signal_rx;


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

	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rx_core_id < 0) {
			rx_core_id = lcore_id;
			printf("RX on core %d\n", lcore_id);
		}
		else if (tx_core_id < 0) {
			tx_core_id = lcore_id;
			printf("TX on core %d\n", lcore_id);
		}
		else if (send_core_id < 0) {
			send_core_id = lcore_id;
			printf("Send on core %d\n", lcore_id);
		}
		else if (recv_core_id < 0) {
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

int terminate(void){
    quit_signal_rx = 1;

    unsigned int lcore_id;
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
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

    struct msgbuf *buf = rte_malloc("msgbuf", sizeof(struct msgbuf), 0);
    buf->info = rte_malloc("msgbuf_info", sizeof(struct msginfo), 0);
    buf->msg = rte_malloc("msgbuf_msg", length, 0);

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
    struct msgbuf *buf;
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

static inline int is_control_pkt(__attribute__((unused)) const struct rte_mbuf *buf){
    return 0;
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
    quit_signal_tx = 1;
    return 0;
}

static int lcore_send(__attribute__((unused)) void *arg){
    return 0;
}
static int lcore_recv(__attribute__((unused)) void *arg){
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