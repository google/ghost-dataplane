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



