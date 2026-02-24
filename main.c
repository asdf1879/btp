#include <arpa/inet.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <immintrin.h>

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
#define BURST_SIZE 32
#define IO_CHUNK 8
#define DEFAULT_COUNT 100
#define APP_PROTO_ID 253
#define TX_REPLY_TIMEOUT_SEC 5
#define TX_CHUNK_MAX_RETRY 10
#define TTL_DEBUG_FIRST_N 8

enum app_mode {
    MODE_INVALID = 0,
    MODE_TX,
    MODE_FWD
};

struct payload_stamp {
    uint64_t tsc_send;
    uint32_t seq;
    uint32_t magic;
} __attribute__((packed));

struct app_config {
    enum app_mode mode;
    uint16_t port_id;
    bool ttl_simd;
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
    printf("  %s [EAL args] -- --mode=tx [--port=0] --peer-mac=aa:bb:cc:dd:ee:ff --src-ip=192.168.1.10 --dst-ip=192.168.1.20 [--count=100]\n", prog);
    printf("  %s [EAL args] -- --mode=fwd [--port=0] [--ttl-mode=scalar|simd]\n", prog);
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
    cfg->ttl_simd = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--mode=", 7) == 0) {
            const char *val = argv[i] + 7;
            if (strcmp(val, "tx") == 0)
                cfg->mode = MODE_TX;
            else if (strcmp(val, "fwd") == 0)
                cfg->mode = MODE_FWD;
            else
                return -1;
        } else if (strncmp(argv[i], "--peer-mac=", 11) == 0) {
            if (parse_mac(argv[i] + 11, &cfg->peer_mac) != 0)
                return -1;
            cfg->peer_mac_set = true;
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            unsigned long p = strtoul(argv[i] + 7, NULL, 10);
            if (p > UINT16_MAX)
                return -1;
            cfg->port_id = (uint16_t)p;
        } else if (strncmp(argv[i], "--src-ip=", 9) == 0) {
            if (parse_ipv4(argv[i] + 9, &cfg->src_ip) != 0)
                return -1;
            cfg->src_ip_set = true;
        } else if (strncmp(argv[i], "--dst-ip=", 9) == 0) {
            if (parse_ipv4(argv[i] + 9, &cfg->dst_ip) != 0)
                return -1;
            cfg->dst_ip_set = true;
        } else if (strncmp(argv[i], "--count=", 8) == 0) {
            cfg->count = (uint32_t)strtoul(argv[i] + 8, NULL, 10);
            if (cfg->count == 0)
                return -1;
        } else if (strncmp(argv[i], "--ttl-mode=", 11) == 0) {
            const char *val = argv[i] + 11;
            if (strcmp(val, "simd") == 0)
                cfg->ttl_simd = true;
            else if (strcmp(val, "scalar") == 0)
                cfg->ttl_simd = false;
            else
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
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

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

static void print_port_stats(uint16_t port_id, const char *tag)
{
    struct rte_eth_stats st;
    if (rte_eth_stats_get(port_id, &st) != 0)
        return;

    printf("[%s] port=%u ipackets=%" PRIu64 " opackets=%" PRIu64
           " imissed=%" PRIu64 " ierrors=%" PRIu64 " oerrors=%" PRIu64 "\n",
           tag, port_id, st.ipackets, st.opackets, st.imissed, st.ierrors, st.oerrors);
}

static struct rte_mbuf *build_ipv4_probe(struct rte_mempool *mp,
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

    stamp->tsc_send = rte_get_tsc_cycles();
    stamp->seq = seq;
    stamp->magic = 0xC0DEFACEu;

    return m;
}

static inline void ttl_decrement_simd(struct rte_ipv4_hdr **ips, uint16_t n)
{
#if defined(__SSE2__)
    uint8_t ttl_in[16] = {0};
    uint8_t ttl_out[16] = {0};
    const __m128i one = _mm_set1_epi8(1);

    for (uint16_t i = 0; i < n; i++)
        ttl_in[i] = ips[i]->time_to_live;

    __m128i v = _mm_loadu_si128((const __m128i *)ttl_in);
    __m128i r = _mm_subs_epu8(v, one);
    _mm_storeu_si128((__m128i *)ttl_out, r);

    for (uint16_t i = 0; i < n; i++)
        ips[i]->time_to_live = ttl_out[i];
#else
    for (uint16_t i = 0; i < n; i++)
        ips[i]->time_to_live -= 1;
#endif
}

static int run_forwarder(uint16_t port_id, const struct app_config *cfg)
{
    struct rte_mbuf *rx_pkts[IO_CHUNK];
    struct rte_mbuf *tx_pkts[IO_CHUNK];
    struct rte_ether_hdr *valid_eth[IO_CHUNK];
    struct rte_ipv4_hdr *valid_ip[IO_CHUNK];
    uint8_t ttl_before_arr[IO_CHUNK];
    uint64_t rx_total = 0;
    uint64_t tx_total = 0;
    uint64_t drop_total = 0;
    uint32_t ttl_debug_printed = 0;
    uint64_t last_stats_tsc = rte_get_tsc_cycles();
    const uint64_t hz = rte_get_tsc_hz();

    printf("Forwarder running on port %u (chunk=%d, ttl_mode=%s)\n",
           port_id, IO_CHUNK, cfg->ttl_simd ? "simd" : "scalar");

    while (1) {
        uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_pkts, IO_CHUNK);
        if (nb_rx == 0) {
            uint64_t now = rte_get_tsc_cycles();
            if (now - last_stats_tsc >= hz) {
                printf("[FWD] rx=%" PRIu64 " tx=%" PRIu64 " drop=%" PRIu64 "\n", rx_total, tx_total, drop_total);
                print_port_stats(port_id, "FWD");
                last_stats_tsc = now;
            }
            continue;
        }

        rx_total += nb_rx;
        uint16_t valid = 0;

        for (uint16_t i = 0; i < nb_rx; i++) {
            struct rte_mbuf *m = rx_pkts[i];
            if (rte_pktmbuf_data_len(m) < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
                rte_pktmbuf_free(m);
                drop_total++;
                continue;
            }

            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                rte_pktmbuf_free(m);
                drop_total++;
                continue;
            }

            struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
            if (ip->time_to_live <= 1) {
                rte_pktmbuf_free(m);
                drop_total++;
                continue;
            }

            valid_eth[valid] = eth;
            valid_ip[valid] = ip;
            ttl_before_arr[valid] = ip->time_to_live;
            tx_pkts[valid++] = m;
        }

        if (valid > 0) {
            if (cfg->ttl_simd)
                ttl_decrement_simd(valid_ip, valid);
            else {
                for (uint16_t i = 0; i < valid; i++)
                    valid_ip[i]->time_to_live -= 1;
            }

            for (uint16_t i = 0; i < valid; i++) {
                struct rte_ether_addr tmp_mac;
                uint32_t tmp_ip;
                struct rte_ether_hdr *eth = valid_eth[i];
                struct rte_ipv4_hdr *ip = valid_ip[i];

                tmp_ip = ip->src_addr;
                ip->src_addr = ip->dst_addr;
                ip->dst_addr = tmp_ip;
                ip->hdr_checksum = 0;
                ip->hdr_checksum = rte_ipv4_cksum(ip);

                if (ttl_debug_printed < TTL_DEBUG_FIRST_N) {
                    printf("[FWD-TTL] seq=%u ttl %u -> %u\n",
                           rte_be_to_cpu_16(ip->packet_id), ttl_before_arr[i], ip->time_to_live);
                    ttl_debug_printed++;
                }

                rte_ether_addr_copy(&eth->src_addr, &tmp_mac);
                rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
                rte_ether_addr_copy(&tmp_mac, &eth->dst_addr);
            }
        }

        uint16_t done = 0;
        while (done < valid) {
            uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &tx_pkts[done], valid - done);
            if (nb_tx == 0) {
                rte_pause();
                continue;
            }
            done += nb_tx;
            tx_total += nb_tx;
        }
    }
}

static int run_sender(uint16_t port_id,
                      struct rte_mempool *mp,
                      const struct rte_ether_addr *my_mac,
                      const struct app_config *cfg)
{
    struct rte_mbuf *out[IO_CHUNK];
    struct rte_mbuf *in[BURST_SIZE];
    uint32_t sent = 0;
    uint32_t received = 0;
    uint64_t last_progress_tsc = rte_get_tsc_cycles();
    uint64_t first_tx_tsc = 0;
    uint64_t last_rx_tsc = 0;
    uint32_t ttl_debug_printed = 0;
    const uint64_t hz = rte_get_tsc_hz();

    printf("Sender target=%u, lockstep chunk=%d\n", cfg->count, IO_CHUNK);
    print_mac("Sender MAC:", my_mac);
    print_mac("Peer MAC:", &cfg->peer_mac);

    while (sent < cfg->count) {
        uint32_t chunk_base = sent;
        uint16_t chunk = (uint16_t)RTE_MIN((uint32_t)IO_CHUNK, cfg->count - chunk_base);
        bool acked[IO_CHUNK] = {0};
        uint16_t acked_cnt = 0;
        uint16_t retry = 0;

        while (acked_cnt < chunk && retry < TX_CHUNK_MAX_RETRY) {
            uint16_t built = 0;

            for (uint16_t i = 0; i < chunk; i++) {
                if (acked[i]) {
                    continue;
                }
                struct rte_mbuf *m = build_ipv4_probe(mp, my_mac, &cfg->peer_mac, cfg->src_ip, cfg->dst_ip, chunk_base + i);
                if (m == NULL) {
                    break;
                }
                out[built++] = m;
            }

            if (built == 0) {
                usleep(1000);
                continue;
            }

            uint16_t tx_done = 0;
            while (tx_done < built) {
                uint16_t tx = rte_eth_tx_burst(port_id, 0, &out[tx_done], built - tx_done);
                if (tx == 0) {
                    rte_pause();
                    continue;
                }
                if (first_tx_tsc == 0)
                    first_tx_tsc = rte_get_tsc_cycles();
                tx_done += tx;
            }
            last_progress_tsc = rte_get_tsc_cycles();

            uint64_t wait_start = last_progress_tsc;
            while (acked_cnt < chunk) {
                uint64_t now = rte_get_tsc_cycles();
                if ((now - wait_start) > (uint64_t)TX_REPLY_TIMEOUT_SEC * hz) {
                    retry++;
                    break;
                }

                uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, in, BURST_SIZE);
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
                    if (stamp->magic != 0xC0DEFACEu) {
                        rte_pktmbuf_free(m);
                        continue;
                    }

                    if (stamp->seq >= chunk_base && stamp->seq < (chunk_base + chunk)) {
                        uint16_t idx = (uint16_t)(stamp->seq - chunk_base);
                        if (!acked[idx]) {
                            acked[idx] = true;
                            acked_cnt++;
                            received++;
                            last_rx_tsc = rte_get_tsc_cycles();
                            if (ttl_debug_printed < TTL_DEBUG_FIRST_N) {
                                printf("[TX-TTL] reply seq=%u ttl=%u\n", stamp->seq, ip->time_to_live);
                                ttl_debug_printed++;
                            }
                        }
                    }
                    last_progress_tsc = rte_get_tsc_cycles();
                    rte_pktmbuf_free(m);
                }
            }
        }

        if (acked_cnt < chunk) {
            printf("[TX] failed chunk base=%u acked=%u/%u after %u retries\n",
                   chunk_base, acked_cnt, chunk, TX_CHUNK_MAX_RETRY);
            break;
        }

        sent += chunk;
    }

    printf("Completed: sent=%u/%u received=%u/%u lost=%u\n",
           sent, cfg->count, received, cfg->count, sent - received);

    if (first_tx_tsc != 0 && last_rx_tsc >= first_tx_tsc) {
        uint64_t delta_tsc = last_rx_tsc - first_tx_tsc;
        double duration_s = (double)delta_tsc / (double)hz;
        double pps = duration_s > 0.0 ? ((double)received / duration_s) : 0.0;
        printf("Perf: first_tx_tsc=%" PRIu64 " last_rx_tsc=%" PRIu64
               " duration=%.6f s throughput=%.2f pps\n",
               first_tx_tsc, last_rx_tsc, duration_s, pps);
    } else {
        printf("Perf: insufficient data to compute duration/throughput\n");
    }

    return 0;
}

static unsigned int next_pow2_u32(unsigned int x)
{
    if (x <= 1)
        return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1;
}

static unsigned int calc_mempool_size(const struct app_config *cfg)
{
    unsigned int min_pkts = NUM_MBUFS * 2;
    if (cfg->count + IO_CHUNK > min_pkts)
        min_pkts = cfg->count + IO_CHUNK + 1024;
    return next_pow2_u32(min_pkts) - 1;
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
        usage("./build/receiver");
        rte_eal_cleanup();
        return 1;
    }

    if (rte_eth_dev_count_avail() == 0)
        rte_exit(EXIT_FAILURE, "No DPDK ports available\n");

    port_id = cfg.port_id;
    if (!rte_eth_dev_is_valid_port(port_id))
        rte_exit(EXIT_FAILURE, "Invalid port_id=%u\n", port_id);

    char port_name[RTE_ETH_NAME_MAX_LEN] = {0};
    if (rte_eth_dev_get_name_by_port(port_id, port_name) == 0)
        printf("Using DPDK port_id=%u name=%s\n", port_id, port_name);

    unsigned int pool_n = calc_mempool_size(&cfg);
    printf("Using mempool size=%u\n", pool_n);

    mp = rte_pktmbuf_pool_create("pkt_pool", pool_n, MBUF_CACHE_SIZE, 0,
                                 RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mp == NULL)
        rte_exit(EXIT_FAILURE, "Mempool creation failed: %s\n", rte_strerror(rte_errno));

    rc = setup_port(port_id, mp);
    if (rc < 0)
        rte_exit(EXIT_FAILURE, "Port setup failed: %s (%d)\n", rte_strerror(-rc), rc);

    if (rte_eth_macaddr_get(port_id, &my_mac) != 0)
        rte_exit(EXIT_FAILURE, "Failed to get MAC address\n");

    print_mac("Local port MAC:", &my_mac);

    if (cfg.mode == MODE_FWD)
        rc = run_forwarder(port_id, &cfg);
    else
        rc = run_sender(port_id, mp, &my_mac, &cfg);

    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_eal_cleanup();
    return rc;
}
