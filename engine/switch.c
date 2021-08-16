#include "init.h"
#include "switch.h"


static uint32_t get_next_hop(struct rte_mbuf *pkt);
inline static void enqueue_port(struct rte_mbuf *pkt, uint32_t kni_port_index);
static void flush_port_queue(uint32_t kni_port_index);
static void print_stats(void);
static void rx_thread(void);
inline static void flush_all(void);


static struct kni_interface_stats kni_stats[KNI_KTHREAD];
static struct coprocessor_stats nf_stats[KNI_KTHREAD];

/* Print out statistics on packets handled */
static void
print_stats(void)
{
	uint16_t i;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\n**KNI Port statistics**\n"
	       "======  ============  ============  ============  ============\n"
	       " Port    rx_packets    parse_err     tx_packets    tx_dropped\n"
	       "------  ------------  ------------  ------------  ------------\n");
	for (i = 0; i < KNI_KTHREAD; i++) {

		printf("%7d %13"PRIu64" %13"PRIu64" %13"PRIu64" "
							"%13"PRIu64"\n", i,
						kni_stats[i].rx_packets,
						kni_stats[i].parse_err,
						kni_stats[i].tx_packets,
						kni_stats[i].tx_dropped);
	}
	printf("======  ============  ============  ============  ============\n");

	printf("\n**Coprocessor statistics**\n"
	       "======  ============  ============  ============  ============\n"
	       " NF      rx_packets    rx_dropped     tx_packets    tx_dropped\n"
	       "------  ------------  ------------  ------------  ------------\n");
	for (i = 0; i < KNI_KTHREAD; i++) {

		printf("%7d %13"PRIu64" %13"PRIu64" %13"PRIu64" "
							"%13"PRIu64"\n", i,
						nf_stats[i].rx_packets,
						nf_stats[i].rx_dropped,
						nf_stats[i].tx_packets,
						nf_stats[i].tx_dropped);
	}
	printf("======  ============  ============  ============  ============\n");


	return;
}


static uint32_t
get_next_hop(struct rte_mbuf *pkt) {
	uint16_t kni_id;
	struct ether_hdr *eth;
	struct ipv4_hdr *ip_hdr;
	uint32_t ipdata_offset, ip_dst;

	eth = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
	uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);
	if(ether_type != ETHER_TYPE_IPv4){
		return UINT16MAX;
	}

	ipdata_offset = sizeof(struct ether_hdr);

    ip_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(pkt, char *) + ipdata_offset);
    ip_dst = rte_be_to_cpu_32(ip_hdr->dst_addr);

    kni_id = tx_mgr.routing_table[ip_dst & 0xFFFF];

    return kni_id;
}

static void
flush_port_queue(uint32_t kni_port_index) {
    uint16_t i, sent;
    
    if(kni_port_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *port_buf;

	port_buf = &tx_mgr.tx[kni_port_index];
	if (port_buf->count == 0)
    	return;

    sent = rte_kni_tx_burst(kni_list[kni_port_index], port_buf->buffer, port_buf->count);
    if (unlikely(sent < port_buf->count)) {
    	for (i = sent; i < port_buf->count; i++) {
        	rte_pktmbuf_free(port_buf->buffer[i]);
        }
    }
    
    kni_stats[kni_port_index].tx_packets += sent;
    kni_stats[kni_port_index].tx_dropped += port_buf->count - sent;

    port_buf->count = 0;
}

static void
flush_nf_rx_queue(uint32_t coprocessor_index){
	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *nf_buf;
    struct rte_ring *ring;
    uint16_t i, sent;

    nf_buf = &nf[coprocessor_index].rx;
	if (nf_buf->count == 0)
    	return;
    ring = nf[coprocessor_index].rx_q;

    sent = rte_ring_enqueue_bulk(ring, (void **)nf_buf->buffer, nf_buf->count, NULL);

    if (unlikely(sent < nf_buf->count)) {
    	for (i = sent; i < nf_buf->count; i++) {
        	rte_pktmbuf_free(nf_buf->buffer[i]);
        }
    }
    
    nf_stats[coprocessor_index].rx_packets += sent;
    nf_stats[coprocessor_index].rx_dropped += nf_buf->count - sent;

    nf_buf->count = 0;
}

static void
flush_nf_tx_queue(uint32_t coprocessor_index){
	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *nf_buf;
    struct rte_ring *ring;
    uint16_t i, sent;

    nf_buf = &nf[coprocessor_index].tx;
	if (nf_buf->count == 0)
    	return;
    ring = nf[coprocessor_index].tx_q;

    sent = rte_ring_enqueue_bulk(ring, (void **)nf_buf->buffer, nf_buf->count, NULL);

    if (unlikely(sent < nf_buf->count)) {
    	for (i = sent; i < nf_buf->count; i++) {
        	rte_pktmbuf_free(nf_buf->buffer[i]);
        }
    }
    
    nf_stats[coprocessor_index].tx_packets += sent;
    nf_stats[coprocessor_index].tx_dropped += nf_buf->count - sent;

    nf_buf->count = 0;
}


inline static void
enqueue_port(struct rte_mbuf *pkt, uint32_t kni_port_index) {

	if(kni_port_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *port_buf;

    port_buf = &tx_mgr.tx[kni_port_index];
    port_buf->buffer[port_buf->count++] = pkt;
    if (port_buf->count == PKT_BURST_SZ) {
    	flush_port_queue(kni_port_index);
    }
}

inline static void
enqueue_nf_rx(struct rte_mbuf *pkt, uint32_t coprocessor_index) {

	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *nf_buf;

    // If not rx, the packet is destined to tx.
    nf_buf = &nf[coprocessor_index].rx ;
    nf_buf->buffer[nf_buf->count++] = pkt;
    if (nf_buf->count == PKT_BURST_SZ) {
    	flush_nf_rx_queue(coprocessor_index);
    }
}

inline static void
enqueue_nf_tx(struct rte_mbuf *pkt, uint32_t coprocessor_index) {

	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

    struct packet_buf *nf_buf;

    // If not rx, the packet is destined to tx.
    nf_buf = &nf[coprocessor_index].tx;
    nf_buf->buffer[nf_buf->count++] = pkt;
    if (nf_buf->count == PKT_BURST_SZ) {
    	flush_nf_tx_queue(coprocessor_index);
    }
}

inline static void
flush_all(void) {

	uint32_t nb_kni = KNI_KTHREAD, i;
	//Transmit packets.
	for (i = 0; i < nb_kni; i++) {
		flush_nf_rx_queue(i);
		flush_port_queue(i);
	}
}

static void
rx_thread(void)
{
	uint16_t i, num, j;
	//uint32_t counter = 0;
	uint32_t nb_kni = KNI_KTHREAD;
	//uint32_t print_delay = PRINT_DELAY;
	struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

	//Recieve packets.
	for (i = 0; i < nb_kni; i++) {
		/* Burst rx from kni */
		num = rte_kni_rx_burst(kni_list[i], pkts_burst, PKT_BURST_SZ);
		kni_stats[i].rx_packets += num;
		
		for (j = 0; j < num; j++) {
			uint32_t kni_port_index = get_next_hop(pkts_burst[j]);
			if(kni_port_index == UINT16MAX){
				rte_pktmbuf_free(pkts_burst[j]);
				kni_stats[i].parse_err += 1;
			}else{
				// the target kni index also equals the coprocessor index.
				enqueue_nf_rx(pkts_burst[j], kni_port_index);
			}
		}

		rte_kni_handle_request(kni_list[i]);
	}


	flush_all();


	//Recieve packets from Ring buffers
	for (i = 0; i < nb_kni; i++) {
		//Receive packets from coprocess i and send it to kni i.
		num = rte_ring_dequeue_burst(nf[i].tx_q, (void **)pkts_burst, PKT_BURST_SZ, NULL);
		for (j = 0; j < num; j++) {
			enqueue_port(pkts_burst[j], i);
		}
	}

	flush_all();

}


static void
coprocessor(void)
{
	uint16_t num, j;
	struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

	unsigned int lcore_id = rte_lcore_id();

	num = rte_ring_dequeue_burst(nf[lcore_id].rx_q, (void **)pkts_burst, PKT_BURST_SZ, NULL);
	
	for (j = 0; j < num; j++) {
		// Process packet.
		enqueue_nf_tx(pkts_burst[j], lcore_id);
	}
	
	flush_nf_tx_queue(lcore_id);

}



int
main_loop(__rte_unused void *arg)
{
	int32_t f_stop;
	double cpu_time;
	uint64_t start = rte_get_tsc_cycles();
	unsigned int lcore_id = rte_lcore_id();

	if(lcore_id == KNI_KTHREAD){
		// Main thread
		while (1) {
			f_stop = rte_atomic32_read(&kni_stop);
			if (f_stop)
				break;
			rx_thread();

			cpu_time = (double)(rte_get_tsc_cycles() - start) / rte_get_tsc_hz();
			if(cpu_time > PRINT_DELAY){
				print_stats();
				start = rte_get_tsc_cycles();
			}
		}	
	}else{
		// Coprocessor at core l_core.
		while (1) {
			f_stop = rte_atomic32_read(&kni_stop);
			if (f_stop)
				break;
			coprocessor();
		}

	}


	

	return 0;

}
