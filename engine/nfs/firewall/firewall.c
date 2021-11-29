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

#include "firewall.h"

void
fw_pkt_parse_char_ip(char* ip_str, uint32_t ipv4) {
        /*
                Convert IP to string.

                Input:
                        - IP string (write to)
                        - ipv4 as uint32_t (read from)
        */
        snprintf(ip_str, 16, "%u.%u.%u.%u", (ipv4 >> 24) & 0xFF, (ipv4 >> 16) & 0xFF,
                (ipv4 >> 8) & 0xFF, ipv4 & 0xFF);
}

int
fw_pkt_parse_ip(char* ip_str, uint32_t* dest) {
        /*
                Convert IP str to IP

                Input:
                        - IP string (read from)
                        - ipv4 as uint32_t (write to)
        */

        int ret;
        int ip[4];

        if (ip_str == NULL || dest == NULL) {
                return -1;
        }

        ret = sscanf(ip_str, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);
        if (ret != 4) {
                return -1;
        }
        *dest = RTE_IPV4(ip[0], ip[1], ip[2], ip[3]);
        return 0;
}

cJSON*
fw_config_parse_file(const char* filename) {
        /*
                Parse the firewall rule config file.

                Input:
                        - File Path.
        */

        if (IS_NULL_OR_EMPTY_STRING(filename)) {
                return NULL;
        }

        FILE* fp = NULL;
        int file_length = 0;
        char temp_buf[255];
        char* tmp = NULL;
        char* line = NULL;
        char* json_str = NULL;
        int str_len = 0;

        fp = fopen(filename, "r");
        if (!fp) {
                return NULL;
        }

        fseek(fp, 0L, SEEK_END);
        file_length = ftell(fp);
        rewind(fp);

        json_str = (char*)malloc(file_length + 1);
        if (!json_str) {
                printf("Unable to allocate space for json_str\n");
                fclose(fp);
                return NULL;
        }
        tmp = json_str;

        while ((line = fgets(temp_buf, file_length, fp)) != NULL) {
                str_len = (int)strlen(line);
                memcpy(tmp, line, str_len);
                tmp += str_len;
        }

        json_str[file_length] = '\0';
        fclose(fp);

        return cJSON_Parse(json_str);
}

int
fw_config_get_item_count(cJSON* config) {
        /*
                Get #items of the firewall config.

                Input:
                        - Pointer to firewall config.
        */
        int arg_count = 0;
        if (config == NULL) {
                return 0;
        }
        if (config->child == NULL) {
                return 0;
        }
        cJSON* current = config->child;
        while (current != NULL) {
                ++arg_count;
                current = current->next;
        }

        return arg_count;
}

struct ipv4_hdr*
fw_pkt_ipv4_hdr(struct rte_mbuf* pkt) {
        /*
                Extract the packet ipv4 header.

                Input:
                        - The pkt descriptor.

        */
        uint32_t ipdata_offset;
        struct ipv4_hdr *ip_hdr;

        ipdata_offset = sizeof(struct ether_hdr);

        ip_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(pkt, char *) + ipdata_offset);

        /* In an IP packet, the first 4 bits determine the version.
         * The next 4 bits are called the Internet Header Length, or IHL.
         * DPDK's ipv4_hdr struct combines both the version and the IHL into one uint8_t.
         */
        uint8_t version = (ip_hdr->version_ihl >> 4) & 0b1111;
        if (unlikely(version != 4)) {
                return NULL;
        }
        return ip_hdr;
}


int
fw_pkt_is_ipv4(struct rte_mbuf* pkt) {
        /*
                Returns 1 if the pkt is ipv4 else 0.

                Input:
                        - The pkt descriptor.
        */
        return fw_pkt_ipv4_hdr(pkt) != NULL;
}

int
fw_packet_handler(struct rte_mbuf *pkt) {
        /*
                The network function packet handler.
                This is called from the coprocessor.

                Input:
                        - The pkt descriptor.
        */
        struct ipv4_hdr *ip_hdr;
        int ret;
        uint32_t rule = 0;
        enum FW_ACTION action;

        stats.pkt_total++;

        if (!fw_pkt_is_ipv4(pkt)) {
                // Packet is not ipv4.
                stats.pkt_not_ipv4++;
                action = FW_DROP;
        }

        // Perform longest prefix matching.
        ip_hdr = fw_pkt_ipv4_hdr(pkt);
        ret = rte_lpm_lookup(lpm_tbl, rte_be_to_cpu_32(ip_hdr->src_addr), &rule);

        if (ret < 0) {
                // No match.
                action = FW_DROP;
        }

        switch (rule) {
                case 0:
                        // Accept rule.
                        action = FW_FORWARD;
                        break;
                default:
                        // No Accept rule.
                        action = FW_DROP;
                        break;
        }

        return action;
}

int
lpm_setup(struct fw_rule **rules, int num_rules) {
        /*
                Setup the longest prefix matching table.

                Input:
                        - FW rules.
                        - Number of rules.

        */
        int rule_idx, ret;
        uint32_t ip;
        char name[64];
        char ip_string[16];

        snprintf(name, sizeof(name), "fw%d-%"PRIu64, rte_lcore_id(), rte_get_tsc_cycles());

        struct rte_lpm_config conf;

        conf.max_rules = 1024;
        conf.number_tbl8s = 24;

        lpm_tbl = rte_lpm_create(name, rte_socket_id(), &conf);

        if (lpm_tbl == NULL) {
                printf("No existing LPM_TBL\n");
        }

        for (rule_idx = 0; rule_idx < num_rules; ++rule_idx) {
                ip = rules[rule_idx]->src_ip;
                fw_pkt_parse_char_ip(ip_string, ip);
                printf("RULE %d: { ip: %s, depth: %d, action: %d }\n", rule_idx, ip_string, rules[rule_idx]->depth, rules[rule_idx]->action);
                ret = rte_lpm_add(lpm_tbl, rules[rule_idx]->src_ip, rules[rule_idx]->depth, rules[rule_idx]->action);
                if (ret < 0) {
                        printf("ERROR ADDING RULE %d\n", ret);
                        return 1;
                }
        }

        return 0;
}

void
lpm_teardown(struct fw_rule **rules, int num_rules) {
        /*
                Tear down the longest prefix matching table.

                Input:
                        - FW rules.
                        - Number of rules.
        */

        int rule_idx;
        if (rules) {
                for (rule_idx = 0; rule_idx < num_rules; ++rule_idx) {
                        if (rules[rule_idx]) free(rules[rule_idx]);
                }
                free(rules);
        }
}

struct fw_rule
**setup_rules(int *total_rules, const char *rules_file) {
        /*
                Read the firewall rules.

                Input:
                        - Total number of rules.
                        - The rule file.
        */

        int num_rules;
        int rule_idx = 0;

        cJSON *rules_json = fw_config_parse_file(rules_file);
        cJSON *rules_ip = NULL;
        cJSON *depth = NULL;
        cJSON *action = NULL;

        if (rules_json == NULL) {
                rte_exit(EXIT_FAILURE, "%s file could not be parsed/not found. Assure rules file"
                                       " the directory to the rules file is being specified.\n", rules_file);
        }

        num_rules = fw_config_get_item_count(rules_json);
        *total_rules = num_rules;
        rules = (struct fw_rule **) malloc(num_rules * sizeof(struct fw_rule *));
        rules_json = rules_json->child;

        while (rules_json != NULL) {
                rules_ip = cJSON_GetObjectItem(rules_json, "ip");
                depth = cJSON_GetObjectItem(rules_json, "depth");
                action = cJSON_GetObjectItem(rules_json, "action");

                if (rules_ip == NULL) rte_exit(EXIT_FAILURE, "IP not found/invalid\n");
                if (depth == NULL) rte_exit(EXIT_FAILURE, "Depth not found/invalid\n");
                if (action == NULL) rte_exit(EXIT_FAILURE, "Action not found/invalid\n");

                rules[rule_idx] = (struct fw_rule *) malloc(sizeof(struct fw_rule));
                fw_pkt_parse_ip(rules_ip->valuestring, &rules[rule_idx]->src_ip);
                rules[rule_idx]->depth = depth->valueint;
                rules[rule_idx]->action = action->valueint;
                rules_json = rules_json->next;
                rule_idx++;
        }
        cJSON_Delete(rules_json);

        return rules;
}

