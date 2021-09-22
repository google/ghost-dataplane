#include "nfs/firewall/firewall.h"

//#define DISABLE_NF 1

#define ENABLE_FW_NF 1

int
process_packet(struct rte_mbuf *pkt);

int
coprocessor_teardown(void);

int
coprocessor_setup(void);

int num_rules;
