import common
import os

def kni_init(kni_to_deploy):
	"""
		Initialize an individual kni ports in kernel.

		Input:
			- A dict() with keys as port name and values
			  being the IP address.
	"""

	# Iterate over all the kni ports.
	for kni_port, ip_addr_str in kni_to_deploy.items():

		add_dev_command = "ip addr add dev {} {}".format(kni_port, ip_addr_str)
		ifup_command = "ifconfig {} up".format(kni_port)

		# Add device and ifup.
		os.popen(add_dev_command)
		os.popen(ifup_command)


def launch_kni(n_kni_ports = None):
	"""
		Initilize all the kni ports required by containers.

		Input:
			- The number of kni ports.

	"""

	# Verify no ports are instantiated.
	kni_ports_ifconfig = common.get_kni_ports()
	if len(kni_ports_ifconfig) != 0:
		print('Non zero KNI ports. Exiting')
		exit()

	# Identify the port ids / IP to deploy.
	port_name_to_ip_map = dict()
	for i in range(n_kni_ports):
		port_name_to_ip_map = {**port_name_to_ip_map, **common.port_to_ip_mapping(i)}

	# Verify that the ports are unique.
	if len(port_name_to_ip_map.values()) != len(set(port_name_to_ip_map.values())):
		print('IP addresses not unique')
		exit()

	# Start the ports.
	kni_init(port_name_to_ip_map)


def get_number_of_engine_threads():
	"""
		Returns the number of engine threads.
	"""

	command = 'ps aux | grep "./build/engine" \
				| grep -v "grep" | wc -l'

	n_threads = int(os.popen(command).read().strip())

	return n_threads
