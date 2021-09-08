import common
import os


def port_to_ip_mapping(index):
    """
    A user defined mapping port_id (kni) to ipv4.
    """
    return {"vEth0_{}".format(index): "192.167.10.{}".format(index + 1)}


def set_interface_ports(kni_to_deploy):
    """
            Initialize kni ports in kernel.

            Input:
                    - A dict() with keys as port name and values
                      being the IP address.
    """

    # Iterate over all the kni ports.
    for kni_port, ip_addr_str in kni_to_deploy.items():

        add_dev_command = "ip addr add dev {} {}".format(kni_port, ip_addr_str)
        ifup_command = "ifconfig {} up".format(kni_port)

        # Add device and ifup.
        common.run_local_cmd(add_dev_command)
        common.run_local_cmd(ifup_command)


def setup_virtual_tcp_interface(n_kni_ports=None):
    """
            Initilize all the kni ports required by containers.

            Input:
                    - The number of kni ports.

            Each virtual port is a DPDK kernel network interface
            port for the purposes of providing a TCP stack.

            A TCP stack is necessary for retransmission functionality.
    """

    # Verify no ports are instantiated.
    kni_ports_ifconfig = common.get_kni_ports()
    if len(kni_ports_ifconfig) != 0:
        print('Non zero KNI ports. Exiting')
        exit()

    # Identify the port ids / IP to deploy.
    port_name_to_ip_map = dict()
    for i in range(n_kni_ports):
        port_name_to_ip_map = {**port_name_to_ip_map, **port_to_ip_mapping(i)}

    # Verify that the ports are unique.
    if len(port_name_to_ip_map.values()) != len(set(port_name_to_ip_map.values())):
        print('IP addresses not unique')
        exit()

    # Start the ports.
    set_interface_ports(port_name_to_ip_map)
