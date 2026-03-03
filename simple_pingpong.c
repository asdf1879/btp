#include <arpa/inet.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#define RX_DESC 1024
#define TX_DESC 1024
#define NUM_MBUFS 8192
#define MBUF_CACHE_SIZE 256
#define RX_BURST 32
#define CHUNK_SIZE 8
#define DEFAULT_COUNT 100
#define APP_PROTO_ID 253
#define APP_MAGIC 0xC0DEFACEu
#define REPLY_TIMEOUT_SEC 5

enum app_mode {
    MODE_INVALID = 0,
    MODE_TX,
    MODE_RX
};

struct payload_stamp {
    uint32_t seq;
    uint32_t magic;
} __attribute__((packed));

struct app_config {
    enum app_mode mode;
    uint16_t port_id;
    struct rte_ether_addr peer_mac;
    bool peer_mac_set;
    uint32_t src_ip;
    uint32_t dst_ip;
    bool src_ip_set;
    bool dst_ip_set;
    uint32_t count;
};

static void usage(const char *prog)
{
    printf("Usage:\n");
    printf("  %s [EAL args] -- --mode=tx --peer-mac=aa:bb:cc:dd:ee:ff --src-ip=192.168.1.10 --dst-ip=192.168.1.20 [--port=0] [--count=100]\n", prog);
    printf("  %s [EAL args] -- --mode=rx [--port=0]\n", prog);
}

static int parse_mac(const char *s, struct rte_ether_addr *mac)
{
    unsigned int b[6];

    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return -1;

    for (int i = 0; i < 6; i++) {
        if (b[i] > 0xff)
            return -1;
        mac->addr_bytes[i] = (uint8_t)b[i];
    }

    return 0;
}

static int parse_ipv4(const char *s, uint32_t *out_be)
{
    struct in_addr addr;
    if (inet_aton(s, &addr) == 0)
        return -1;

    *out_be = addr.s_addr;
    return 0;
}

static int parse_app_args(int argc, char **argv, struct app_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->count = DEFAULT_COUNT;
    cfg->port_id = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *val = argv[i] + 7;
            if (strcmp(val, "tx") == 0)
                cfg->mode = MODE_TX;
            else if (strcmp(val, "rx") == 0)
                cfg->mode = MODE_RX;
            else
                return -1;
        } else if (strncmp(argv[i], "--peer-mac=", 11) == 0) {
            if (parse_mac(argv[i] + 11, &cfg->peer_mac) != 0)
                return -1;
            cfg->peer_mac_set = true;
        } else if (strncmp(argv[i], "--src-ip=", 9) == 0) {
            if (parse_ipv4(argv[i] + 9, &cfg->src_ip) != 0)
                return -1;
            cfg->src_ip_set = true;
        } else if (strncmp(argv[i], "--dst-ip=", 9) == 0) {
            if (parse_ipv4(argv[i] + 9, &cfg->dst_ip) != 0)
                return -1;
            cfg->dst_ip_set = true;
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            unsigned long p = strtoul(argv[i] + 7, NULL, 10);
            if (p > UINT16_MAX)
                return -1;
            cfg->port_id = (uint16_t)p;
        } else if (strncmp(argv[i], "--count=", 8) == 0) {
            cfg->count = (uint32_t)strtoul(argv[i] + 8, NULL, 10);
            if (cfg->count == 0)
                return -1;
        } else {
            return -1;
        }
    }

    if (cfg->mode == MODE_TX && (!cfg->peer_mac_set || !cfg->src_ip_set || !cfg->dst_ip_set))
        return -1;

    if (cfg->mode == MODE_INVALID)
        return -1;

    return 0;
}

static int setup_port(uint16_t port_id, struct rte_mempool *mp)
{
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;
    int rc;

    memset(&port_conf, 0, sizeof(port_conf));

    rc = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
    if (rc < 0)
        return rc;

    rc = rte_eth_dev_info_get(port_id, &dev_info);
    if (rc < 0)
        return rc;

    rc = rte_eth_rx_queue_setup(port_id, 0, RX_DESC, rte_eth_dev_socket_id(port_id),
                                &dev_info.default_rxconf, mp);
    if (rc < 0)
        return rc;

    txconf = dev_info.default_txconf;
    rc = rte_eth_tx_queue_setup(port_id, 0, TX_DESC, rte_eth_dev_socket_id(port_id), &txconf);
    if (rc < 0)
        return rc;

    rc = rte_eth_dev_start(port_id);
    if (rc < 0)
        return rc;

    rte_eth_promiscuous_enable(port_id);
    return 0;
}

static void print_mac(const char *label, const struct rte_ether_addr *mac)
{
    printf("%s %02x:%02x:%02x:%02x:%02x:%02x\n", label,
           mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2],
           mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5]);
}

static struct rte_mbuf *build_probe(struct rte_mempool *mp,
                                    const struct rte_ether_addr *src_mac,
                                    const struct rte_ether_addr *dst_mac,
                                    uint32_t src_ip_be,
                                    uint32_t dst_ip_be,
                                    uint32_t seq)
{
    const uint16_t pkt_len = (uint16_t)(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct payload_stamp));
    struct rte_mbuf *m = rte_pktmbuf_alloc(mp);
    if (m == NULL)
        return NULL;

    char *data = rte_pktmbuf_append(m, pkt_len);
    if (data == NULL) {
        rte_pktmbuf_free(m);
        return NULL;
    }

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    struct payload_stamp *stamp = (struct payload_stamp *)(ip + 1);

    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    memset(ip, 0, sizeof(*ip));
    ip->version_ihl = RTE_IPV4_VHL_DEF;
    ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct payload_stamp));
    ip->packet_id = rte_cpu_to_be_16((uint16_t)seq);
    ip->time_to_live = 64;
    ip->next_proto_id = APP_PROTO_ID;
    ip->src_addr = src_ip_be;
    ip->dst_addr = dst_ip_be;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    stamp->seq = seq;
    stamp->magic = APP_MAGIC;
    return m;
}

static int run_receiver(uint16_t port_id)
{
    struct rte_mbuf *rx[RX_BURST];
    struct rte_mbuf *batch[CHUNK_SIZE];
    struct rte_ether_hdr *batch_eth[CHUNK_SIZE];
    struct rte_ipv4_hdr *batch_ip[CHUNK_SIZE];
    uint16_t collected = 0;

    printf("Receiver running on port %u (chunk=%u)\n", port_id, CHUNK_SIZE);

    while (1) {
        uint16_t want = (uint16_t)(CHUNK_SIZE - collected);
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx, RTE_MIN((uint16_t)RX_BURST, want));
        if (nb_rx == 0) {
            rte_pause();
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx[i];
            if (rte_pktmbuf_data_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct payload_stamp)) {
                rte_pktmbuf_free(m);
                continue;
            }

            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(m);
                continue;
            }

            struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
            if (ip->next_proto_id != APP_PROTO_ID || ip->time_to_live <= 1) {
                rte_pktmbuf_free(m);
                continue;
            }

            struct payload_stamp *stamp = (struct payload_stamp *)(ip + 1);
            if (stamp->magic != APP_MAGIC) {
                rte_pktmbuf_free(m);
                continue;
            }

            batch[collected] = m;
            batch_eth[collected] = eth;
            batch_ip[collected] = ip;
            collected++;
        }

        if (collected < CHUNK_SIZE)
            continue;

        for (uint16_t i = 0; i < CHUNK_SIZE; i++) {
            struct rte_ether_addr tmp_mac;
            uint32_t tmp_ip;
            struct rte_ether_hdr *eth = batch_eth[i];
            struct rte_ipv4_hdr *ip = batch_ip[i];

            ip->time_to_live -= 1;
            tmp_ip = ip->src_addr;
            ip->src_addr = ip->dst_addr;
            ip->dst_addr = tmp_ip;
            ip->hdr_checksum = 0;
            ip->hdr_checksum = rte_ipv4_cksum(ip);

            rte_ether_addr_copy(&eth->src_addr, &tmp_mac);
            rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
            rte_ether_addr_copy(&tmp_mac, &eth->dst_addr);
        }

        uint16_t done = 0;
        while (done < CHUNK_SIZE) {
            uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &batch[done], CHUNK_SIZE - done);
            if (nb_tx == 0) {
                rte_pause();
                continue;
            }
            done += nb_tx;
        }

        collected = 0;
    }
}

static int run_sender(uint16_t port_id,
                      struct rte_mempool *mp,
                      const struct rte_ether_addr *my_mac,
                      const struct app_config *cfg)
{
    struct rte_mbuf *out[CHUNK_SIZE];
    struct rte_mbuf *in[RX_BURST];
    uint32_t sent = 0;
    uint32_t received = 0;
    const uint64_t hz = rte_get_tsc_hz();

    printf("Sender target=%u chunk=%u\n", cfg->count, CHUNK_SIZE);
    print_mac("Sender MAC:", my_mac);
    print_mac("Peer MAC:", &cfg->peer_mac);

    while (sent < cfg->count) {
        uint32_t chunk_base = sent;
        uint16_t chunk = (uint16_t)RTE_MIN((uint32_t)CHUNK_SIZE, cfg->count - chunk_base);
        bool seen[CHUNK_SIZE] = {0};
        uint16_t seen_cnt = 0;

        for (uint16_t i = 0; i < chunk; i++) {
            out[i] = build_probe(mp, my_mac, &cfg->peer_mac, cfg->src_ip, cfg->dst_ip, chunk_base + i);
            if (out[i] == NULL) {
                for (uint16_t j = 0; j < i; j++)
                    rte_pktmbuf_free(out[j]);
                printf("[TX] failed to allocate mbufs for chunk base=%u\n", chunk_base);
                return 1;
            }
        }

        uint16_t tx_done = 0;
        while (tx_done < chunk) {
            uint16_t tx = rte_eth_tx_burst(port_id, 0, &out[tx_done], chunk - tx_done);
            if (tx == 0) {
                rte_pause();
                continue;
            }
            tx_done += tx;
        }

        uint64_t start_wait = rte_get_tsc_cycles();
        while (seen_cnt < chunk) {
            if (rte_get_tsc_cycles() - start_wait > (uint64_t)REPLY_TIMEOUT_SEC * hz) {
                printf("[TX] timeout in chunk base=%u received=%u/%u\n", chunk_base, seen_cnt, chunk);
                return 1;
            }

            uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, in, RX_BURST);
            if (nb_rx == 0) {
                rte_pause();
                continue;
            }

            for (uint16_t i = 0; i < nb_rx; i++) {
                struct rte_mbuf *m = in[i];
                if (rte_pktmbuf_data_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct payload_stamp)) {
                    rte_pktmbuf_free(m);
                    continue;
                }

                struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                    rte_pktmbuf_free(m);
                    continue;
                }

                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                if (ip->next_proto_id != APP_PROTO_ID) {
                    rte_pktmbuf_free(m);
                    continue;
                }

                struct payload_stamp *stamp = (struct payload_stamp *)(ip + 1);
                if (stamp->magic != APP_MAGIC) {
                    rte_pktmbuf_free(m);
                    continue;
                }

                if (stamp->seq >= chunk_base && stamp->seq < (chunk_base + chunk)) {
                    uint16_t idx = (uint16_t)(stamp->seq - chunk_base);
                    if (!seen[idx]) {
                        seen[idx] = true;
                        seen_cnt++;
                        received++;
                    }
                }

                rte_pktmbuf_free(m);
            }
        }

        sent += chunk;
        printf("[TX] completed chunk base=%u size=%u total=%u/%u\n", chunk_base, chunk, sent, cfg->count);
    }

    printf("Done sent=%u received=%u lost=%u\n", sent, received, sent - received);
    return 0;
}

int main(int argc, char **argv)
{
    struct app_config cfg;
    int eal_argc;
    uint16_t port_id;
    struct rte_mempool *mp;
    struct rte_ether_addr my_mac;
    int rc;

    eal_argc = rte_eal_init(argc, argv);
    if (eal_argc < 0)
        rte_exit(EXIT_FAILURE, "EAL init failed\n");

    argc -= eal_argc;
    argv += eal_argc;

    if (parse_app_args(argc, argv, &cfg) != 0) {
        usage("./build/pingpong-simple");
        rte_eal_cleanup();
        return 1;
    }

    if (rte_eth_dev_count_avail() == 0)
        rte_exit(EXIT_FAILURE, "No DPDK ports available\n");

    port_id = cfg.port_id;
    if (!rte_eth_dev_is_valid_port(port_id))
        rte_exit(EXIT_FAILURE, "Invalid port_id=%u\n", port_id);

    mp = rte_pktmbuf_pool_create("pingpong_pool", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                                 RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mp == NULL)
        rte_exit(EXIT_FAILURE, "Mempool creation failed: %s\n", rte_strerror(rte_errno));

    rc = setup_port(port_id, mp);
    if (rc < 0)
        rte_exit(EXIT_FAILURE, "Port setup failed: %s (%d)\n", rte_strerror(-rc), rc);

    if (rte_eth_macaddr_get(port_id, &my_mac) != 0)
        rte_exit(EXIT_FAILURE, "Failed to get MAC address\n");

    print_mac("Local port MAC:", &my_mac);

    if (cfg.mode == MODE_RX)
        rc = run_receiver(port_id);
    else
        rc = run_sender(port_id, mp, &my_mac, &cfg);

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();
    return rc;
}
