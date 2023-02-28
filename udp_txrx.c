#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <time.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define NUM_TX 3001
#define TX_DELAY_MS 10

#define UDP_ECHO_PORT 6666
#define UDP_TXRX_PORT 6000

#define IP_DEFTTL 64 /* from RFC 1340. */
#define IP_VERSION 0x40
#define IP_HDRLEN 0x05 /* default IP header length == five 32-bits words. */
#define IP_VHL_DEF (IP_VERSION | IP_HDRLEN)

void DumpHex(const void *, size_t);
uint32_t string_to_ip(char *);
uint64_t string_to_mac(char *);
int get_rtt_or_drop(struct rte_mbuf *m, uint64_t rx_ns, int64_t *rtt_ns);
int tx_packets(void *arg);
void rx_packets(void);
void exit_stats(int);
uint64_t gettime_ns(void);

uint64_t ECHO_MAC;
uint32_t IP_TXRX_ADDR, IP_ECHO_ADDR;
struct rte_mempool *mbuf_pool;
uint64_t rx_packet_count = 0;
uint64_t tx_packet_count = 0;
int64_t delays[NUM_TX];

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
    },
};

uint64_t gettime_ns(void){
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_nsec + now.tv_sec * 1e9;
}

void DumpHex(const void *data, size_t size)
{
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i)
    {
        printf("%02X ", ((const unsigned char *)data)[i]);
        if (((const unsigned char *)data)[i] >= ' ' && ((const unsigned char *)data)[i] <= '~')
        {
            ascii[i % 16] = ((const unsigned char *)data)[i];
        }
        else
        {
            ascii[i % 16] = '.';
        }
        if ((i + 1) % 8 == 0 || i + 1 == size)
        {
            printf(" ");
            if ((i + 1) % 16 == 0)
            {
                printf("|  %s \n", ascii);
            }
            else if (i + 1 == size)
            {
                ascii[(i + 1) % 16] = '\0';
                if ((i + 1) % 16 <= 8)
                {
                    printf(" ");
                }
                for (j = (i + 1) % 16; j < 16; ++j)
                {
                    printf("   ");
                }
                printf("|  %s \n", ascii);
            }
        }
    }
}

// convert a quad-dot IP string to uint32_t IP address
uint32_t string_to_ip(char *s)
{
    unsigned char a[4];
    int rc = sscanf(s, "%hhd.%hhd.%hhd.%hhd", a + 0, a + 1, a + 2, a + 3);
    if (rc != 4)
    {
        fprintf(stderr, "bad source IP address format. Use like: -s 198.19.111.179\n");
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
        fprintf(stderr, "bad MAC address format. Use like: -m 0a:38:ca:f6:f3:20\n");
        exit(1);
    }

    return (uint64_t)(a[0]) << 40 |
           (uint64_t)(a[1]) << 32 |
           (uint64_t)(a[2]) << 24 |
           (uint64_t)(a[3]) << 16 |
           (uint64_t)(a[4]) << 8 |
           (uint64_t)(a[5]);
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint16_t port)
{
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0)
    {
        printf("Error during getting device (port %u) info: %s\n",
               port, strerror(-retval));
        return retval;
    }

    if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        port_conf.txmode.offloads |=
            DEV_TX_OFFLOAD_MBUF_FAST_FREE;

    /* Configure the Ethernet device. */
    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    /* Allocate and set up 1 RX queue per Ethernet port. */
    for (q = 0; q < rx_rings; q++)
    {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    /* Allocate and set up 1 TX queue per Ethernet port. */
    for (q = 0; q < tx_rings; q++)
    {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port), &txconf);
        if (retval < 0)
            return retval;
    }

    /* Start the Ethernet port. */
    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    /* Display the port MAC address. */
    struct rte_ether_addr addr;
    rte_eth_macaddr_get(port, &addr);
    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           port,
           addr.addr_bytes[0], addr.addr_bytes[1],
           addr.addr_bytes[2], addr.addr_bytes[3],
           addr.addr_bytes[4], addr.addr_bytes[5]);

    /* Enable RX in promiscuous mode for the Ethernet device. */
    retval = rte_eth_promiscuous_enable(port);

    return 0;
}

int get_rtt_or_drop(struct rte_mbuf *m, uint64_t rx_ns, int64_t *rtt_ns)
{
    struct rte_udp_hdr *udp_hdr;

    udp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *,
                                      sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    if (udp_hdr->dst_port != rte_cpu_to_be_16(UDP_TXRX_PORT))
        return 1;

    uint64_t tx_ns = *rte_pktmbuf_mtod_offset(m, uint64_t *,
                                                 sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));

    *rtt_ns = rx_ns - tx_ns;

    return 0;
}

// setup packet UDP, IP, and ETH headers
static void setup_headers(struct rte_ether_hdr *eth_hdr,
                          struct rte_ipv4_hdr *ip_hdr,
                          struct rte_udp_hdr *udp_hdr,
                          uint16_t udp_data_len,
                          uint16_t port)
{
    uint16_t *ptr16;
    uint32_t ip_cksum;
    uint16_t pkt_len;

    // Initialize UDP header.
    pkt_len = (uint16_t)(udp_data_len + sizeof(struct rte_udp_hdr));
    udp_hdr->src_port = rte_cpu_to_be_16(UDP_TXRX_PORT);
    udp_hdr->dst_port = rte_cpu_to_be_16(UDP_ECHO_PORT);
    udp_hdr->dgram_len = rte_cpu_to_be_16(pkt_len);
    udp_hdr->dgram_cksum = 0; /* No UDP checksum. */

    // Initialize IP header.
    pkt_len = (uint16_t)(pkt_len + sizeof(struct rte_ipv4_hdr));
    ip_hdr->version_ihl = IP_VHL_DEF;
    ip_hdr->type_of_service = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = IP_DEFTTL;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->packet_id = 0;
    ip_hdr->total_length = rte_cpu_to_be_16(pkt_len);
    ip_hdr->src_addr = rte_cpu_to_be_32(IP_TXRX_ADDR);
    ip_hdr->dst_addr = rte_cpu_to_be_32(IP_ECHO_ADDR);

    // Compute IP header checksum.
    ptr16 = (unaligned_uint16_t *)ip_hdr;
    ip_cksum = 0;
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
    ip_hdr->hdr_checksum = (uint16_t)ip_cksum;

    // Set ethernet header MAC
    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } echo_eth_addr;
    echo_eth_addr.as_int = rte_cpu_to_be_64(ECHO_MAC);
    rte_eth_macaddr_get(port, &eth_hdr->s_addr);
    rte_ether_addr_copy(&echo_eth_addr.as_addr, &eth_hdr->d_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
}

// actually send the packet
static void send_packet(uint16_t port)
{
    struct rte_mbuf *pkt;
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct rte_mbuf *pkts_burst[1];

    pkt = rte_mbuf_raw_alloc(mbuf_pool);
    if (pkt == NULL)
    {
        printf("trouble at rte_mbuf_raw_alloc\n");
        return;
    }
    rte_pktmbuf_reset_headroom(pkt);

    uint16_t udp_data_len = sizeof(uint64_t);

    // copy header to packet in mbuf
    eth_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ether_hdr *, 0);
    ip_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *, sizeof(struct rte_ether_hdr));
    udp_hdr = rte_pktmbuf_mtod_offset(pkt, struct rte_udp_hdr *,
                                      sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr));

    setup_headers(eth_hdr, ip_hdr, udp_hdr, udp_data_len, port);

    // Add some pkt fields
    pkt->data_len = sizeof(struct rte_ether_hdr) +
                    sizeof(struct rte_ipv4_hdr) +
                    sizeof(struct rte_udp_hdr) +
                    udp_data_len;
    pkt->nb_segs = 1;
    pkt->pkt_len = pkt->data_len;
    pkt->ol_flags = 0;

    // Add timestamp
    uint64_t *udp_data = rte_pktmbuf_mtod_offset(pkt, uint64_t *,
                                                 sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr));

    *udp_data = gettime_ns();

    // Actually send the packet
    pkts_burst[0] = pkt;
    const uint16_t nb_tx = rte_eth_tx_burst(port, 0, pkts_burst, 1);
    tx_packet_count += nb_tx; // TODO make atomic?
    rte_mbuf_raw_free(pkt);
}
int tx_packets(__attribute__((unused)) void *arg)
{
    uint16_t port;

    printf("\nCore %u sending %u packets.\n",
           rte_lcore_id(), NUM_TX);

    int i;
    for (i = 0; i < NUM_TX; i++)
    {
        RTE_ETH_FOREACH_DEV(port)
        {
            send_packet(port);
        }
        rte_delay_ms(TX_DELAY_MS);
    }

    printf("\nCore %u done.\n",
           rte_lcore_id());

    return 0;
}

void rx_packets(void)
{
    uint16_t port;

    printf("\nCore %u receiving packets. [Ctrl+C to quit]\n",
           rte_lcore_id());

    /* Run until the application is quit or killed. */
    for (;;)
    {
        RTE_ETH_FOREACH_DEV(port)
        {

            struct rte_mbuf *bufs[BURST_SIZE];
            const uint16_t nb_rx = rte_eth_rx_burst(port, 0,
                                                    bufs, BURST_SIZE);
            if (unlikely(nb_rx == 0))
                continue;

            uint64_t rx_ns = gettime_ns();

            uint16_t buf;
            for (buf = 0; buf < nb_rx; buf++)
            {
                //DumpHex(rte_pktmbuf_mtod(bufs[buf], char *), bufs[buf]->pkt_len);
                int64_t rtt_ns = 0;
                if (!get_rtt_or_drop(bufs[buf], rx_ns, &rtt_ns)) {
                    //printf("%ld ns\n", rtt);
                    if (rx_packet_count < NUM_TX)
                        delays[rx_packet_count] = rtt_ns;
                    rx_packet_count += 1;
                }

                rte_pktmbuf_free(bufs[buf]);
            }
        }
    }
}

void exit_stats(int sig)
{
    printf("Caught signal %d\n", sig);
    printf("Total sent packets: %lu\n", tx_packet_count);
    printf("Total received packets: %lu\n", rx_packet_count);
    printf("Printing delays in ns:\n");
    int printlen = (NUM_TX < rx_packet_count) ? NUM_TX : rx_packet_count;
    for (int i = 0; i < printlen;i++)
        printf("%ld\n", delays[i]);
    exit(0);
}

int main(int argc, char *argv[])
{
    int c;
    int mac_flag = 0, ip_txrx_flag = 0, ip_echo_flag = 0;
    unsigned nb_ports;
    uint16_t portid;

    /* Initialize the Environment Abstraction Layer (EAL). */
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    while ((c = getopt(argc, argv, "m:s:d:h")) != -1)
        switch (c)
        {
        case 'm':
            // note, not quite sure why last two bytes are zero, but that is how DPDK likes it
            ECHO_MAC = 0ULL;
            ECHO_MAC = string_to_mac(optarg) << 16;
            mac_flag = 1;
            break;
        case 's':
            IP_TXRX_ADDR = string_to_ip(optarg);
            ip_txrx_flag = 1;
            break;
        case 'd':
            IP_ECHO_ADDR = string_to_ip(optarg);
            ip_echo_flag = 1;
            break;
        case 'h':
            printf("usage -- -m [dst MAC] -s [src IP] -d [dst IP]\n");
            exit(0);
            break;
        }

    if (mac_flag == 0)
    {
        fprintf(stderr, "missing -m for destination MAC adress\n");
        exit(1);
    }
    if (ip_txrx_flag == 0)
    {
        fprintf(stderr, "missing -s for IP source adress\n");
        exit(1);
    }
    if (ip_echo_flag == 0)
    {
        fprintf(stderr, "missing -d for IP destination adress\n");
        exit(1);
    }

    nb_ports = rte_eth_dev_count_avail();
    printf("rte_eth_dev_count_avail()=%d\n", nb_ports);

    /* Creates a new mempool in memory to hold the mbufs. */
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
                                        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    /* Initialize all ports. */
    RTE_ETH_FOREACH_DEV(portid)
    {
        if (port_init(portid) != 0)
            rte_exit(EXIT_FAILURE, "Cannot init port %" PRIu16 "\n", portid);
    }
    // call tx_packets() on every slave lcore
    unsigned lcore_id;
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        rte_eal_remote_launch(tx_packets, NULL, lcore_id);
    }

    signal(SIGINT, exit_stats);

    // call rx_packets() on the master core
    rx_packets();

    return 0;
}
