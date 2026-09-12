/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_probe.c
 * @brief Measures the real cadence of CIP class 1 I/O on the wire.
 *
 * The question this exists to answer is "is the RPI the link actually
 * achieves the RPI that was asked for", and no log inside either process can
 * answer it: both report what they intended to send. Only a capture at the
 * interface sees what left and what arrived, and when.
 *
 * It is a purpose-built sniffer rather than post-processing a pcap because at
 * a 5 ms RPI an hour of traffic is 1.4 million packets in each direction, and
 * the interesting quantities - inter-arrival distribution, sequence gaps,
 * longest silence - are all computable in one pass. Nothing is stored per
 * packet, so the run length is bounded by patience rather than by disk.
 *
 *   eip_probe <ifname> <seconds> <rpi_us> <timeout_us> [label]
 *
 * Statistics are kept per connection ID, which also means a connection that
 * drops and is reopened shows up as a second block rather than as one long
 * gap - the ForwardOpen count falls out of the same data.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CIP_IO_PORT        2222
#define MAX_CONNS            32
/* 5 us bins to 600 ms. Fine enough to separate a 5 ms RPI from its jitter,
 * wide enough to hold a connection-timeout-sized silence without clipping. */
#define HIST_BIN_US           5
#define HIST_BINS        120000
#define TOP_GAPS             20

typedef struct gap {
    double   at_s;        /* seconds since capture start */
    uint64_t delta_us;
    uint32_t seq_before;
    uint32_t lost;        /* sequence numbers skipped */
} gap_t;

typedef struct conn {
    uint32_t id;
    uint32_t src_ip, dst_ip;
    int      used;

    uint64_t packets;
    uint64_t first_us, last_us;

    uint32_t prev_seq;
    int      have_prev;
    uint64_t prev_us;

    /* Welford, in microseconds. */
    double   mean, m2;
    uint64_t min_us, max_us;

    uint64_t seq_lost;        /* sum of skipped sequence numbers */
    uint64_t seq_gaps;        /* number of discontinuities         */
    uint64_t seq_regress;     /* sequence went backwards           */

    uint64_t late_1_5x, late_2x, late_4x;   /* deltas beyond n x RPI */
    uint64_t over_timeout;                  /* deltas beyond the CIP budget */

    /* Longest run of consecutive *expected* packets that did not arrive,
     * derived from the delta rather than from the sequence number, so a
     * silence still counts even if the peer never numbered the gap. */
    uint64_t max_consecutive_missed;

    uint64_t *hist;
    gap_t     top[TOP_GAPS];
} conn_t;

static conn_t g_conns[MAX_CONNS];
static volatile sig_atomic_t g_stop;

static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static conn_t *conn_for(uint32_t id, uint32_t sip, uint32_t dip) {
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (g_conns[i].used && g_conns[i].id == id) return &g_conns[i];
    }
    for (int i = 0; i < MAX_CONNS; ++i) {
        if (!g_conns[i].used) {
            conn_t *c = &g_conns[i];
            memset(c, 0, sizeof(*c));
            c->used = 1;
            c->id = id;
            c->src_ip = sip;
            c->dst_ip = dip;
            c->min_us = UINT64_MAX;
            c->hist = calloc(HIST_BINS, sizeof(uint64_t));
            if (!c->hist) { perror("calloc"); exit(1); }
            return c;
        }
    }
    return NULL;
}

static void note_gap(conn_t *c, double at_s, uint64_t delta, uint32_t seq, uint32_t lost) {
    /* Keep the worst TOP_GAPS, smallest first so the head is the one to evict. */
    if (c->top[0].delta_us >= delta) return;
    c->top[0].at_s = at_s;
    c->top[0].delta_us = delta;
    c->top[0].seq_before = seq;
    c->top[0].lost = lost;
    for (int i = 1; i < TOP_GAPS; ++i) {
        if (c->top[i].delta_us > c->top[i - 1].delta_us) break;
        gap_t t = c->top[i]; c->top[i] = c->top[i - 1]; c->top[i - 1] = t;
    }
}

static uint64_t pct(const conn_t *c, double p) {
    if (c->packets < 2) return 0;
    const uint64_t n = c->packets - 1;   /* deltas, not packets */
    uint64_t want = (uint64_t)(p * (double)n / 100.0);
    if (want == 0) want = 1;
    uint64_t seen = 0;
    for (int i = 0; i < HIST_BINS; ++i) {
        seen += c->hist[i];
        if (seen >= want) return (uint64_t)i * HIST_BIN_US;
    }
    return (uint64_t)HIST_BINS * HIST_BIN_US;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s <ifname> <seconds> <rpi_us> <timeout_us> [label]\n",
                argv[0]);
        return 2;
    }
    const char    *ifname   = argv[1];
    const uint64_t seconds  = strtoull(argv[2], NULL, 10);
    const uint64_t rpi_us   = strtoull(argv[3], NULL, 10);
    const uint64_t tmo_us   = strtoull(argv[4], NULL, 10);
    const char    *label    = (argc > 5) ? argv[5] : "";

    /* ETH_P_ALL, not ETH_P_IP. Outgoing packets are copied to packet sockets
     * by dev_queue_xmit_nit(), which walks only the ptype_all tap list; a
     * socket bound to a specific protocol sits on ptype_base and therefore
     * sees received packets only. Binding to IP here silently measured one
     * direction of a bidirectional link. */
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket(AF_PACKET)"); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); return 1; }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family   = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex  = ifr.ifr_ifindex;
    if (bind(fd, (struct sockaddr *)&sll, sizeof(sll)) < 0) { perror("bind"); return 1; }

    /* A generous receive buffer: the point of this program is to not be the
     * thing that drops packets. */
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const uint64_t t0 = now_us();
    const uint64_t deadline = t0 + seconds * 1000000u;
    uint64_t total_udp = 0, non_cip = 0;

    unsigned char buf[2048];
    while (!g_stop) {
        const uint64_t nowv = now_us();
        if (nowv >= deadline) break;

        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            perror("recv");
            break;
        }
        const uint64_t ts = now_us();

        if (n < (ssize_t)(sizeof(struct ethhdr) + 20 + 8)) continue;
        const struct ethhdr *eth = (const struct ethhdr *)buf;
        if (ntohs(eth->h_proto) != ETH_P_IP) continue;
        const unsigned char *ip = buf + sizeof(struct ethhdr);
        if ((ip[0] >> 4) != 4) continue;
        const int ihl = (ip[0] & 0x0f) * 4;
        if (ip[9] != IPPROTO_UDP) continue;

        uint32_t sip, dip;
        memcpy(&sip, ip + 12, 4);
        memcpy(&dip, ip + 16, 4);

        const unsigned char *udp = ip + ihl;
        if ((udp - buf) + 8 > n) continue;
        const uint16_t sport = (uint16_t)((udp[0] << 8) | udp[1]);
        const uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
        if (sport != CIP_IO_PORT && dport != CIP_IO_PORT) continue;
        total_udp++;

        const unsigned char *p = udp + 8;
        const ssize_t avail = n - (p - buf);
        /* Common Packet Format: count, then a sequenced address item whose
         * payload is the connection ID and the encapsulation sequence
         * number. That 32-bit counter is what makes loss countable rather
         * than inferred. */
        if (avail < 4 + 8) { non_cip++; continue; }
        const uint16_t item_count = (uint16_t)(p[0] | (p[1] << 8));
        const uint16_t type_id    = (uint16_t)(p[2] | (p[3] << 8));
        const uint16_t item_len   = (uint16_t)(p[4] | (p[5] << 8));
        if (item_count < 2 || type_id != 0x8002 || item_len < 8) { non_cip++; continue; }

        uint32_t conn_id, seq;
        memcpy(&conn_id, p + 6, 4);
        memcpy(&seq,     p + 10, 4);

        conn_t *c = conn_for(conn_id, sip, dip);
        if (!c) continue;

        c->packets++;
        c->last_us = ts;
        if (c->packets == 1) {
            c->first_us = ts;
            c->prev_seq = seq;
            c->prev_us  = ts;
            c->have_prev = 1;
            continue;
        }

        const uint64_t delta = ts - c->prev_us;

        /* Welford. */
        const double d = (double)delta - c->mean;
        c->mean += d / (double)(c->packets - 1);
        c->m2   += d * ((double)delta - c->mean);

        if (delta < c->min_us) c->min_us = delta;
        if (delta > c->max_us) c->max_us = delta;

        uint64_t bin = delta / HIST_BIN_US;
        if (bin >= HIST_BINS) bin = HIST_BINS - 1;
        c->hist[bin]++;

        if (rpi_us) {
            if (delta > rpi_us * 3 / 2) c->late_1_5x++;
            if (delta > rpi_us * 2)     c->late_2x++;
            if (delta > rpi_us * 4)     c->late_4x++;
            const uint64_t missed = delta / rpi_us;
            if (missed > 1 && missed - 1 > c->max_consecutive_missed)
                c->max_consecutive_missed = missed - 1;
        }
        if (tmo_us && delta > tmo_us) c->over_timeout++;

        uint32_t lost = 0;
        if (seq == c->prev_seq + 1) {
            /* in order */
        } else if (seq > c->prev_seq) {
            lost = seq - c->prev_seq - 1;
            c->seq_lost += lost;
            c->seq_gaps++;
        } else {
            c->seq_regress++;
        }

        if (delta > rpi_us * 2 || lost)
            note_gap(c, (double)(ts - t0) / 1e6, delta, c->prev_seq, lost);

        c->prev_seq = seq;
        c->prev_us  = ts;
    }

    const uint64_t elapsed = now_us() - t0;

    printf("=== eip_probe %s ===\n", label);
    printf("interface            : %s\n", ifname);
    printf("capture duration     : %.3f s\n", (double)elapsed / 1e6);
    printf("configured RPI       : %llu us\n", (unsigned long long)rpi_us);
    printf("CIP conn timeout     : %llu us\n", (unsigned long long)tmo_us);
    printf("UDP:2222 packets     : %llu (non-CIP-CPF: %llu)\n",
           (unsigned long long)total_udp, (unsigned long long)non_cip);
    printf("distinct connections : ");
    int nconn = 0;
    for (int i = 0; i < MAX_CONNS; ++i) if (g_conns[i].used) nconn++;
    printf("%d\n\n", nconn);

    for (int i = 0; i < MAX_CONNS; ++i) {
        conn_t *c = &g_conns[i];
        if (!c->used) continue;

        char s[16], d[16];
        struct in_addr a;
        a.s_addr = c->src_ip; snprintf(s, sizeof(s), "%s", inet_ntoa(a));
        a.s_addr = c->dst_ip; snprintf(d, sizeof(d), "%s", inet_ntoa(a));

        const double span = (double)(c->last_us - c->first_us) / 1e6;
        const double sd   = (c->packets > 2)
            ? sqrt(c->m2 / (double)(c->packets - 2)) : 0.0;

        printf("--- connection 0x%08X  %s -> %s ---\n", c->id, s, d);
        printf("  packets            : %llu over %.3f s\n",
               (unsigned long long)c->packets, span);
        if (rpi_us && span > 0) {
            const double expected = span * 1e6 / (double)rpi_us;
            printf("  expected @ RPI     : %.0f  (delivered %.4f%%)\n",
                   expected, 100.0 * (double)c->packets / expected);
        }
        printf("  cycle mean         : %.1f us\n", c->mean);
        printf("  cycle min / max    : %llu / %llu us\n",
               (unsigned long long)(c->min_us == UINT64_MAX ? 0 : c->min_us),
               (unsigned long long)c->max_us);
        printf("  cycle stddev       : %.1f us\n", sd);
        printf("  max jitter (|max-RPI|) : %llu us\n",
               (unsigned long long)(c->max_us > rpi_us ? c->max_us - rpi_us : 0));
        printf("  P50 / P95 / P99    : %llu / %llu / %llu us\n",
               (unsigned long long)pct(c, 50.0),
               (unsigned long long)pct(c, 95.0),
               (unsigned long long)pct(c, 99.0));
        printf("  P99.9 / P99.99     : %llu / %llu us\n",
               (unsigned long long)pct(c, 99.9),
               (unsigned long long)pct(c, 99.99));
        printf("  deltas >1.5xRPI    : %llu\n", (unsigned long long)c->late_1_5x);
        printf("  deltas >2xRPI      : %llu\n", (unsigned long long)c->late_2x);
        printf("  deltas >4xRPI      : %llu\n", (unsigned long long)c->late_4x);
        printf("  deltas >CIP budget : %llu\n", (unsigned long long)c->over_timeout);
        printf("  seq gaps / lost    : %llu / %llu\n",
               (unsigned long long)c->seq_gaps, (unsigned long long)c->seq_lost);
        printf("  seq regressions    : %llu\n", (unsigned long long)c->seq_regress);
        printf("  max consecutive missed : %llu\n",
               (unsigned long long)c->max_consecutive_missed);

        printf("  worst gaps (t=s, delta_us, seq_before, lost):\n");
        for (int k = TOP_GAPS - 1; k >= 0; --k) {
            if (c->top[k].delta_us == 0) continue;
            printf("    %10.3f  %8llu  %10u  %u\n",
                   c->top[k].at_s, (unsigned long long)c->top[k].delta_us,
                   c->top[k].seq_before, c->top[k].lost);
        }
        printf("\n");
    }
    close(fd);
    return 0;
}
