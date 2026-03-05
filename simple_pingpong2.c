#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_log.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#define APP_NAME "simple_pingpong"
#define RTE_LOGTYPE_SIMPLE RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define SEND_BURST_COUNT 8
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
#define MEMPOOL_CACHE_SIZE 256

#define APP_MAGIC 0x50494E47u /* "PING" */
#define APP_ETHER_TYPE 0x88B5
#define DEFAULT_PKT_LEN 64
#define DEFAULT_TX_REPEAT 100
#define DEFAULT_TX_INTERVAL_MS 1000

struct app_payload {
    uint32_t magic;
    uint32_t seq;
} __attribute__((packed));

static volatile bool force_quit;
static uint16_t portid = 0;
static bool tx_mode = false;
static uint32_t tx_repeat = DEFAULT_TX_REPEAT;
static uint32_t tx_interval_ms = DEFAULT_TX_INTERVAL_MS;
static struct rte_ether_addr port_mac;
static const struct rte_ether_addr broadcast_mac = {
    .addr_bytes = {0xA0, 0x36, 0x9F, 0x28, 0xFA, 0x20}
};

static struct rte_mempool *pktmbuf_pool;

static const struct rte_eth_conf port_conf = {
    .rxmode = {0},
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
    },
};

static void log_port_link_and_stats(uint16_t pid, const char *tag)
{
    struct rte_eth_link link;
    struct rte_eth_stats st;
    char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];
    int ret;

    memset(&link, 0, sizeof(link));
    ret = rte_eth_link_get_nowait(pid, &link);
    if (ret == 0) {
        rte_eth_link_to_str(link_status_text, sizeof(link_status_text), &link);
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE, "[%s] Port %u link: %s\n",
                tag, pid, link_status_text);
    } else {
        rte_log(RTE_LOG_WARNING, RTE_LOGTYPE_SIMPLE,
                "[%s] Port %u link query failed: %s\n",
                tag, pid, rte_strerror(-ret));
    }

    ret = rte_eth_stats_get(pid, &st);
    if (ret == 0) {
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
                "[%s] stats ipackets=%" PRIu64 " opackets=%" PRIu64
                " ibytes=%" PRIu64 " obytes=%" PRIu64
                " imissed=%" PRIu64 " ierrors=%" PRIu64 " oerrors=%" PRIu64 "\n",
                tag, st.ipackets, st.opackets, st.ibytes, st.obytes,
                st.imissed, st.ierrors, st.oerrors);
    }
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
                "Signal %d received, preparing to exit\n", signum);
        force_quit = true;
    }
}

static void usage(const char *prog)
{
    printf("%s [EAL options] -- -p PORT [-t] [-r REPEAT] [-i INTERVAL_MS]\n"
           "  -p PORT: DPDK port id (default 0)\n"
           "  -t     : TX mode (default RX mode)\n"
           "  -r N   : TX repeats of 8-packet burst (default 100)\n"
           "  -i MS  : Delay between TX repeats in ms (default 100)\n",
           prog);
}

static int parse_args(int argc, char **argv)
{
    int opt;

    while ((opt = getopt(argc, argv, "p:tr:i:")) != -1) {
        switch (opt) {
        case 'p':
            portid = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 't':
            tx_mode = true;
            break;
        case 'r':
            tx_repeat = (uint32_t)strtoul(optarg, NULL, 10);
            if (tx_repeat == 0)
                tx_repeat = 1;
            break;
        case 'i':
            tx_interval_ms = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        default:
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static void init_port(uint16_t pid)
{
    int ret;
    uint16_t nb_rxd = RX_DESC_DEFAULT;
    uint16_t nb_txd = TX_DESC_DEFAULT;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_rxconf rxq_conf;
    struct rte_eth_txconf txq_conf;
    struct rte_eth_conf local_port_conf = port_conf;

    ret = rte_eth_dev_info_get(pid, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get(port %u) failed: %s\n",
                 pid, rte_strerror(-ret));

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        local_port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    ret = rte_eth_dev_configure(pid, 1, 1, &local_port_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_configure(port %u) failed: %d\n", pid, ret);

    ret = rte_eth_dev_adjust_nb_rx_tx_desc(pid, &nb_rxd, &nb_txd);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_adjust_nb_rx_tx_desc(port %u) failed: %d\n", pid, ret);

    rxq_conf = dev_info.default_rxconf;
    rxq_conf.offloads = local_port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(pid, 0, nb_rxd, rte_eth_dev_socket_id(pid),
                                 &rxq_conf, pktmbuf_pool);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup(port %u) failed: %d\n", pid, ret);

    txq_conf = dev_info.default_txconf;
    txq_conf.offloads = local_port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(pid, 0, nb_txd, rte_eth_dev_socket_id(pid), &txq_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup(port %u) failed: %d\n", pid, ret);

    ret = rte_eth_dev_start(pid);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start(port %u) failed: %d\n", pid, ret);

    ret = rte_eth_macaddr_get(pid, &port_mac);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_macaddr_get(port %u) failed: %s\n",
                 pid, rte_strerror(-ret));

    // ret = rte_eth_promiscuous_enable(pid);
    // if (ret != 0)
    //     rte_log(RTE_LOG_WARNING, RTE_LOGTYPE_SIMPLE,
    //             "promiscuous enable failed on port %u: %s\n", pid, rte_strerror(-ret));

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "Port %u initialized (rxq=1 txq=1, rxd=%u txd=%u)\n",
            pid, nb_rxd, nb_txd);
    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "Port %u MAC: " RTE_ETHER_ADDR_PRT_FMT "\n",
            pid, RTE_ETHER_ADDR_BYTES(&port_mac));
    log_port_link_and_stats(pid, "init");
}

static struct rte_mbuf *build_packet(uint32_t seq)
{
    const uint16_t pkt_len = DEFAULT_PKT_LEN;
    struct rte_mbuf *m;
    struct rte_ether_hdr *eth;
    struct app_payload *pl;
    uint16_t payload_offset = sizeof(*eth);

    m = rte_pktmbuf_alloc(pktmbuf_pool);
    if (m == NULL)
        return NULL;

    char *data = rte_pktmbuf_append(m, pkt_len);
    if (data == NULL) {
        rte_pktmbuf_free(m);
        return NULL;
    }
    memset(data, 0, pkt_len);

    eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    rte_ether_addr_copy(&broadcast_mac, &eth->dst_addr);
    rte_ether_addr_copy(&port_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(APP_ETHER_TYPE);

    pl = (struct app_payload *)((char *)eth + payload_offset);
    pl->magic = rte_cpu_to_be_32(APP_MAGIC);
    pl->seq = rte_cpu_to_be_32(seq);

    m->l2_len = sizeof(*eth);

    return m;
}

static int run_tx(void)
{
    uint64_t total_sent_all = 0;

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "TX mode: %u bursts, each with %u packets, interval=%u ms\n",
            tx_repeat, SEND_BURST_COUNT, tx_interval_ms);

    for (uint32_t round = 0; round < tx_repeat && !force_quit; round++) {
        struct rte_mbuf *tx_pkts[SEND_BURST_COUNT];
        uint16_t total_sent = 0;

        for (uint32_t i = 0; i < SEND_BURST_COUNT; i++) {
            uint32_t seq = (round * SEND_BURST_COUNT) + i;
            tx_pkts[i] = build_packet(seq);
            if (tx_pkts[i] == NULL)
                rte_exit(EXIT_FAILURE, "Failed to build packet seq=%" PRIu32 "\n", seq);
        }

        while (total_sent < SEND_BURST_COUNT && !force_quit) {
            uint16_t sent = rte_eth_tx_burst(portid, 0, &tx_pkts[total_sent],
                                             SEND_BURST_COUNT - total_sent);
            total_sent += sent;

            if (sent == 0)
                rte_delay_us_block(10);
        }

        if (total_sent < SEND_BURST_COUNT)
            for (uint16_t i = total_sent; i < SEND_BURST_COUNT; i++)
                rte_pktmbuf_free(tx_pkts[i]);

        total_sent_all += total_sent;
        rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
                "TX burst %u/%u sent=%u\n", round + 1, tx_repeat, total_sent);

        if (tx_interval_ms > 0 && (round + 1) < tx_repeat)
            rte_delay_ms(tx_interval_ms);
    }

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "TX final total_sent=%" PRIu64 " (requested=%u)\n",
            total_sent_all, tx_repeat * SEND_BURST_COUNT);
    log_port_link_and_stats(portid, "after-tx");
    rte_delay_ms(100);
    return 0;
}

static int run_rx(void)
{
    struct rte_mbuf *rx_pkts[MAX_PKT_BURST];
    uint32_t received = 0;
    uint64_t last_tsc = rte_rdtsc();
    const uint64_t hz = rte_get_tsc_hz();

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "RX mode: waiting for %u packets\n", SEND_BURST_COUNT);

    while (!force_quit && received < SEND_BURST_COUNT) {
        uint64_t now = rte_rdtsc();
        if (now - last_tsc > hz) {
            log_port_link_and_stats(portid, "rx-loop");
            last_tsc = now;
        }

        uint16_t nb_rx = rte_eth_rx_burst(portid, 0, rx_pkts, MAX_PKT_BURST);
        if (nb_rx == 0)
            continue;

        rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_SIMPLE,
                "Received burst nb_rx=%u\n", nb_rx);

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            struct app_payload *pl;
            uint16_t eth_type;
            uint32_t magic;
            uint32_t seq;

            eth_type = rte_be_to_cpu_16(eth->ether_type);
            if (eth_type != APP_ETHER_TYPE) {
                rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_SIMPLE,
                        "Skip packet: ether_type=0x%04x\n", eth_type);
                rte_pktmbuf_free(m);
                continue;
            }

            if (m->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct app_payload)) {
                rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_SIMPLE,
                        "Drop: packet too short len=%u\n", m->pkt_len);
                rte_pktmbuf_free(m);
                continue;
            }

            pl = (struct app_payload *)(eth + 1);
            magic = rte_be_to_cpu_32(pl->magic);
            seq = rte_be_to_cpu_32(pl->seq);
            if (magic != APP_MAGIC) {
                rte_log(RTE_LOG_DEBUG, RTE_LOGTYPE_SIMPLE,
                        "Drop: bad magic=0x%08" PRIx32 "\n", magic);
                rte_pktmbuf_free(m);
                continue;
            }

            rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
                    "RX packet seq=%" PRIu32 " len=%u\n", seq, m->pkt_len);
            received++;
            rte_pktmbuf_free(m);

            if (received >= SEND_BURST_COUNT)
                break;
        }
    }

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "RX done: received=%" PRIu32 "/%u\n", received, SEND_BURST_COUNT);
    log_port_link_and_stats(portid, "after-rx");
    return (received == SEND_BURST_COUNT) ? 0 : -1;
}

int main(int argc, char **argv)
{
    int ret;
    uint16_t nb_ports;
    unsigned int nb_mbufs;

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
    argc -= ret;
    argv += ret;

    if (parse_args(argc, argv) < 0)
        rte_exit(EXIT_FAILURE, "Invalid app arguments\n");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    rte_log_set_global_level(RTE_LOG_DEBUG);
    rte_log_set_level(RTE_LOGTYPE_SIMPLE, RTE_LOG_DEBUG);

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE,
            "Starting %s mode on lcore=%u port=%u\n",
            tx_mode ? "TX" : "RX", rte_lcore_id(), portid);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports available\n");
    if (portid >= nb_ports)
        rte_exit(EXIT_FAILURE, "Invalid port %u (available: 0..%u)\n", portid, nb_ports - 1);

    nb_mbufs = RTE_MAX((unsigned int)(nb_ports *
                      (RX_DESC_DEFAULT + TX_DESC_DEFAULT + MAX_PKT_BURST + MEMPOOL_CACHE_SIZE)),
                      8192U);
    pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs, MEMPOOL_CACHE_SIZE,
                                           0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (pktmbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool: %s\n", rte_strerror(rte_errno));

    init_port(portid);

    ret = tx_mode ? run_tx() : run_rx();

    rte_eth_dev_stop(portid);
    rte_eth_dev_close(portid);
    rte_eal_cleanup();

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_SIMPLE, "Exit ret=%d\n", ret);
    return ret;
}
