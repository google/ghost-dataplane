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

#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_kni.h>
#include <stdbool.h>
#include <rte_malloc.h>

#ifndef _ENGINE_INIT_H_
#define _ENGINE_INIT_H_

#define UNKNOWN_PORT 0xFFFF
#define ROUTING_TBL_SZ 0x10000


#define RTE_IPV4(a, b, c, d) ((uint32_t)(((a) & 0xff) << 24) | \
                        (((b) & 0xff) << 16) | \
                        (((c) & 0xff) << 8)  | \
                        ((d) & 0xff))

/* Max size of a single packet */
#define MAX_PACKET_SZ           2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in mempool that is created */
#define NB_MBUF                 (8192 * 16)

/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ            32

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ        PKT_BURST_SZ

#define KNI_KTHREAD 5

#define NF_QUEUE_RINGSIZE 16384

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1


struct packet_buf {
        struct rte_mbuf *buffer[PKT_BURST_SZ];
        uint16_t count;
};

struct kni_config {
	uint32_t id;
	uint32_t ip;
	uint32_t core_id;        
};

struct queue_mgr {
        struct packet_buf *tx; 
        uint16_t nb_active_kni;
        struct kni_config kni_cnf[KNI_KTHREAD];
        uint16_t routing_table[ROUTING_TBL_SZ];
};

struct coprocessor_mgr {
        struct rte_ring *rx_q;
        struct rte_ring *tx_q;
        struct packet_buf rx;
        struct packet_buf tx;
};


struct core_mapping {
        uint16_t rx_lcore;
        uint16_t coproc_lcore_first;
        uint16_t coproc_lcore_last;
};

/* Mempool for mbufs */
extern struct rte_mempool * pktmbuf_pool;
extern struct rte_kni *kni_list[KNI_KTHREAD];
extern struct queue_mgr tx_mgr;
extern struct coprocessor_mgr nf[KNI_KTHREAD];
extern rte_atomic32_t kni_stop;

int init(void);
int read_config(void);

#define MP_NF_RXQ_NAME "MProc_Client_%u_RX"
#define MP_NF_TXQ_NAME "MProc_Client_%u_TX"

static inline const char *
get_rx_queue_name(unsigned id) {
        /* buffer for return value. Size calculated by %u being replaced
         * by maximum 3 digits (plus an extra byte for safety) */
        static char buffer[sizeof(MP_NF_RXQ_NAME) + 2];

        snprintf(buffer, sizeof(buffer) - 1, MP_NF_RXQ_NAME, id);
        return buffer;
}

/*
 * Given the tx queue name template above, get the queue name
 */
static inline const char *
get_tx_queue_name(unsigned id) {
        /* buffer for return value. Size calculated by %u being replaced
         * by maximum 3 digits (plus an extra byte for safety) */
        static char buffer[sizeof(MP_NF_TXQ_NAME) + 2];

        snprintf(buffer, sizeof(buffer) - 1, MP_NF_TXQ_NAME, id);
        return buffer;
}
#endif


