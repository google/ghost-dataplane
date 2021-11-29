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

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <libgen.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_malloc.h>

#include <rte_lpm.h>
#include <rte_cycles.h>
#include "../../thirdparty/cJSON.h"

#define IS_NULL_OR_EMPTY_STRING(s) ((s) == NULL || strncmp(s, "", 1) == 0 ? 1 : 0)
#define MAX_RULES 256
#define NUM_TBLS 8

/* Shared data structure containing host port info */
extern struct port_info *ports;

/* Struct for the firewall LPM rules */
struct fw_rule {
        uint32_t src_ip;
        uint8_t depth;
        uint8_t action;
};

/* Struct for printing stats */
struct firewall_pkt_stats {
        uint64_t pkt_drop;
        uint64_t pkt_accept;
        uint64_t pkt_not_ipv4;
        uint64_t pkt_total;
};

struct lpm_request {
        char name[64];
        uint32_t max_num_rules;
        uint32_t num_tbl8s;
        int socket_id;
        int status;
};

enum FW_ACTION{
        FW_FORWARD,
        FW_DROP
};


int
fw_packet_handler(struct rte_mbuf *pkt);

int
lpm_setup(struct fw_rule **rules, int num_rules);

void
lpm_teardown(struct fw_rule **rules, int num_rules);

struct fw_rule **setup_rules(int *total_rules, const char *rules_file);

void
fw_pkt_parse_char_ip(char* ip_dest, uint32_t ip_src);

int
fw_pkt_parse_ip(char* ip_str, uint32_t* dest);

cJSON*
fw_config_parse_file(const char* filename);

int
fw_config_get_item_count(cJSON* config);

struct ipv4_hdr*
fw_pkt_ipv4_hdr(struct rte_mbuf* pkt);

int
fw_pkt_is_ipv4(struct rte_mbuf* pkt);

/* Structs that contain information to setup LPM and its rules */
struct lpm_request *firewall_req;
struct firewall_pkt_stats stats;
struct rte_lpm *lpm_tbl;
struct fw_rule **rules;

#define RTE_IPV4(a, b, c, d) ((uint32_t)(((a) & 0xff) << 24) | \
                        (((b) & 0xff) << 16) | \
                        (((c) & 0xff) << 8)  | \
                        ((d) & 0xff))
