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

import os
import ipaddress
import re

# Global variables
N_KNI_PORTS = 10


# Get container port.
get_container_port_ip = (lambda server_pid, port_name :
							"ip netns exec {} ifconfig {} | grep \"inet \" | xargs | cut -d \' \' -f 2"
							.format(server_pid, port_name))
get_container_port_ether = (lambda server_pid, port_name :
							"ip netns exec {} ifconfig {} | grep \"ether \" | xargs | cut -d \' \' -f 2"
							.format(server_pid, port_name))

def get_port_ip(port_name):
        """
        Get Host port IP.

        Input:
            - The name of the port (e.g, as shown in ifconfig)
        """
        return "ifconfig {} | grep \"inet \" | xargs | cut -d \' \' -f 2".format(port_name)

def get_port_ether(port_name):
        """
        Get Host port Ethernet Address.

        Input:
            - The name of the port (e.g, as shown in ifconfig)
        """
        return "ifconfig {} | grep \"ether \" | xargs | cut -d \' \' -f 2".format(port_name)

def get_container_port_ip(server_pid, port_name):
        """
        Get the container port IP.

        Input:
            - The container pid.
            - The name of the port (e.g, as shown in ifconfig)
        """
        return "ip netns exec {} ifconfig {} | grep \"inet \" | xargs | cut -d \' \' -f 2".format(server_pid, port_name)

def get_container_port_ether(server_pid, port_name):
        """
        Get the container port Ethernet Address.

        Input:
            - The container pid.
            - The name of the port (e.g, as shown in ifconfig)
        """
        return "ip netns exec {} ifconfig {} | grep \"ether \" | xargs | cut -d \' \' -f 2".format(server_pid, port_name)


def get_kni_ports():
	"""
	A KNI port is a list of string of format vEth0_%d where %d is the port index.
	"""
	kni_ports = run_local_cmd('ifconfig | grep vEth0_ | cut -d\':\' -f1 ', get_output = True).split('\n')
	return set([port for port in kni_ports if port != ''])

def is_ipv4(string):
    try:
        ipaddress.IPv4Network(string)
        return True
    except ValueError:
        return False

def is_mac(string):
	if re.match("[0-9a-f]{2}([-:])[0-9a-f]{2}(\\1[0-9a-f]{2}){4}$", string.lower()):
		return True
	else:
		return False


def run_local_cmd(cmd, get_output = False):
        """
        Runs a local command. Optionally, return the cmd output.

        Input:
            - cmd: Command to run.
            - get_output: whether to return the output of the cmd.

        """
        print( "Local cmd $ %s" % cmd )

        if not get_output:
            os.system( cmd )
            return

        output = os.popen( cmd ).read().strip()

        return output
