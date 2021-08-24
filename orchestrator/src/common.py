import os
import ipaddress
import re

# Global variables
N_KNI_PORTS = 10

# Get host port.
get_port_ip = (lambda port_name : 
				"ifconfig {} | grep \"inet \" | xargs | cut -d \' \' -f 2"
				.format(port_name))
get_port_ether = (lambda port_name : 
				"ifconfig {} | grep \"ether \" | xargs | cut -d \' \' -f 2"
				.format(port_name))

# Get container port.
get_container_port_ip = (lambda server_pid, port_name :
							"ip netns exec {} ifconfig {} | grep \"inet \" | xargs | cut -d \' \' -f 2"
							.format(server_pid, port_name))
get_container_port_ether = (lambda server_pid, port_name :
							"ip netns exec {} ifconfig {} | grep \"ether \" | xargs | cut -d \' \' -f 2"
							.format(server_pid, port_name))

def port_to_ip_mapping(index):
	"""
	A user defined mapping port_id (kni) to ipv4.
	"""
	return {"vEth0_{}".format(index) : "192.167.10.{}".format(index + 1)}


def get_kni_ports():
	"""
	A KNI port is a list of string of format vEth0_%d where %d is the port index.
	"""
	kni_ports = os.popen('ifconfig | grep vEth0_ | cut -d\':\' -f1 ').read().strip().split('\n')
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
