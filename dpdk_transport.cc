#include "dpdk_transport.h"
#include <stdint.h>
#include <assert.h>
#include <rte_ethdev.h>

uint32_t MsgInfo::string_to_ip(const std::string &s)
{
    unsigned char a[4];
    int rc = sscanf(s.c_str(), "%hhd.%hhd.%hhd.%hhd", a + 0, a + 1, a + 2, a + 3);
    if (rc != 4)
    {
        fprintf(stderr, "Bad IP address format: %s\n", s);
        exit(1);
    }

    return (uint32_t)(a[0]) << 24 |
           (uint32_t)(a[1]) << 16 |
           (uint32_t)(a[2]) << 8 |
           (uint32_t)(a[3]); 
}

uint64_t MsgInfo::string_to_mac(const std::string &s)
{
    unsigned char a[6];
    int rc = sscanf(s.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                    a + 0, a + 1, a + 2, a + 3, a + 4, a + 5);
    if (rc != 6)
    {
        fprintf(stderr, "Bad MAC address format: %s\n", s);
        exit(1);
    }

    return (uint64_t)(a[0]) << 40 |
           (uint64_t)(a[1]) << 32 |
           (uint64_t)(a[2]) << 24 |
           (uint64_t)(a[3]) << 16 |
           (uint64_t)(a[4]) << 8 |
           (uint64_t)(a[5]);
}

DpdkTransport::DpdkTransport(const std::unordered_map<uint8_t, MsgQueue> &rx_queues) : m_rx_queues(rx_queues)
{
    m_next_msg_id = 0;
    m_is_init = false;
}

DpdkTransport::~DpdkTransport()
{
}

void DpdkTransport::init(uint16_t port)
{
    if (m_is_init){
        fprintf(stderr, "Cannot call init after already initiated\n");
        exit(1);
    }

    m_is_init = true;
}

void DpdkTransport::terminate() {
    if (!m_is_init){
        fprintf(stderr, "Must call init before calling terminate\n");
        exit(1);
    }

    m_is_init = false;
}

void DpdkTransport::send(const std::string &msg, const MsgInfo &msg_info){
    if (!m_is_init){
        fprintf(stderr, "Must call init before calling send\n");
        exit(1);
    }


}

uint64_t DpdkTransport::get_mac()
{
    if (!m_is_init){
        fprintf(stderr, "Must call init before calling get_mac\n");
        exit(1);
    }

    union
    {
        uint64_t as_int;
        struct rte_ether_addr as_addr;
    } mac_addr;

    mac_addr addr;
    rte_eth_macaddr_get(m_port, &addr.as_addr);

    return addr.as_int;
}