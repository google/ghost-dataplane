#include "coprocessor.h"

const char *rule_file = "./nfs/firewall/rules.json";

int
coprocessor_setup(void)
{
	/*
		Set up the coprocessor with the required NF state.
	*/
#ifdef ENABLE_FW_NF
	struct fw_rule **rules;
	rules = setup_rules(&num_rules, rule_file);
	lpm_setup(rules, num_rules);
#endif

	return 0;
}

int
coprocessor_teardown(void)
{
	/*
		Teardown the NF state within in coprocessor.

	*/
#ifdef ENABLE_FW_NF
	lpm_teardown(rules, num_rules);
#endif

	return 0;
}

int
process_packet(struct rte_mbuf *pkt)
{
	/*
		Process coprocessor packet.

		Input:
			- Packet descriptor.
	*/
#ifdef ENABLE_FW_NF
	if (fw_packet_handler(pkt) != FW_FORWARD)
		return -1;
#endif

	return 0;
}
