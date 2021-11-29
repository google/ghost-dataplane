# 
# Copyright 2021 Google LLC
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#     https://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# 

import docker
import ports
import os
import pandas as pd
import time
import cloudsuite
import common

def get_number_of_engine_threads():
    """
        Returns the number of engine threads.
    """

    command = 'ps aux | grep "./build/engine" | grep -v "grep" | wc -l'
    n_threads = int(common.run_local_cmd(command, get_output = True))
    return n_threads


def main():
    # Script config:
    n_kni_ports = 10
    experiment_path = '../data/experiment.txt'

    # Check if there is at least one engine thread.
    n_engine_threads = get_number_of_engine_threads()
    if n_engine_threads < 1:
        print("Engine thread not running")
        exit()

    # Setup the KNI ports in kernel.
    ports.setup_virtual_tcp_interface(n_kni_ports)

    # Check if there are no containers already running.
    n_docker = docker.number_of_running_processes()
    while n_docker > 0:
        print("Non zero #containers.")
        print("Stop and remove all containers by running ../prune.sh")
        exit()

    experiment_config = list()

    memcached_exp = cloudsuite.Memcached()
    memcached_exp.client_cpu_set = '0,56-86'
    memcached_exp.client_port_name = 'vEth0_0'
    memcached_exp.client_name = "dc-client"

    memcached_exp.server_list_cpu_set = ['1,12-14','2,15-17','3,18-20','4,21-23']
    memcached_exp.server_list_port_name = ['vEth0_{}'.format(i) for i in range(1,5)]
    memcached_exp.server_list_name = ["dc-server{}".format(i) for i in range(1,5)]
    memcached_exp.launch(experiment_config)

    websearch_exp = cloudsuite.WebSearch()
    websearch_exp.client_cpu_set = '6'
    websearch_exp.client_port_name = 'vEth0_6'
    websearch_exp.client_name = "client"
    websearch_exp.server_cpu_set = '7,24-26'
    websearch_exp.server_port_name = 'vEth0_5'
    websearch_exp.server_name = "server"
    websearch_exp.launch(experiment_config)


    pd.DataFrame(experiment_config).to_csv(experiment_path)

if __name__ == "__main__":
    main()
