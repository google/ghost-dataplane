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
import common

class Memcached:
    """
            A memcached setup consisting of 1 client and 4 servers.

            User must set up the class attributes and call launch.

    """

    def memcached_client_cmd(self, name="dc-client", cpu_set='56-66'):
        """
                Construct the command to start memcached client.
        """
        cmd = (
            "docker run -dit "
            "--name {} "
            "--net none "
            "--cpuset-cpus {} "
            "-u root "
            "--privileged "
            "cloudsuite/data-caching:client bash"
        ).format(str(name), str(cpu_set))

        return cmd

    def memcached_server_cmd(self, name=None, cpu_set=None,
                             memcached_server_thread=4,
                             memcached_server_mem=4096,
                             memcached_min_obj_sz=550):
        """
                Construct the command to start memcached server.
        """
        cmd = (
            "docker run "
            "--name {} "
            "--net none "
            "--cpuset-cpus {} "
            "-d "
            "cloudsuite/data-caching:server "
            "-t {} "
            "-m {} "
            "-n {} "
        ).format(str(name), str(cpu_set),
                 str(memcached_server_thread),
                 str(memcached_server_mem),
                 str(memcached_min_obj_sz))

        return cmd

    def launch(self, experiment_config):
        """
                Start the docker containers.

                Input:
                        - An array where the experiment config will be appended to.
        """

        # launch the clients.
        client_cmd = \
            self.memcached_client_cmd(cpu_set=self.client_cpu_set,
                                      name=self.client_name)

        memcached_client = \
            docker.start_container(	command=client_cmd,
                                    port_name=self.client_port_name,
                                    name=self.client_name)

        memcached_client['cpu_set'] = self.client_cpu_set
        memcached_client['port'] = self.client_port_name
        experiment_config.append(memcached_client)

        # TODO: update the memcached server file.

        # Make this mutable (memcached servers).
        n_servers = 4

        # launch the servers.
        assert len(self.server_list_cpu_set) == n_servers
        assert len(self.server_list_port_name) == n_servers
        assert len(self.server_list_name) == n_servers

        for i in range(n_servers):
            server_cmd = \
                self.memcached_server_cmd(name=self.server_list_name[i],
                                          cpu_set=self.server_list_cpu_set[i])

            memcached_server = \
                docker.start_container(command=server_cmd,
                                       port_name=self.server_list_port_name[i],
                                       name=self.server_list_name[i])

            memcached_server['cpu_set'] = self.server_list_cpu_set[i]
            memcached_server['port'] = self.server_list_port_name[i]
            experiment_config.append(memcached_server)

            # Setup the connectivity.
            docker.connect(memcached_client, memcached_server)
            docker.dns(memcached_client, memcached_server)

        # Warm up the memcached.
        warmup_cmd = (
            "docker exec -dit {} "
            "/usr/src/memcached/memcached_client/loader -a "
            "/usr/src/memcached/twitter_dataset/twitter_dataset_unscaled -o "
            "/usr/src/memcached/twitter_dataset/twitter_dataset_30x -s "
            "/usr/src/memcached/memcached_client/docker_servers.txt "
            "-w 4 -S 30 -D 4096 -j -T 1"
        ).format(self.client_name)

        common.run_local_cmd(warmup_cmd)

    def __init__(self):

        self.client_cpu_set = None
        self.client_port_name = None
        self.client_name = None

        self.server_list_cpu_set = None
        self.server_list_port_name = None
        self.server_list_name = None


class WebSearch:
    """
            A websearch setup consisting of 1 client and 1 server.

            User must set up the class attributes and call launch.

    """

    def websearch_client_cmd(self, name=None, cpu_set=None):
        """
                Construct the command to start websearch client.
        """
        cmd = (
            "docker run "
            "-dit "
            "--entrypoint /bin/bash "
            "--name {} "
            "--cpuset-cpus {} "
            "--net none "
            "cloudsuite3/web-search:client "
        ).format(str(name), str(cpu_set))

        return cmd

    def websearch_server_cmd(self, name=None, cpu_set=None,
                             webserver_mem=12,
                             n_index_nodes=1):
        """
                Construct the command to start websearch server.
        """
        cmd = (
            "docker run "
            "-dit "
            "--name {} "
            "--net none "
            "--cpuset-cpus {} "
            "cloudsuite3/web-search:server "
            "{}g {}"
        ).format(str(name), str(cpu_set),
                 str(webserver_mem),
                 str(n_index_nodes))

        return cmd

    def launch(self, experiment_config):
        """
                Start the docker containers.

                Input:
                        - An array where the experiment config will be appended to.
        """

        # launch the server.
        server_cmd = \
            self.websearch_server_cmd(name=self.server_name,
                                      cpu_set=self.server_cpu_set)

        websearch_server = \
            docker.start_container(	command=server_cmd,
                                    port_name=self.server_port_name,
                                    name=self.server_name)

        websearch_server['cpu_set'] = self.server_cpu_set
        websearch_server['port'] = self.server_port_name
        experiment_config.append(websearch_server)

        # launch the client.
        client_cmd = \
            self.websearch_client_cmd(cpu_set=self.client_cpu_set,
                                      name=self.client_name)

        websearch_client = \
            docker.start_container(	command=client_cmd,
                                    port_name=self.client_port_name,
                                    name=self.client_name)

        websearch_client['cpu_set'] = self.client_cpu_set
        websearch_client['port'] = self.client_port_name
        experiment_config.append(websearch_client)

        # Setup the connectivity.
        docker.connect(websearch_server, websearch_client)

    def __init__(self):

        self.client_cpu_set = None
        self.client_port_name = None
        self.client_name = None

        self.server_cpu_set = None
        self.server_port_name = None
        self.server_name = None
