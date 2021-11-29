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

#include "init.h"
#include "switch.h"


static uint32_t get_next_hop(struct rte_mbuf *pkt);
inline static void enqueue_port(struct rte_mbuf *pkt, uint32_t kni_port_index);
static void flush_port_queue(uint32_t kni_port_index);
static void print_stats(void);
static void fastpath(uint32_t nb_kni, uint32_t nb_coproc);
inline static void flush_all(uint32_t nb_kni, uint32_t nb_coproc);


static struct kni_interface_stats kni_stats[KNI_KTHREAD];
static struct coprocessor_stats nf_stats[KNI_KTHREAD];

/* Print out statistics on packets handled */
static void
print_stats(void)
{
	/*
		Print the packet counters for each port and coprocessor.
	*/

	uint16_t i;

	// We clear the screen in the next few lines.
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };
	printf("%s%s", clr, topLeft);

	// We show the virtual port statistics in the next few lines.
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
		kni_stats[i].rx_packets = 0;
		kni_stats[i].parse_err  = 0;
		kni_stats[i].tx_packets = 0;
		kni_stats[i].tx_dropped = 0;

	}
	printf("======  ============  ============  ============  ============\n");

	// We show the coprocessor statistics in the next few lines.
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
		nf_stats[i].rx_packets = 0;
		nf_stats[i].rx_dropped = 0;
		nf_stats[i].tx_packets = 0;
		nf_stats[i].tx_dropped = 0;

	}
	printf("======  ============  ============  ============  ============\n");

	return;
}


static uint32_t
get_next_hop(struct rte_mbuf *pkt) {
	/*
		Determine the outport port for the packet.

		Inputs: 
			- rte_mbuf : packet descriptor
		Returns:
			- The index of the virtual port to forward the pkt.

		The port id is determines from the last 16bits of
		the pkt destination IP (e.g., xx in 192.167.xx.xx).
	*/

	uint16_t kni_id;
	struct ether_hdr *eth;
	struct ipv4_hdr *ip_hdr;
	uint32_t ipdata_offset, ip_dst;

	/*
		We parse the ethernet hdr and check whether the next hdr is IP.
		If not, set the outgoing port as UNKNOWN.
	*/
	eth = rte_pktmbuf_mtod(pkt, struct ether_hdr *);
	uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);
	if(ether_type != ETHER_TYPE_IPv4){
		return UNKNOWN_PORT;
	}

	/*
		We parse the IP hdr and extract the destination IP.
	*/
	ipdata_offset = sizeof(struct ether_hdr);
    	ip_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(pkt, char *) + ipdata_offset);
    	ip_dst = rte_be_to_cpu_32(ip_hdr->dst_addr);

    	/*
		Query the routing table mapping the ip address (16 bits) to vport id.
		The routing table is implemented as a array indexed by destination ip.
    	*/
    	kni_id = tx_mgr.routing_table[ip_dst & 0xFFFF];

    	return kni_id;
}

static void
flush_port_queue(uint32_t kni_idx) {
	/*
		Flush the port buffers.

		Input:
			- The vport id to flush pkts to.

		Packets are buffered so that packet descriptors
		can be enqueued as a bulk ops to amortize the cost.

		The fast path calls this routing periodically or when the temporary
		buffer is full.

	*/

    	uint16_t packet_idx, sent;
  	// Do not proceed with the flush ops if the vport id is invalid.
    	if(kni_idx >= tx_mgr.nb_active_kni){
		return;
	}

	// Get the vport's temporary pkt buffer addr.
    	struct packet_buf *port_buf;
	port_buf = &tx_mgr.tx[kni_idx];
	if (port_buf->count == 0)
    		return;

    	/*
    		Enqueue the packets (e.g., flush the buffer).
    		Free the packet descriptors of the failed enqueue pkts.
    	*/
    	sent = rte_kni_tx_burst(kni_list[kni_idx], port_buf->buffer, port_buf->count);
    	if (unlikely(sent < port_buf->count)) {
    		for (packet_idx = sent; packet_idx < port_buf->count; packet_idx++) {
        		rte_pktmbuf_free(port_buf->buffer[packet_idx]);
        	}
    	}
    
    	// Update the port statistics (e.g., packets sent and dropped)
    	kni_stats[kni_idx].tx_packets += sent;
    	kni_stats[kni_idx].tx_dropped += port_buf->count - sent;

    	// Reset the temporaty buffer counter to reflect it containing 0 pkts.
    	port_buf->count = 0;
}

static void
flush_nf_rx_queue(uint32_t coproc_idx){
	/*
		Flush the NF RX queue.

		Input:
			- The coprocessor id whose RX queue pkts are to be flushed to.

		Packets are buffered so that packet descriptors
		can be enqueued as a bulk ops to amortize the cost.

		The fast path calls this routing periodically or when the temporary
		buffer is full.

	*/

	/* 
		Do not proceed with the flush ops if the coprocessor id is invalid.
		Coprocessor id = i is responsible for packets going to vport id = i.
	*/
	if(coproc_idx >= tx_mgr.nb_active_kni){
		return;
	}

    	struct packet_buf *nf_buf = &nf[coproc_idx].rx;
    	struct rte_ring *ring;
    	uint16_t packet_idx, sent;

    	/*
    		Get the coprocessor's RX temporary pkt buffer addr.
    		Get the coprocessor's RX ring buffer.
    	*/
    	if (nf_buf->count == 0)
    		return;
    	ring = nf[coproc_idx].rx_q;

    	/*
    		Enqueue the packets (e.g., flush the buffer).
    		Free the packet descriptors of the failed enqueue pkts.
    	*/
    	sent = rte_ring_enqueue_bulk(ring, (void **)nf_buf->buffer, nf_buf->count, NULL);
    	if (unlikely(sent < nf_buf->count)) {
    		for (packet_idx = sent; packet_idx < nf_buf->count; packet_idx++) {
        		rte_pktmbuf_free(nf_buf->buffer[packet_idx]);
        	}
    	}
    
    	// Update the port statistics (e.g., packets sent and dropped)
    	nf_stats[coproc_idx].rx_packets += sent;
    	nf_stats[coproc_idx].rx_dropped += nf_buf->count - sent;

    	// Reset the temporaty buffer counter to reflect it containing 0 pkts.
    	nf_buf->count = 0;
}

static void
flush_nf_tx_queue(uint32_t coproc_idx){
	/*
		Flush the NF RX queue.

		Input:
			- The coprocessor id whose Tx ring packets are to be flushed to.

		Packets are buffered so that packet descriptors
		can be enqueued as a bulk ops to amortize the cost.

		The coprocessor thread calls this periodically or when the temporary
		buffer is full.

	*/

	if(coproc_idx >= tx_mgr.nb_active_kni){
		return;
	}

	struct packet_buf *nf_buf = &nf[coproc_idx].tx;
	struct rte_ring *ring;
	uint16_t packet_idx, sent;

	if (nf_buf->count == 0)
		return;
	ring = nf[coproc_idx].tx_q;

	sent = rte_ring_enqueue_bulk(ring, (void **)nf_buf->buffer, nf_buf->count, NULL);

	if (unlikely(sent < nf_buf->count)) {
		for (packet_idx = sent; packet_idx < nf_buf->count; packet_idx++) {
	    	rte_pktmbuf_free(nf_buf->buffer[packet_idx]);
	    }
	}

	nf_stats[coproc_idx].tx_packets += sent;
	nf_stats[coproc_idx].tx_dropped += nf_buf->count - sent;

	nf_buf->count = 0;
}


inline static void
enqueue_port(struct rte_mbuf *pkt, uint32_t kni_port_index) {
	/*
		Enqueue pkt to the temporary buffer of the port.

		Input:
			- Packet desciptor (to be enqueued)
			- The vport id where the packet is to be enqueued.
	*/

	// Check if the vport is valid.
	if(kni_port_index >= tx_mgr.nb_active_kni){
		return;
	}

	// Store the packet in buffer and flush when the buffer is full.
	struct packet_buf *port_buf = &tx_mgr.tx[kni_port_index];
	port_buf->buffer[port_buf->count++] = pkt;
	if (port_buf->count == PKT_BURST_SZ) {
		flush_port_queue(kni_port_index);
	}
}

inline static void
enqueue_nf_rx(struct rte_mbuf *pkt, uint32_t coprocessor_index) {
	/*
		Enqueue pkt to the temporary buffer of the coprocessor (Rx).

		Input:
			- Packet desciptor (to be enqueued)
			- The coprocessor id where the packet is to be enqueued.
	*/

	// Check if the coprocessor is valid.
	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

	// Store the packet in buffer and flush when the buffer is full.
	struct packet_buf *nf_buf = &nf[coprocessor_index].rx ;
	nf_buf->buffer[nf_buf->count++] = pkt;
	if (nf_buf->count == PKT_BURST_SZ) {
		flush_nf_rx_queue(coprocessor_index);
	}
}

inline static void
enqueue_nf_tx(struct rte_mbuf *pkt, uint32_t coprocessor_index) {
	/*
		Enqueue pkt to the temporary buffer of coprocessor (Tx).
		This packet is bound to the fast path.

		Input:
			- Packet desciptor (to be enqueued)
			- The coprocessor id where the packet is to be enqueued.
	*/

	// Check if the coprocessor is valid.
	if(coprocessor_index >= tx_mgr.nb_active_kni){
		return;
	}

	// Store the packet in buffer and flush when the buffer is full.
    	struct packet_buf *nf_buf = &nf[coprocessor_index].tx;
    	nf_buf->buffer[nf_buf->count++] = pkt;
    	if (nf_buf->count == PKT_BURST_SZ) {
    		flush_nf_tx_queue(coprocessor_index);
    	}
}

inline static void
flush_all(uint32_t nb_kni, uint32_t nb_coproc) {
	/*
		Flush all port and Rx temporary buffers.
		Do not call 'flush_nf_tx_queue'
		as it runs in the coprocessor thread.

		Inputs:
			- The number of KNI ports.
			- The number of Coprocessors.
	*/

	uint32_t kni_idx, coproc_idx;
	
	for (kni_idx = 0; kni_idx < nb_kni; kni_idx++) {
		flush_port_queue(kni_idx);
	}

	for (coproc_idx = 0; coproc_idx < nb_coproc; coproc_idx++) {
		flush_nf_rx_queue(coproc_idx);
	}
}

static void
fastpath(uint32_t nb_kni, uint32_t nb_coproc){
	/*
		Fast path spinning thread. This is also referred to as the Rx thread.

		Input:
			- The number of vports.
			- The number of coprocessors.

		Get pkt descriptors (in bulk) from vports and coprocessor Tx rings.

		Identifies the next hop and forwards the packet into the tmp buffer.

	*/

	uint16_t kni_idx, packet_idx, n_pkts_recvd, coproc_idx;
	struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

	// Iterate over the vports.
	for (kni_idx = 0; kni_idx < nb_kni; kni_idx++) {
		// Burst rx from kni
		n_pkts_recvd = rte_kni_rx_burst(kni_list[kni_idx], pkts_burst, PKT_BURST_SZ);
		kni_stats[kni_idx].rx_packets += n_pkts_recvd;
		
		/*
			In the next few lines we
			- identify the packet egress port.
			- enqueue packets to coprocessor rx ring buffers.
		*/
		for (packet_idx = 0; packet_idx < n_pkts_recvd; packet_idx++) {
			uint32_t kni_egress_port = get_next_hop(pkts_burst[packet_idx]);
			if(kni_egress_port == UNKNOWN_PORT){
				rte_pktmbuf_free(pkts_burst[packet_idx]);
				kni_stats[kni_idx].parse_err += 1;
			}else{
				#ifdef DISABLE_NF
				enqueue_port(pkts_burst[packet_idx], kni_egress_port);
				#else
				enqueue_nf_rx(pkts_burst[packet_idx], kni_egress_port);
				#endif
			}
		}

		// This handles net config. requests.
		rte_kni_handle_request(kni_list[kni_idx]);
	}

	// Periodically flush pkts in all temporary buffers.
	flush_all(nb_kni, nb_coproc);

	#ifndef DISABLE_NF
	// Iterate over coprocessors.
	for (coproc_idx = 0; coproc_idx < nb_coproc; coproc_idx++) {
		//Receive packets from coprocess i and send it to vport i.
		n_pkts_recvd = rte_ring_dequeue_burst(nf[coproc_idx].tx_q, (void **)pkts_burst, PKT_BURST_SZ, NULL);
		for (packet_idx = 0; packet_idx < n_pkts_recvd; packet_idx++) {
			enqueue_port(pkts_burst[packet_idx], coproc_idx);
		}
	}

	// Periodically flush pkts in all temporary buffers.
	flush_all(nb_kni, nb_coproc);
	#endif

}


static void
coprocessor(void){
	/*
		Coprocessor spinning thread.

		Get pkt descriptors (in bulk) from Rx rings.

		Dequeues the packet and invokes coprocessor.

	*/

	int ret;
	uint16_t packet_idx;
	uint32_t packets_dequeued;
	struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

	// The lcore id acts as the coprocessor ID.
	unsigned int lcore_id = rte_lcore_id();

	// Dequeue a burst of packets from the Rx ring buffer.
	packets_dequeued = rte_ring_dequeue_burst(nf[lcore_id].rx_q, (void **)pkts_burst, PKT_BURST_SZ, NULL);
	for (packet_idx = 0; packet_idx < packets_dequeued; packet_idx++) {
		ret = process_packet(pkts_burst[packet_idx]);
		if( ret == 0)
			enqueue_nf_tx(pkts_burst[packet_idx], lcore_id);
		else
			rte_pktmbuf_free(pkts_burst[packet_idx]);
	}
	
	// Periodically flush the pkt buffers.
	flush_nf_tx_queue(lcore_id);
}



int
main_loop(__rte_unused void *arg)
{
	/*
		Distriguishes between Rx and coprocessor threads.

		Periodically displays statistics and exits the loop
		when the SIGRTMIN/SIGINT signal gets handled.

		Please recall, every vport gets a coprocessor since a container instance
		will be running there.

	*/

	int32_t f_stop;
	double cpu_time;
	uint64_t start = rte_get_tsc_cycles();
	unsigned int lcore_id = rte_lcore_id();
	struct core_mapping *rsrc = arg;

	// Identify what processing is to run on this core.
	bool is_rx = (lcore_id == rsrc->rx_lcore);
	bool is_coproc = ((lcore_id >= rsrc->coproc_lcore_first) && 
					  (lcore_id <= rsrc->coproc_lcore_last ));

	uint32_t nb_kni = rsrc->coproc_lcore_last - rsrc->coproc_lcore_first + 1;
	uint32_t nb_coproc = nb_kni;

	if(is_rx){
		// RX thread also referred to the as the fast path.
		while (1) {
			// Check if the function is to be exited.
			f_stop = rte_atomic32_read(&kni_stop);
			if (f_stop)
				break;

			fastpath(nb_kni, nb_coproc);

			// Periodically print statistics.
			cpu_time = (double)(rte_get_tsc_cycles() - start) / rte_get_tsc_hz();
			if(cpu_time > PRINT_DELAY){
				print_stats();
				start = rte_get_tsc_cycles();
			}
		}	
	}else if(is_coproc){
		#ifndef DISABLE_NF
		if( coprocessor_setup() != 0)
			rte_exit(EXIT_FAILURE, "Coprocessor setup failed.\n");

		// Coprocessor thread.
		while (1) {
			// Check if the function is to be exited.
			f_stop = rte_atomic32_read(&kni_stop);
			if (f_stop)
				break;
			coprocessor();
		}

		coprocessor_teardown();
		#endif
	}
	return 0;
}
