#ifndef DPDK_TRANSPORT_H
#define DPDK_TRANSPORT_H

#define MAX_MSG_SIZE 100000
#define MAX_OUTSTANDING_SENDS 2047
#define MAX_OUTSTANDING_RECVS 2047
#define MAX_COMPLETED_RECVS 2047

#include <inttypes.h>

struct msg_info
{
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t src_mac;
	uint64_t dst_mac;
	uint32_t length;
	uint16_t portid;
};

int init(int argc, char *argv[]);
int terminate(void);
int send_dpdk(const void *buffer, const struct msg_info *info);
uint32_t recv_dpdk(void *buffer, struct msg_info *info);
uint64_t port_to_mac(uint16_t portid);
uint32_t string_to_ip(char *s);
uint64_t string_to_mac(char *s);

#endif /* DPDK_TRANSPORT_H */