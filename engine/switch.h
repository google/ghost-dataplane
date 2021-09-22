#include <rte_mbuf.h>
#include <rte_kni.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include "coprocessor.h"

#define PRINT_DELAY 2

/* Structure type for recording kni interface specific stats */
struct kni_interface_stats {
	uint64_t rx_packets;
	uint64_t parse_err;
	uint64_t tx_packets;
	uint64_t tx_dropped;
};

struct coprocessor_stats {
	uint64_t rx_packets;
	uint64_t rx_dropped;
	uint64_t tx_packets;
	uint64_t tx_dropped;
};

int main_loop(__rte_unused void *);



