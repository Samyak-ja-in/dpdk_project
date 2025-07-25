#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_pdump.h>

static volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating = 1;

/* Ports set in promiscuous mode off by default. */
static int promiscuous_on;

/* Filtering packets is off by default */
static int filtering_packets = 0;

/* smartfwd_rx_queue_per_lcore indicates number of rx queue handled by each lcore */
static unsigned int smartfwd_rx_queue_per_lcore = 1;

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

#define RTE_LOGTYPE_SMARTFWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 256
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

#define SUBNET_BASE    RTE_IPV4(17, 0, 0, 0)
#define SUBNET_MASK    RTE_IPV4(255, 0, 0, 0)

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

/* list of ports */
static uint32_t smartfwd_dst_ports[RTE_MAX_ETHPORTS];

/* ethernet addresses of ports */
static struct rte_ether_addr smartfwd_ports_eth_addr[RTE_MAX_ETHPORTS];

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

struct __rte_cache_aligned lcore_queue_conf {
        unsigned n_rx_port;
        unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
};
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

/* Memory pool for packet rx tx */
struct rte_mempool * smartfwd_pktmbuf_pool = NULL;

static struct rte_eth_conf port_conf = {
        .txmode = {
                .mq_mode = RTE_ETH_MQ_TX_NONE,
        },
};

/* Per-port statistics struct */
struct __rte_cache_aligned smartfwd_port_statistics {
        uint64_t tx;
        uint64_t rx;
        uint64_t dropped;
};
struct smartfwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

static void
signal_handler(int signum)
{
        if (signum == SIGINT || signum == SIGTERM) {
                printf("\n\nSignal %d received, preparing to exit...\n",
                                signum);
                force_quit = true;
        }
}

/* Print out statistics on packets dropped */
static void
print_stats(void)
{
        uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
        unsigned portid;

        total_packets_dropped = 0;
        total_packets_tx = 0;
        total_packets_rx = 0;

        const char clr[] = { 27, '[', '2', 'J', '\0' };
        const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

                /* Clear screen and move to top left */
        printf("%s%s", clr, topLeft);

        printf("\nPort statistics ====================================");

        for (portid = 0; portid < 2; portid++) {
                printf("\nStatistics for port %u ------------------------------"
                           "\nPackets sent: %24"PRIu64
                           "\nPackets received: %20"PRIu64
                           "\nPackets dropped: %21"PRIu64,
                           portid,
                           port_statistics[portid].tx,
                           port_statistics[portid].rx,
                           port_statistics[portid].dropped);

                total_packets_dropped += port_statistics[portid].dropped;
                total_packets_tx += port_statistics[portid].tx;
                total_packets_rx += port_statistics[portid].rx;
        }
        printf("\nAggregate statistics ==============================="
                   "\nTotal packets sent: %18"PRIu64
                   "\nTotal packets received: %14"PRIu64
                   "\nTotal packets dropped: %15"PRIu64,
                   total_packets_tx,
                   total_packets_rx,
                   total_packets_dropped);
        printf("\n====================================================\n");

        fflush(stdout);
}

static inline void
print_ether_addr(const char *what, struct rte_ether_addr *eth_addr)
{
        char buf[RTE_ETHER_ADDR_FMT_SIZE];
        rte_ether_format_addr(buf, RTE_ETHER_ADDR_FMT_SIZE, eth_addr);
        printf("%s%s\n", what, buf);
}

static void
smartfwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
        struct rte_ether_hdr *eth;
        void *tmp;

        eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	//print_ether_addr("src_addr:",&eth->src_addr);

        /* 02:00:00:00:00:xx */
        tmp = &eth->dst_addr.addr_bytes[0];
        *((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);
        *((uint64_t *)tmp) = 0x25fea923cef4;

        /* src addr */
        rte_ether_addr_copy(&smartfwd_ports_eth_addr[dest_portid], &eth->src_addr);

	//print_ether_addr("src_addr:",&eth->src_addr);
	//print_ether_addr("dst_addr:",&eth->dst_addr);
}

/**
 * Returns : 1 if packet needs to be dropped
 * 	     0 if packet needs to be forwarded 
 */
static int
smartfwd_filter_packets(struct rte_mbuf *mbuf){
	struct rte_ether_hdr *eth_hdr;
    	struct rte_ipv4_hdr *ip_hdr;

    	eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    	if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        	return 0;

    	ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    	uint32_t src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);

    	if ((src_ip & SUBNET_MASK) == SUBNET_BASE) {
        	return 0;  
    	}
	else
	// Drop the packet
    	return 1;	

}

/* Simple forward. 8< */
static void
smartfwd_simple_forward(struct rte_mbuf *m, unsigned portid)
{
        unsigned dst_port;
        int sent;
        struct rte_eth_dev_tx_buffer *buffer;
	int is_drop = 0;

        dst_port = smartfwd_dst_ports[portid];

        if (mac_updating)
                smartfwd_mac_updating(m, dst_port);
	
	if(filtering_packets)
		is_drop = smartfwd_filter_packets(m);
	
	if(is_drop)
	{
		rte_pktmbuf_free(m);
		port_statistics[portid].dropped += 1;
	}
	else{
        	buffer = tx_buffer[dst_port];
        	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
        	if (sent)
                	port_statistics[dst_port].tx += sent;
	}
}
/* >8 End of simple forward. */

static void
smartfwd_main_loop(void)
{
        struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
        struct rte_mbuf *m;
        int sent;
        unsigned lcore_id;
        uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
        unsigned i, j, portid, nb_rx;
        struct lcore_queue_conf *qconf;
        const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
                        BURST_TX_DRAIN_US;
        struct rte_eth_dev_tx_buffer *buffer;

        prev_tsc = 0;
        timer_tsc = 0;

        lcore_id = rte_lcore_id();
        qconf = &lcore_queue_conf[lcore_id];

        if (qconf->n_rx_port == 0) {
                RTE_LOG(INFO, SMARTFWD, "lcore %u has nothing to do\n", lcore_id);
                return;
        }

        RTE_LOG(INFO, SMARTFWD, "entering main loop on lcore %u\n", lcore_id);
        for (i = 0; i < qconf->n_rx_port; i++) {

                portid = qconf->rx_port_list[i];
                RTE_LOG(INFO, SMARTFWD, " -- lcoreid=%u portid=%u\n", lcore_id,
                        portid);

        }

        while (!force_quit) {

                /* Drains TX queue in its main loop. 8< */
                cur_tsc = rte_rdtsc();

                /*
                 * TX burst queue drain
                 */
                diff_tsc = cur_tsc - prev_tsc;
                if (unlikely(diff_tsc > drain_tsc)) {

                        for (i = 0; i < qconf->n_rx_port; i++) {

                                portid = smartfwd_dst_ports[qconf->rx_port_list[i]];
                                buffer = tx_buffer[portid];

                                sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
                                if (sent)
                                        port_statistics[portid].tx += sent;

                        }

                        /* if timer is enabled */
                        if (timer_period > 0) {

                                /* advance the timer */
                                timer_tsc += diff_tsc;

                                /* if timer has reached its timeout */
                                if (unlikely(timer_tsc >= timer_period)) {

                                        /* do this only on main core */
                                        if (lcore_id == rte_get_main_lcore()) {
                                                print_stats();
                                                /* reset the timer */
                                                timer_tsc = 0;
                                        }
                                }
                        }

                        prev_tsc = cur_tsc;
                }
                /* >8 End of draining TX queue. */

                /* Read packet from RX queues. 8< */
                for (i = 0; i < qconf->n_rx_port; i++) {

                        portid = qconf->rx_port_list[i];
                        nb_rx = rte_eth_rx_burst(portid, 0,
                                                 pkts_burst, MAX_PKT_BURST);

                        if (unlikely(nb_rx == 0))
                                continue;

                        port_statistics[portid].rx += nb_rx;

                        for (j = 0; j < nb_rx; j++) {
                                m = pkts_burst[j];
                                rte_prefetch0(rte_pktmbuf_mtod(m, void *));
                                smartfwd_simple_forward(m, portid);
                        }
                }
                /* >8 End of read packet from RX queues. */
        }
}


static int
smartfwd_launch_one_lcore(__rte_unused void *dummy)
{
        smartfwd_main_loop();
        return 0;
}

/* display usage */
static void
smartfwd_usage(const char *prgname)
{
        printf("%s [EAL options] -- [-P] [-q NQ]\n"
               "  -P : Enable promiscuous mode\n"
               "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
               "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  -F Filtering: If ip packet filtering is enabled(Default Disabled)"
               "  --no-mac-updating: Disable MAC addresses updating (enabled by default)\n"
               "      When enabled:\n"
               "       - The source MAC address is replaced by the TX port MAC address\n"
               "       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n\n",
               prgname);
}


static unsigned int
smartfwd_parse_nqueue(const char *q_arg)
{
        char *end = NULL;
        unsigned long n;

        /* parse hexadecimal string */
        n = strtoul(q_arg, &end, 10);
        if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
                return 0;
        if (n == 0)
                return 0;
        if (n >= MAX_RX_QUEUE_PER_LCORE)
                return 0;

        return n;
}

static int
smartfwd_parse_timer_period(const char *q_arg)
{
        char *end = NULL;
        int n;

        /* parse number string */
        n = strtol(q_arg, &end, 10);
        if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
                return -1;
        if (n >= MAX_TIMER_PERIOD)
                return -1;

        return n;
}

static const char short_options[] =
        "P"   /* promiscuous */
        "q:"  /* number of queues */
        "T:"  /* timer period */
	"F"   /* IP Packet Filtering Enabled */
        ;

#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"

enum {
        /* long options mapped to a short option */
        /* first long only option value must be >= 256, so that we won't
         * conflict with short options */
        CMD_LINE_OPT_NO_MAC_UPDATING_NUM = 256,
};

static const struct option lgopts[] = {
        { CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, 0,
                CMD_LINE_OPT_NO_MAC_UPDATING_NUM},
        {NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
smartfwd_parse_args(int argc, char **argv)
{
        int opt, ret, timer_secs;
        char **argvopt;
        int option_index;
        char *prgname = argv[0];

        argvopt = argv;

        while ((opt = getopt_long(argc, argvopt, short_options,
                                  lgopts, &option_index)) != EOF) {

                switch (opt) {
                case 'P':
                        promiscuous_on = 1;
                        break;
                
                case 'q':
                        smartfwd_rx_queue_per_lcore = smartfwd_parse_nqueue(optarg);
                        if (smartfwd_rx_queue_per_lcore == 0) {
                                printf("invalid queue number\n");
                                smartfwd_usage(prgname);
                                return -1;
                        }
                        break;

                case 'T':
                        timer_secs = smartfwd_parse_timer_period(optarg);
                        if (timer_secs < 0) {
                                printf("invalid timer period\n");
                                smartfwd_usage(prgname);
                                return -1;
                        }
                        timer_period = timer_secs;
                        break;

		case 'F':
			filtering_packets = 1;

                case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
                        mac_updating = 0;
                        break;

                default:
                        smartfwd_usage(prgname);
                        return -1;
                }
        }

        if (optind >= 0)
                argv[optind-1] = prgname;

        ret = optind-1;
        optind = 1; /* reset getopt lib */
        return ret;
}			


int
main(int argc, char **argv)
{
        struct lcore_queue_conf *qconf;
        int ret;
        uint16_t nb_ports;
        uint16_t nb_ports_available = 0;
        uint16_t portid, last_port;
        unsigned lcore_id, rx_lcore_id;
        unsigned nb_ports_in_mask = 0;
        unsigned int nb_lcores = 0;
        unsigned int nb_mbufs;
	
	ret = rte_eal_init(argc, argv);
        if (ret < 0)
                rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
        argc -= ret;
        argv += ret;
	
	force_quit = false;
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);
	
	ret = smartfwd_parse_args(argc, argv);
        if (ret < 0)
                rte_exit(EXIT_FAILURE, "Invalid smartforward arguments\n");

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

        /* convert to number of cycles rte_get_timer_hz() returns number of ticks per sec*/
        timer_period *= rte_get_timer_hz();

        nb_ports = rte_eth_dev_count_avail();
        if (nb_ports == 0)
                rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	
	/* reset smartfwd_dst_ports */
        for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
                smartfwd_dst_ports[portid] = 0;
        last_port = 0;
        
	RTE_ETH_FOREACH_DEV(portid) {
                if (nb_ports_in_mask % 2) {
                        smartfwd_dst_ports[portid] = last_port;
                        smartfwd_dst_ports[last_port] = portid;
                } else {
                        last_port = portid;
                }

                nb_ports_in_mask++;
        }
        if (nb_ports_in_mask % 2) {
                printf("Notice: odd number of ports in portmask.\n");
                smartfwd_dst_ports[last_port] = last_port;
        }
	
        rx_lcore_id = 0;
        qconf = NULL;

	/* Initialize the lcore/port_queue configuration of each logical core */
        RTE_ETH_FOREACH_DEV(portid) {
                /* get the lcore_id for this port */
                while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
                       lcore_queue_conf[rx_lcore_id].n_rx_port ==
                       smartfwd_rx_queue_per_lcore) {
                        rx_lcore_id++;
                        if (rx_lcore_id >= RTE_MAX_LCORE)
                                rte_exit(EXIT_FAILURE, "Not enough cores\n");
                }

                if (qconf != &lcore_queue_conf[rx_lcore_id]) {
                        /* Assigned a new logical core in the loop above. */
                        qconf = &lcore_queue_conf[rx_lcore_id];
                        nb_lcores++;
                }

                qconf->rx_port_list[qconf->n_rx_port] = portid;
                qconf->n_rx_port++;
                printf("Lcore %u: RX port %u TX port %u\n", rx_lcore_id,
                       portid, smartfwd_dst_ports[portid]);
        }

	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
                nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

        /* Create the mbuf pool. 8< */
        smartfwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
                MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
                rte_socket_id());
        if (smartfwd_pktmbuf_pool == NULL)
                rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

        /* Initialise each port */
        RTE_ETH_FOREACH_DEV(portid) {
                struct rte_eth_rxconf rxq_conf;
                struct rte_eth_txconf txq_conf;
                struct rte_eth_conf local_port_conf = port_conf;
                struct rte_eth_dev_info dev_info;

                nb_ports_available++;

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
                /* >8 End of configuration of the number of queues for a port. */

                ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
                                                       &nb_txd);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE,
                                 "Cannot adjust number of descriptors: err=%d, port=%u\n",
                                 ret, portid);

                ret = rte_eth_macaddr_get(portid,
                                          &smartfwd_ports_eth_addr[portid]);
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
                                             smartfwd_pktmbuf_pool);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
                                  ret, portid);
                /* >8 End of RX queue setup. */

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

                ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
                                rte_eth_tx_buffer_count_callback,
                                &port_statistics[portid].dropped);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE,
                        "Cannot set error callback for tx buffer on port %u\n",
                                 portid);

                ret = rte_eth_dev_set_ptypes(portid, RTE_PTYPE_UNKNOWN, NULL,
                                             0);
                if (ret < 0)
                        printf("Port %u, Failed to disable Ptype parsing\n",
                                        portid);
                /* Start device */
                ret = rte_eth_dev_start(portid);
                if (ret < 0)
                        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
                                  ret, portid);

                printf("done: \n");
                if (promiscuous_on) {
                        ret = rte_eth_promiscuous_enable(portid);
                        if (ret != 0)
                                rte_exit(EXIT_FAILURE,
                                        "rte_eth_promiscuous_enable:err=%s, port=%u\n",
                                        rte_strerror(-ret), portid);
                }

                printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
                        portid,
                        RTE_ETHER_ADDR_BYTES(&smartfwd_ports_eth_addr[portid]));

                /* initialize port stats */
                memset(&port_statistics, 0, sizeof(port_statistics));
	}

	rte_pdump_init();

        ret = 0;
        /* launch per-lcore init on every lcore */
        rte_eal_mp_remote_launch(smartfwd_launch_one_lcore, NULL, CALL_MAIN);
        RTE_LCORE_FOREACH_WORKER(lcore_id) {
                if (rte_eal_wait_lcore(lcore_id) < 0) {
                        ret = -1;
                        break;
                }
        }

        RTE_ETH_FOREACH_DEV(portid) {
                printf("Closing port %d...", portid);
                ret = rte_eth_dev_stop(portid);
                if (ret != 0)
                        printf("rte_eth_dev_stop: err=%d, port=%d\n",
                               ret, portid);
                rte_eth_dev_close(portid);
                printf(" Done\n");
        }

        /* clean up the EAL */
        rte_eal_cleanup();
        printf("Bye...\n");

        return ret;
}
	
