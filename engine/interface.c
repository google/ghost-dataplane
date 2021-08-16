#include "interface.h"
#include "init.h"


/* Options for configuring ethernet port */
static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = ETH_MQ_RX_RSS,
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
        .split_hdr_size = 0,
        .offloads = DEV_RX_OFFLOAD_CHECKSUM,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = ETH_RSS_TCP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

/* Initializes a single port */
void
init_phy_port(uint16_t port)
{
	int ret;
	uint16_t nb_rxd = NB_RXD;
	uint16_t nb_txd = NB_TXD;

	/* Initialise device and RX/TX queues */
	RTE_LOG(INFO, APP, "Initialising port %u ...\n", (unsigned)port);
	fflush(stdout);
	ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not configure port%u (%d)\n",
		            (unsigned)port, ret);

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not adjust number of descriptors "
				"for port%u (%d)\n", (unsigned)port, ret);

	ret = rte_eth_rx_queue_setup(port, 0, nb_rxd,
		rte_eth_dev_socket_id(port), NULL, pktmbuf_pool);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not setup up RX queue for "
				"port%u (%d)\n", (unsigned)port, ret);

	ret = rte_eth_tx_queue_setup(port, 0, nb_txd,
		rte_eth_dev_socket_id(port), NULL);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not setup up TX queue for "
				"port%u (%d)\n", (unsigned)port, ret);

	ret = rte_eth_dev_start(port);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Could not start port%u (%d)\n",
						(unsigned)port, ret);

	rte_eth_promiscuous_enable(port);
}


/* Check the link status of all ports in up to 9s, and print them finally */
void
check_port_link_status(uint16_t portid)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t count, port_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		port_up = 1;
		
		memset(&link, 0, sizeof(link));
		rte_eth_link_get_nowait(portid, &link);
		/* print link status if flag set */
		if (print_flag == 1) {
			if (link.link_status)
				printf(
				"Port%d Link Up - speed %uMbps - %s\n",
					portid, link.link_speed,
			(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
				("full-duplex") : ("half-duplex\n"));
			else
				printf("Port %d Link Down\n", portid);
		}

		/* clear ports_up flag if any link down */
		if (print_flag == 0 && link.link_status == ETH_LINK_DOWN) {
			port_up = 0;
		}
		
		/* after finally printing all link status, get out */
		if (print_flag == 1){
			break;
		}

		if (port_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (port_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

/* Callback for request of configuring network interface up/down */
static int
kni_config_network_interface(uint16_t port_id, uint8_t if_up)
{
	
	RTE_LOG(INFO, APP, "Configure network interface of %d %s\n",
					port_id, if_up ? "up" : "down");

	if (if_up != 0) { /* Configure network interface up */
		printf("IFUP %u\n", (unsigned int)port_id);
	} else /* Configure network interface down */
		printf("IFDOWN %u\n", (unsigned int)port_id);
		
	return 0;
}

static int
kni_alloc(uint16_t nb_kni, uint16_t port_id)
{
	uint8_t i;
	struct rte_kni *kni;
	struct rte_kni_conf conf;
	// TODO: Get the config from the network.
	// For now all KNIs will be running on 0.
	uint16_t lcore_k = 0;
	
	for (i = 0; i < nb_kni; i++) {
		/* Clear conf at first */
		memset(&conf, 0, sizeof(conf));
		snprintf(conf.name, RTE_KNI_NAMESIZE, "vEth%u_%u", port_id, i);
		conf.core_id = lcore_k;
		conf.force_bind = 1;
		conf.group_id = port_id;
		conf.mbuf_size = MAX_PACKET_SZ;

		struct rte_kni_ops ops;
		if (i == 0) {
			memset(&ops, 0, sizeof(ops));
			ops.port_id = i; //port_id
			//ops.change_mtu = kni_change_mtu;
			ops.config_network_if = kni_config_network_interface;

			kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);
		} else{
			memset(&ops, 0, sizeof(ops));
			ops.port_id = i;
			//ops.change_mtu = kni_change_mtu;
			ops.config_network_if = kni_config_network_interface;
			kni = rte_kni_alloc(pktmbuf_pool, &conf, &ops);
		}

		if (!kni)
			rte_exit(EXIT_FAILURE, "Fail to create kni for "
						"port: %d\n", port_id);
		kni_list[i] = kni;
	}

	return 0;
}




void
init_kni_port(uint16_t port_id)
{
	uint16_t nb_kni = KNI_KTHREAD;

	rte_kni_init(nb_kni);
	kni_alloc(nb_kni, port_id);

}

int
kni_free_kni(void)
{
	uint8_t i;
	uint16_t nb_kni = KNI_KTHREAD;

	for (i = 0; i < nb_kni; i++) {
		if (rte_kni_release(kni_list[i]))
			printf("Fail to release kni\n");
		kni_list[i] = NULL;
	}

	return 0;
}
