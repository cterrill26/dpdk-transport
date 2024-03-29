#ifndef DPDK_TRANSPORT_H
#define DPDK_TRANSPORT_H

#ifdef __cplusplus
extern "C"{
#endif 

#include <inttypes.h>

#define MAX_MSG_SIZE 100000
#define F_SINGLE_SEND 0x1
#define F_SINGLE_RECV 0x2

struct msg_info
{
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t src_mac;
	uint64_t dst_mac;
	uint32_t length;
	uint16_t portid;
};

int init_dpdk(int argc, char *argv[], unsigned int flags);
int terminate_dpdk(void);
int send_dpdk(const void *buffer, const struct msg_info *info);
uint32_t recv_dpdk(void *buffer, struct msg_info *info, unsigned int *available);
uint64_t port_to_mac(uint16_t portid);
uint32_t string_to_ip(const char *s);
uint64_t string_to_mac(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* DPDK_TRANSPORT_H */