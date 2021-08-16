#include "init.h"

struct rte_mempool *pktmbuf_pool = NULL;
struct rte_kni *kni_list[KNI_KTHREAD];
struct queue_mgr *rx_mgr[KNI_KTHREAD];

struct queue_mgr tx_mgr;
struct coprocessor_mgr nf[KNI_KTHREAD];

rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);

static void
engine_free(void){
	
    if (tx_mgr.tx != NULL) {
           rte_free(tx_mgr.tx);
    }
    
}

int read_config(void)
{
	/*
		TODO: This should read a txt file in the future.
	*/
	uint32_t i;
	const unsigned ringsize = NF_QUEUE_RINGSIZE;

	for(i = 0; i < KNI_KTHREAD; i++){
		tx_mgr.routing_table[i] = UINT16MAX;
	}


	for(i = 0; i < KNI_KTHREAD; i++){
		tx_mgr.kni_cnf[i].id = i;
		tx_mgr.kni_cnf[i].ip = RTE_IPV4(192, 167, 10, i + 1);
		tx_mgr.kni_cnf[i].core_id = i;
	}

	tx_mgr.nb_active_kni = KNI_KTHREAD;



	for(i = 0; i < KNI_KTHREAD; i++){
		uint32_t instance_id = i;
		unsigned socket_id;
		const char *rq_name;
		const char *tq_name;
		socket_id = rte_socket_id();
        rq_name = get_rx_queue_name(instance_id);
        tq_name = get_tx_queue_name(instance_id);
        nf[i].rx_q = rte_ring_create(rq_name, ringsize, socket_id, RING_F_SC_DEQ); /* multi prod, single cons */
        nf[i].tx_q = rte_ring_create(tq_name, ringsize, socket_id, RING_F_SC_DEQ); /* multi prod, single cons */
	}

	//Populate the routing table.
	for( i = 0; i < tx_mgr.nb_active_kni; i++){
		tx_mgr.routing_table[tx_mgr.kni_cnf[i].ip & 0xFFFF] = tx_mgr.kni_cnf[i].id;
	}

	return 0;
}


int init(void){

 	/* Create the mbuf pool */
	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF,
		MEMPOOL_CACHE_SZ, 0, MBUF_DATA_SZ, rte_socket_id());
	if (pktmbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Could not initialise mbuf pool\n");
		return -1;
	}

    tx_mgr.tx = rte_calloc(NULL, KNI_KTHREAD, sizeof(struct packet_buf), RTE_CACHE_LINE_SIZE);
    if (tx_mgr.tx == NULL) {
        goto engine_free;
    }
	
	if( read_config() != 0 ){
		goto engine_free;	
	}
	
	return 0;

engine_free:
    RTE_LOG(ERR, APP, "Can't allocate required struct.\n");
    engine_free();
    return -1;

}

