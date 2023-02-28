#ifndef DPDK_TRANSPORT_H
#define DPDK_TRANSPORT_H

#include<stdint.h>

class MsgInfo {
	public:
	uint32_t src_ip;
	uint32_t dst_ip;
	uint64_t src_mac;
	uint64_t dst_mac;
	uint8_t type;

	void set_src_ip(const std::string& s) { src_ip = string_to_ip(s); }
	void set_dst_ip(const std::string& s) { dst_ip = string_to_ip(s); }
	void set_src_mac(const std::string& s) { src_mac = string_to_mac(s); }
	void set_dst_mac(const std::string& s) { dst_mac = string_to_mac(s); }

	static uint32_t string_to_ip(const std::string& s);
	static uint64_t string_to_mac(const std::string& s);
}

class  DpdkTransport {
	public:
	DpdkTransport();
	~DpdkTransport();
	void send(const std::string& msg, const MsgInfo& msg_info);
	void run();
	void terminate();
	uint64_t get_mac();

	private:
	uint32_t next_msg_id;
};

#endif /* DPDK_TRANSPORT_H */