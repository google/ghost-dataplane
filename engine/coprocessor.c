/*
Copyright 2021 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

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
