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

#include "nfs/firewall/firewall.h"

//#define DISABLE_NF 1

#define ENABLE_FW_NF 1

int
process_packet(struct rte_mbuf *pkt);

int
coprocessor_teardown(void);

int
coprocessor_setup(void);

int num_rules;
