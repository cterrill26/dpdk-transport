#include "dpdk_transport.h"
#include <rte_ethdev.h>
#include <rte_memcpy.h>
#include <rte_malloc.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define NUM_MBUFS ((64*1024)-1)
#define SCHED_TX_RING_SZ 65536
#define SCHED_RX_SEND_RING_SZ 65536
#define SCHED_RX_RECV_RING_SZ 65536
#define SCHED_SEND_RING_SZ 65536
#define SCHED_RECV_RING_SZ 65536
#define MBUF_CACHE_SIZE 128
#define BURST_SIZE 64
#define BURST_SIZE_TX 32

#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

static int port_init(uint16_t portid);

struct rte_ring *recv_ring;
struct rte_ring *send_ring;
struct rte_mempool *mbuf_pool;

struct msgbuf{
    struct msginfo *info;
    char *msg;
};


int init(int argc, char *argv[]){
    /* Initialize the Environment Abstraction Layer (EAL). */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    unsigned nb_ports = rte_eth_dev_count_avail();
    printf("rte_eth_dev_count_avail()=%d\n", nb_ports);

    /* Creates a new mempool in memory to hold the mbufs. */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");



	recv_ring = rte_ring_create("Recv_ring", SCHED_RECV_RING_SZ,
			rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
	send_ring = rte_ring_create("Send_ring", SCHED_SEND_RING_SZ,
			rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
	// rx_recv_ring = rte_ring_create("Rx_recv_ring", SCHED_RX_RECV_RING_SZ,
	// 		rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
	// rx_send_ring = rte_ring_create("Rx_send_ring", SCHED_RX_SEND_RING_SZ,
	// 		rte_socket_id(), RING_F_SC_DEQ | RING_F_SP_ENQ);
	// tx_ring = rte_ring_create("Tx_ring", SCHED_TX_RING_SZ,
	// 		rte_socket_id(), RING_F_SC_DEQ);

    return ret;
}


int sendmsg(void *buffer, const struct msginfo *info){
    uint32_t length = info->length;

    struct msgbuf *buf = rte_malloc("msgbuf", sizeof(msgbuf), 0);
    buf->info = rte_malloc("msgbuf_info", sizeof(msginfo), 0);
    buf->msg = rte_malloc("msgbuf_msg", length, 0);

    rte_memcpy(buf->info, info, sizeof(msginfo));
    rte_memcpy(buf->msg, buffer, length);

    if (rte_ring_enqueue(&send_ring, buf) != 0) {
        return -1;
    }

    return 0;
}

uint32_t recvmsg(void *buffer, struct msginfo *info){
    struct msgbuf *buf;
    if (rte_ring_dequeue(&recv_ring, &buf) != 0) {
        info->length = 0;
        return 0;
    }

    uint32_t length = buf->info->length;
    rte_memcpy(buffer, buf->msg, length);
    rte_memcpy(info, buf->info, sizeof(msginfo));

    rte_free(buf->info);
    rte_free(buf->msg);
    rte_free(buf);
    return length;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static int port_init(uint16_t port)
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

// get the uint64_t MAC address for a given portid
uint64_t port_to_mac(uint16_t portid)
{
    if (!rte_eth_dev_is_valid_port(portid)){
        fprintf(stderr, "invalid portid: %u\n", portid);
        exit(1);
    }
    
    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;

    mac_addr addr;
    rte_eth_macaddr_get(portid, &addr.as_addr);

    return addr.as_int;
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