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

import common
import ports
import os
import subprocess
import time

await_seconds = 1  # Async invocations must be verfied (or used) after a delay.


def setup_container_port(docker_pid=None, port_name=None, port_ip_addr=None):
    """
            Push the vport to the container namespace.
    """
    # Push the KNI port to namespace.

    common.run_local_cmd(
        "ip link set {} netns {}".format(port_name, docker_pid))
    time.sleep(await_seconds)
    common.run_local_cmd(
        "ip netns exec {} ifconfig {} up".format(docker_pid, port_name))
    time.sleep(await_seconds)
    common.run_local_cmd("ip netns exec {} ifconfig {} {}".format(
        docker_pid, port_name, port_ip_addr))
    time.sleep(await_seconds)
    common.run_local_cmd(
        "ip netns exec {} ifconfig {} promisc".format(docker_pid, port_name))
    time.sleep(await_seconds)


def setup_network_namespace(docker_pid=None, port_name=None):
    """
            Setup the network namespace for the docker container.
    """

    # Namespace configuration.
    proc_filepath = "/proc/{}/ns/net".format(docker_pid)
    netns_filepath = "/var/run/netns/{}".format(docker_pid)

    # Proc dir wont exist if the container is not running.
    if not os.path.isfile(proc_filepath):
        print('proc pid dir does not exist. {}'.format(proc_filepath))
        exit()

    # Create a symbolic link.
    common.run_local_cmd("ln -sf {} {}".format(proc_filepath, netns_filepath))
    # Wait for linking to be successful.
    time.sleep(await_seconds)

    # Check if the netns is correctly setup.
    if not os.path.isfile(netns_filepath):
        print('netns pid dir does not exist. {}'.format(netns_filepath))
        exit()

    # Verify that the KNI port (exposed by DPDK) is up.
    kni_ports = common.get_kni_ports()
    if port_name not in kni_ports:
        print('KNI {} not up'.format(port_name))
        exit()

    # Get port ip and ethernet address.
    port_ip_addr = common.run_local_cmd(
        common.get_port_ip(port_name), get_output=True)
    port_eth_addr = common.run_local_cmd(
        common.get_port_ether(port_name), get_output=True)

    # Verify port is setup with a valid ip and ethernet address.
    if not common.is_ipv4(port_ip_addr):
        print('Port {} does not have an assigned IP addr')
        exit()

    if not common.is_mac(port_eth_addr):
        print('Port {} does not have an assigned ether addr')
        exit()

    setup_container_port(docker_pid=docker_pid,
                         port_name=port_name, port_ip_addr=port_ip_addr)

    return port_eth_addr, port_ip_addr


def start_container(command=None, port_name=None, name=None):
    """
            Start the docker containers. Setup the network namespace.
    """

    docker_pid_cmd = "%s %s" % ("docker inspect -f {{.State.Pid}}", name)

    print('Docker command: {}'.format(command))

    # Start the docker.
    common.run_local_cmd(command)

    # Wait for container to run.
    time.sleep(await_seconds)

    docker_pid = common.run_local_cmd(docker_pid_cmd, get_output=True)
    # If there is no such container with this 'name', then this will error out.
    docker_pid = int(docker_pid)

    port_eth_addr, port_ip_addr = setup_network_namespace(
        docker_pid=docker_pid, port_name=port_name)

    # Get the container IP address and Ethernet address.
    container_port_ip_addr = common.run_local_cmd(
        common.get_container_port_ip(str(docker_pid), port_name), get_output=True)
    container_port_eth_addr = common.run_local_cmd(
        common.get_container_port_ether(str(docker_pid), port_name), get_output=True)

    # Verify the correctness of the port and ethernet addr.
    if container_port_ip_addr != port_ip_addr:
        print('Incorrect IP within container: Container {}, Host {}'
              .format(container_port_ip_addr, port_ip_addr))
        print(common.get_container_port_ip(str(docker_pid), port_name))
        exit()

    if container_port_eth_addr != port_eth_addr:
        print('Incorrect Ether within container: Container {}, Host {}'
              .format(container_port_eth_addr, port_eth_addr))
        print(common.get_container_port_ether(str(docker_pid), port_name))
        exit()

    # Store the configuration.
    ret = {
        'name': name,
        'netns': docker_pid,
        'pid': docker_pid,
        'ip_addr': container_port_ip_addr,
        'eth_addr': container_port_eth_addr,
        'port_name': port_name,
        'command': command
    }

    return ret


def connect(node_a, node_b):
    """
            Setup the IP route and ARP table in the containers.

            Inputs: 
                    - node_a : Container 'a' conf.
                    - node_b : Container 'b' conf.

            node_a and node_b are interchangable.
    """

    route_a2b = ("ip netns exec {} ip route add {} dev {}"
                 .format(node_a['netns'],
                         node_b['ip_addr'],
                         node_a['port_name']))
    route_b2a = ("ip netns exec {} ip route add {} dev {}"
                 .format(node_b['netns'],
                         node_a['ip_addr'],
                         node_b['port_name']))
    arp_a2b = ("ip netns exec {} arp -s {} {}"
               .format(node_a['netns'],
                       node_b['ip_addr'],
                       node_b['eth_addr']))
    arp_b2a = ("ip netns exec {} arp -s {} {}"
               .format(node_b['netns'],
                       node_a['ip_addr'],
                       node_a['eth_addr']))

    common.run_local_cmd(route_a2b)
    common.run_local_cmd(route_b2a)
    common.run_local_cmd(arp_a2b)
    common.run_local_cmd(arp_b2a)


def dns(node_this, node_other):
    """
            Setup the DNS in 'node_this' so that 'node_other'
            can be reached by name (e.g., resolved) instead of IP.

            Input:
                    - node_a: Container whose DNS is to be updated.
                    - node_b: Container that should be reachable.
    """
    command = ("docker exec -u root -it {} bash -c \"echo \'{} {}\' >> /etc/hosts\""
               .format(node_this['name'], node_other['ip_addr'], node_other['name']))
    os.popen(command)
    time.sleep(await_seconds)


def number_of_running_processes():
    """
            Return the count of running containers.
    """
    n_docker = common.run_local_cmd('expr $(docker ps -a | wc -l) - 1', get_output=True)
    return int(n_docker)


def stop_all_docker_containers():
    """
            Stop all containers.
    """
    common.run_local_cmd('docker stop $(docker ps -a -q)')
    time.sleep(await_seconds)


def remove_all_docker_containers():
    """
            Remove all containers.
    """
    common.run_local_cmd('docker rm $(docker ps -a -q)')
    time.sleep(await_seconds)
