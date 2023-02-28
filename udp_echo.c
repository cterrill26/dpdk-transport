#include <stdint.h>
#include <rte_byteorder.h>

/**
 * DPDK Transport Header
 */
struct rte_dt_hdr {
	rte_be32_t msg_id;   /* DPDK transport message ID */
	rte_be32_t msg_len;   /* DPDK transport message length */
	rte_be8_t msg_type;   /* DPDK transport message type */
	rte_be8_t frame_id;   /* DPDK transport frame ID */
} __attribute__((__packed__));