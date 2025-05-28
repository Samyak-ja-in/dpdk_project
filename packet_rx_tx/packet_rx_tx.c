#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <signal.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>

#include <rte_eal.h>

#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>


#define MEMPOOL_CACHE_SIZE 256
#define MAX_PKT_BURST 32

static volatile bool force_quit;
struct rte_mempool * pktmbuf_pool = NULL;

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];
static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

static struct rte_eth_conf port_conf = {
        .txmode = {
                .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
};

static unsigned int l2fwd_rx_queue_per_lcore = 1;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
/* List of queues to be polled for a given lcore. 8< */
struct __rte_cache_aligned lcore_queue_conf {
        unsigned n_rx_port;
        unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
};
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS]={1,0};

/* Simple forward. 8< */
static void
app_simple_forward(struct rte_mbuf *m, unsigned portid)
{
        struct rte_ether_hdr *eth;
	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	printf("port_id:%u\n",portid);
	printf("src_ethernet_address:%#x:%#x:%#x:%#x:%#x:%#x\ndst_ethernet_address:%#x:%#x:%#x:%#x:%#x:%#x\nnext_pkt_type:%u\n",eth->src_addr.addr_bytes[0],eth->src_addr.addr_bytes[1],eth->src_addr.addr_bytes[2],eth->src_addr.addr_bytes[3],eth->src_addr.addr_bytes[4],eth->src_addr.addr_bytes[5],eth->dst_addr.addr_bytes[0],eth->dst_addr.addr_bytes[1],eth->dst_addr.addr_bytes[2],eth->dst_addr.addr_bytes[3],eth->dst_addr.addr_bytes[4],eth->dst_addr.addr_bytes[5],eth->ether_type);
	/*
	unsigned dst_port;
        int sent;
        struct rte_eth_dev_tx_buffer *buffer;

        dst_port = l2fwd_dst_ports[portid];

        if (mac_updating)
                l2fwd_mac_updating(m, dst_port);

        buffer = tx_buffer[dst_port];
        sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
	*/
}
/* >8 End of simple forward. */


static void
app_main_loop(void)
{
        struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
        struct rte_mbuf *m;
        int sent;
        unsigned lcore_id;
        uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
        unsigned i, j, portid, nb_rx;
        struct lcore_queue_conf *qconf;
        struct rte_eth_dev_tx_buffer *buffer;

        lcore_id = rte_lcore_id();
        qconf = &lcore_queue_conf[lcore_id];

        if (qconf->n_rx_port == 0) {
                printf("lcore %u has nothing to do\n", lcore_id);
                return;
        }

	for (i = 0; i < qconf->n_rx_port; i++) {

                portid = qconf->rx_port_list[i];
                printf(" -- lcoreid=%u portid=%u\n", lcore_id,
                        portid);

        }

	while (!force_quit) {
		 /* Drains TX queue in its main loop. 8< */
		for (i = 0; i < qconf->n_rx_port; i++) {

                                portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
                                buffer = tx_buffer[portid];

                                sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
                        }
		/* >8 End of draining TX queue. */

		/* Read packet from RX queues. 8< */
                for (i = 0; i < qconf->n_rx_port; i++) {

                        portid = qconf->rx_port_list[i];
                        nb_rx = rte_eth_rx_burst(portid, 0,
                                                 pkts_burst, MAX_PKT_BURST);

                        if (unlikely(nb_rx == 0))
                                continue;

                        for (j = 0; j < nb_rx; j++) {
                                m = pkts_burst[j];
                                rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                                app_simple_forward(m, portid);
                        }
                }
                /* >8 End of read packet from RX queues. */
	}
	
/*	
	// Packet reception loop
    struct rte_mbuf *bufs[BURST_SIZE];
    while (1) {
        const uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);

        if (nb_rx > 0) {
            printf("Received %u packets\n", nb_rx);

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *mbuf = bufs[i];
                // Do something with the packet
                printf("Packet length: %" PRIu16 "\n", rte_pktmbuf_pkt_len(mbuf));
                // Free the packet after processing
                rte_pktmbuf_free(mbuf);
            }
        }
    }
*/	


}

static int
app_launch_one_lcore(__rte_unused void *dummy)
{
        app_main_loop();
        return 0;
}

static void
signal_handler(int signum)
{
        if (signum == SIGINT || signum == SIGTERM) {
                printf("\n\nSignal %d received, preparing to exit...\n",
                                signum);
                force_quit = true;
        }
}


int main(int argc,char** argv){
	struct lcore_queue_conf *qconf;
	int ret;
	uint16_t portid;
	unsigned int nb_mbufs=8192U;
	unsigned lcore_id,rx_lcore_id;

	force_quit = false;
	signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
	
	ret = rte_eal_init(argc, argv);
        if (ret < 0)
                rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

	rx_lcore_id = 0;
        qconf = NULL;

        /* Initialize the port/queue configuration of each logical core */
        RTE_ETH_FOREACH_DEV(portid) {
                /* skip ports that are not enabled */
                /*
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
                        continue;
		*/
                /* get the lcore_id for this port */
                while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
                       lcore_queue_conf[rx_lcore_id].n_rx_port ==
                       l2fwd_rx_queue_per_lcore) {
                        rx_lcore_id++;
                        if (rx_lcore_id >= RTE_MAX_LCORE)
                                rte_exit(EXIT_FAILURE, "Not enough cores\n");
                }

                if (qconf != &lcore_queue_conf[rx_lcore_id]) {
                        /* Assigned a new logical core in the loop above. */
                        qconf = &lcore_queue_conf[rx_lcore_id];
                        //nb_lcores++;
                }

                qconf->rx_port_list[qconf->n_rx_port] = portid;
                qconf->n_rx_port++;
                printf("Lcore %u: RX port %u TX port %u\n", rx_lcore_id,
                       portid, l2fwd_dst_ports[portid]);
        }	


	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
                MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                rte_socket_id());
        if (pktmbuf_pool == NULL)
                rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	
	/* Initialise each port */
        RTE_ETH_FOREACH_DEV(portid) {
                struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
                struct rte_eth_dev_info dev_info;

		/* init port */
                printf("Initializing port %u... ", portid);
                fflush(stdout);
		
		ret = rte_eth_dev_info_get(portid, &dev_info);
                if (ret != 0)
                        rte_exit(EXIT_FAILURE,
                                "Error during getting device (port %u) info: %s\n",
                                portid, strerror(-ret));
		
		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
                        local_port_conf.txmode.offloads |=
                                RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
				
		/* Configure the number of queues for a port. */
                ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                                  ret, portid);
		
		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
                                                       &nb_txd);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE,
                                 "Cannot adjust number of descriptors: err=%d, port=%u\n",
                                 ret, portid);
		
		ret = rte_eth_macaddr_get(portid,
                                          &l2fwd_ports_eth_addr[portid]);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE,
                                 "Cannot get MAC address: err=%d, port=%u\n",
                                 ret, portid);
		
		/* init one RX queue */
                fflush(stdout);
                rxq_conf = dev_info.default_rxconf;
                rxq_conf.offloads = local_port_conf.rxmode.offloads;
                /* RX queue setup. 8< */
                ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                                             rte_eth_dev_socket_id(portid),
                                             &rxq_conf,
                                             pktmbuf_pool);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                                  ret, portid);

			
		/* Init one TX queue on each port. 8< */
                fflush(stdout);
                txq_conf = dev_info.default_txconf;
                txq_conf.offloads = local_port_conf.txmode.offloads;
                ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                rte_eth_dev_socket_id(portid),
                                &txq_conf);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                                ret, portid);
                /* >8 End of init one TX queue on each port. */

                /* Initialize TX buffers */
                tx_buffer[portid] = rte_zmalloc_socket("tx_buffer",
                                RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
                                rte_eth_dev_socket_id(portid));
                if (tx_buffer[portid] == NULL)
                        rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
                                        portid);

                rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		/* Start device */
                ret = rte_eth_dev_start(portid);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                                  ret, portid);

                printf("done: \n");
                //if (promiscuous_on) {
                        ret = rte_eth_promiscuous_enable(portid);
                        if (ret != 0)
                                rte_exit(EXIT_FAILURE,
                                        "rte_eth_promiscuous_enable:err=%s, port=%u\n",
                                        rte_strerror(-ret), portid);
                //}

                printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
                        portid,
                        RTE_ETHER_ADDR_BYTES(&l2fwd_ports_eth_addr[portid]));

	}
	
	rte_eal_mp_remote_launch(app_launch_one_lcore, NULL, CALL_MAIN);
        RTE_LCORE_FOREACH_WORKER(lcore_id) {
                if (rte_eal_wait_lcore(lcore_id) < 0) {
                        ret = -1;
                        break;
                }
        }


	return 0;
}
