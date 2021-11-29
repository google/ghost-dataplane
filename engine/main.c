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

#include "interface.h"
#include "init.h"
#include "switch.h"

/* Total octets in ethernet header */
#define KNI_ENET_HEADER_SIZE    14

/* Total octets in the FCS */
#define KNI_ENET_FCS_SIZE       4

#define KNI_US_PER_SECOND       1000000
#define KNI_SECOND_PER_DAY      86400

static void signal_handler(int signum);

/* Custom handling of signals to handle stats and kni processing */
static void
signal_handler(int signum)
{

	/* When we receive a RTMIN or SIGINT signal, stop kni processing */
	if (signum == SIGRTMIN || signum == SIGINT){
		printf("SIGRTMIN is received, and the KNI processing is "
							"going to stop\n");
		rte_atomic32_inc(&kni_stop);
		return;
    	}
}

/* Initialise ports/queues etc. and start main loop on each core */
int
main(int argc, char** argv)
{
	int ret;
	unsigned int n_lcore;
	uint16_t i;
	struct core_mapping rsrc;

	/* Associate signal_hanlder function with USR signals */
	signal(SIGRTMIN, signal_handler);
	signal(SIGINT, signal_handler);

	/* Initialise EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not initialise EAL (%d)\n", ret);
	argc -= ret;
	argv += ret;

	if( init() != 0){
		rte_exit(EXIT_FAILURE, "Initialization failed\n");
	}

	init_kni_port(0);

	n_lcore = rte_lcore_count();
	if(n_lcore != KNI_KTHREAD + 1){
		rte_exit(EXIT_FAILURE, \
			"Number of lcores (%d) != KNI_KTHREAD + 1 (%d)\n", \
			 n_lcore, KNI_KTHREAD + 1);
	}
	
	for(i = 0; i < n_lcore; i++){
		// This works in the N2D machine.
		if( rte_lcore_index(i) == -1){
			rte_exit(EXIT_FAILURE, \
			"Unexpected index for for lcore (%d)\n", i);
		}
	}
	
	/*
		- lcore id '#vports' is reserved for the Rx thread.
		- lcore id [0 .. (#vports-1)] is reserved for coprocessor [0..(#vports-1)].

	*/
	rsrc.rx_lcore = KNI_KTHREAD;
	rsrc.coproc_lcore_first = 0;
	rsrc.coproc_lcore_last = KNI_KTHREAD - 1;

	/* Launch per-lcore function on every lcore */
	rte_eal_mp_remote_launch(main_loop, &rsrc, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(i) {
		if (rte_eal_wait_lcore(i) < 0)
			return -1;
	}
	kni_free_kni();
}
