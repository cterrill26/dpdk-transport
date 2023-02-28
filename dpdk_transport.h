#ifndef DPDK_TRANSPORT_H
#define DPDK_TRANSPORT_H

#include <stdint.h>

class MsgInfo;

typedef ConcurrentQueue<std::pair<std::string, MsgInfo>> MsgQueue;

	class MsgInfo
{
public:
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t src_mac;
	uint64_t dst_mac;
	uint8_t type;

	void set_src_ip(const std::string &s) { src_ip = string_to_ip(s); }
	void set_dst_ip(const std::string &s) { dst_ip = string_to_ip(s); }
	void set_src_mac(const std::string &s) { src_mac = string_to_mac(s); }
	void set_dst_mac(const std::string &s) { dst_mac = string_to_mac(s); }

	static uint32_t string_to_ip(const std::string &s);
	static uint64_t string_to_mac(const std::string &s);
};

class DpdkTransport
{
public:
	DpdkTransport(const std::unordered_map<uint8_t, MsgQueue> &rx_queues);
	~DpdkTransport();

	void init(uint16_t port); // TODO how to handle multiple ports
	void terminate();
	void send(const std::string &msg, const MsgInfo &msg_info);
	uint64_t get_mac();

private:
	uint32_t m_next_msg_id;
	bool m_is_init;
	uint16_t m_port;
	const std::unordered_map<uint8_t, MsgQueue> m_rx_queues;
};

#endif /* DPDK_TRANSPORT_H */