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

	/* Launch per-lcore function on every lcore */
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(i) {
		if (rte_eal_wait_lcore(i) < 0)
			return -1;
	}


	kni_free_kni();


}
