#ifndef DPDK_TRANSPORT_H
#define DPDK_TRANSPORT_H

#define MAX_MSG_SiZE 100000

struct addrinfo
{
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t src_mac;
	uint64_t dst_mac;
};

void run(uint16_t portid);
void terminate(uint16_t portid);
void send(uint16_t portid, const void *buffer, uint32_t length, const struct addrinfo *info);
uint32_t read(uint16_t portid, void *buffer, struct addrinfo *info);
uint64_t port_to_mac(uint16_t portid);
uint32_t string_to_ip(char *s);
uint64_t string_to_mac(char *s);

#endif /* DPDK_TRANSPORT_H */