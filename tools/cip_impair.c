/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file cip_impair.c
 * @brief A userspace wire between two namespaces that can damage what crosses it.
 *
 * This kernel is built without CONFIG_NET_SCH_NETEM, so `tc qdisc add ... netem`
 * fails and the three impairments that matter most to a fieldbus - delay,
 * jitter and reordering - had to be recorded as "not measured". iptables can
 * drop a packet and nothing else: it cannot hold one back, and holding one
 * back is the whole mechanism behind reordering.
 *
 * So the wire becomes a process. Run it in a middle namespace holding one veth
 * end from each side; it forwards frames between them with AF_PACKET, and on
 * the way through it can delay them, hold one back so a later one passes
 * first, duplicate them, replay an old one, truncate them, or corrupt a byte.
 * That covers reordering (B1c), duplicate and rolled-back sequence numbers
 * (B3), and malformed frames (B4) from one place.
 *
 *   cip_impair <if_a> <if_b> <seconds> [key=value ...]
 *
 *   delay_us=N        hold every CIP I/O frame this long
 *   jitter_us=N       add 0..N uniformly to the delay
 *   reorder_pct=N     hold this share of frames by an extra reorder_gap_us
 *   reorder_gap_us=N  how far back a reordered frame is pushed (default 2x RPI)
 *   dup_pct=N         send this share of frames twice
 *   replay_pct=N      re-send a frame kept from ~replay_age frames ago
 *   replay_age=N      how many frames back the replayed copy comes from
 *   drop_pct=N        drop this share
 *   trunc_pct=N       cut this share short at a random point inside the payload
 *   corrupt_pct=N     flip one random payload byte in this share
 *   seed=N            make a run repeatable
 *
 * Percentages are applied to CIP class 1 I/O frames only (UDP 2222). ARP, TCP
 * 44818 and everything else is forwarded untouched, so a test can damage the
 * I/O path while leaving session management alone - the distinction B1b and B2
 * are built on.
 *
 * One thread per direction, each with its own pending queue, so a frame held
 * in one direction never delays the other.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CIP_IO_PORT   2222
#define MAX_FRAME     2048
#define PENDING_MAX    512
#define REPLAY_KEEP     64

typedef struct {
    uint32_t delay_us, jitter_us;
    uint32_t reorder_pct, reorder_gap_us;
    uint32_t dup_pct, replay_pct, replay_age;
    uint32_t drop_pct, trunc_pct, corrupt_pct;
} knobs_t;

typedef struct {
    uint64_t due_us;
    uint32_t len;
    uint8_t  data[MAX_FRAME];
} pending_t;

typedef struct {
    int      rx_fd, tx_fd;
    int      tx_ifindex;
    char     name[64];
    knobs_t  k;
    unsigned seed;
    /* counters, read by main after the threads stop */
    uint64_t seen, io_seen, forwarded, dropped, delayed, reordered;
    uint64_t duplicated, replayed, truncated, corrupted;
    uint64_t tx_err;
    int      last_errno;
    uint64_t max_hold_us;
} dir_t;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* Not for anything but choosing which packets to damage. */
static uint32_t rnd(unsigned *seed, uint32_t bound) {
    return bound ? (uint32_t)(rand_r(seed) % bound) : 0;
}
static int hits(unsigned *seed, uint32_t pct) {
    return pct && rnd(seed, 100) < pct;
}

/**
 * Is this a CIP class 1 I/O frame?
 *
 * Only those are damaged. Matching on the UDP port rather than on anything in
 * the CIP payload keeps this honest about what it is doing to the link: the
 * test says "the I/O path is impaired", not "some frames were impaired".
 */
static int is_cip_io(const uint8_t *f, uint32_t len, uint32_t *payload_off,
                     uint32_t *payload_len) {
    if (len < 14 + 20 + 8) return 0;
    if (((uint16_t)f[12] << 8 | f[13]) != ETH_P_IP) return 0;
    const uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4) return 0;
    const uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4u;
    if (ip[9] != IPPROTO_UDP || len < 14 + ihl + 8) return 0;
    const uint8_t *udp = ip + ihl;
    const uint16_t sport = (uint16_t)(udp[0] << 8 | udp[1]);
    const uint16_t dport = (uint16_t)(udp[2] << 8 | udp[3]);
    if (sport != CIP_IO_PORT && dport != CIP_IO_PORT) return 0;
    *payload_off = 14 + ihl + 8;
    /* From the UDP header, not from the frame: a short datagram is padded up
     * to 60 bytes and damaging the padding would damage nothing at all. */
    const uint32_t udp_len = (uint32_t)(udp[4] << 8 | udp[5]);
    const uint32_t body = (udp_len > 8) ? udp_len - 8 : 0;
    const uint32_t on_wire = (len > *payload_off) ? len - *payload_off : 0;
    *payload_len = (body && body <= on_wire) ? body : on_wire;
    return 1;
}

/**
 * Recompute the header checksums of a frame we are about to re-transmit.
 *
 * A packet captured on the sending side has not necessarily had its checksum
 * filled in: with offload enabled the kernel hands the NIC - or the veth peer
 * - a partial sum and lets the hardware finish it. Re-transmitting that byte
 * for byte puts a wrong checksum on the wire, and the receiving stack drops
 * the frame without a word. That failure is indistinguishable from "the peer
 * never answered", which is exactly the thing these tests measure, so it has
 * to be fixed here rather than worked around per case.
 *
 * Doing it properly also makes the malformed cases meaningful: a frame with a
 * bit flipped in its CIP payload should be delivered and rejected by the CIP
 * parser, not discarded two layers below it.
 */
static uint16_t ones_complement(const uint8_t *p, uint32_t n, uint32_t start) {
    uint32_t sum = start;
    for (uint32_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1u) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static void fix_checksums(uint8_t *f, uint32_t len) {
    if (len < 14 + 20) return;
    if (((uint16_t)f[12] << 8 | f[13]) != ETH_P_IP) return;
    uint8_t *ip = f + 14;
    if ((ip[0] >> 4) != 4) return;
    const uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4u;
    if (ihl < 20 || len < 14 + ihl) return;

    /* Ethernet pads anything below 60 bytes, so the captured frame is often
     * longer than the datagram inside it - deriving the IP length from the
     * frame length would hand the padding to the receiver as payload. That
     * silently corrupted every small packet, the TCP session handshake
     * included, and looked exactly like the malformed-input failure this
     * harness exists to find.
     *
     * So the header's own length is authoritative, and is only overridden
     * when the frame is too short to hold what it claims - which is what
     * truncation deliberately does. */
    const uint32_t avail   = len - 14;
    const uint32_t claimed = (uint32_t)(ip[2] << 8 | ip[3]);
    const uint32_t ip_len  = (claimed >= ihl && claimed <= avail) ? claimed : avail;
    ip[2] = (uint8_t)(ip_len >> 8);
    ip[3] = (uint8_t)(ip_len & 0xFF);
    ip[10] = ip[11] = 0;
    const uint16_t ipck = ones_complement(ip, ihl, 0);
    ip[10] = (uint8_t)(ipck >> 8);
    ip[11] = (uint8_t)(ipck & 0xFF);

    const uint32_t l4_len = ip_len - ihl;
    uint8_t *l4 = ip + ihl;
    const uint8_t proto = ip[9];
    if (proto != IPPROTO_UDP && proto != IPPROTO_TCP) return;
    if (l4_len < 8) return;

    if (proto == IPPROTO_UDP) {
        l4[4] = (uint8_t)(l4_len >> 8);
        l4[5] = (uint8_t)(l4_len & 0xFF);
        l4[6] = l4[7] = 0;
    } else {
        if (l4_len < 20) return;
        l4[16] = l4[17] = 0;
    }

    /* Pseudo-header: source, destination, protocol, L4 length. */
    uint32_t pseudo = 0;
    for (int i = 12; i < 20; i += 2) pseudo += (uint32_t)(ip[i] << 8 | ip[i + 1]);
    pseudo += proto;
    pseudo += l4_len;
    while (pseudo >> 16) pseudo = (pseudo & 0xFFFFu) + (pseudo >> 16);

    uint16_t ck = ones_complement(l4, l4_len, pseudo);
    /* On IPv4 a zero UDP checksum means "not computed", so the one value that
     * cannot be written is the one that would say that. */
    if (proto == IPPROTO_UDP && ck == 0) ck = 0xFFFFu;
    const int off = (proto == IPPROTO_UDP) ? 6 : 16;
    l4[off]     = (uint8_t)(ck >> 8);
    l4[off + 1] = (uint8_t)(ck & 0xFF);
}

static void tx(dir_t *d, const uint8_t *buf, uint32_t len) {
    uint8_t frame[MAX_FRAME];
    if (len > sizeof(frame)) return;
    memcpy(frame, buf, len);
    fix_checksums(frame, len);

    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family   = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex  = d->tx_ifindex;
    sa.sll_halen    = ETH_ALEN;
    memcpy(sa.sll_addr, frame, ETH_ALEN);
    if (sendto(d->tx_fd, frame, len, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        /* A send that fails while the far end is down is the point of some of
         * these tests, not an error in the harness - but it is never allowed
         * to be invisible, or a wire that forwards nothing reads exactly like
         * a peer that answers nothing. */
        d->tx_err++;
        d->last_errno = errno;
    }
}

static void *pump(void *arg) {
    dir_t *d = arg;
    pending_t *q = calloc(PENDING_MAX, sizeof(*q));
    uint32_t qn = 0;
    /* A small ring of recent frames, so a replay can be a frame that really
     * crossed this wire rather than something synthesised. */
    pending_t *keep = calloc(REPLAY_KEEP, sizeof(*keep));
    uint32_t keep_n = 0, keep_w = 0;
    uint8_t buf[MAX_FRAME];

    while (!g_stop) {
        /* Anything due goes out first, so a held frame leaves on time even
         * when the link has gone quiet. */
        const uint64_t t = now_us();
        for (uint32_t i = 0; i < qn; ) {
            if (q[i].due_us <= t) {
                tx(d, q[i].data, q[i].len);
                d->forwarded++;
                q[i] = q[--qn];
            } else {
                ++i;
            }
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200 };
        fd_set r;
        FD_ZERO(&r);
        FD_SET(d->rx_fd, &r);
        if (select(d->rx_fd + 1, &r, NULL, NULL, &tv) <= 0) continue;

        /* An ETH_P_ALL socket also sees what this process transmits on the
         * interface it is bound to. Forwarding those back would put every
         * frame into a loop between the two sockets; PACKET_OUTGOING is how
         * the kernel distinguishes them. */
        struct sockaddr_ll from;
        socklen_t fromlen = sizeof(from);
        const ssize_t n = recvfrom(d->rx_fd, buf, sizeof(buf), 0,
                                   (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        if (from.sll_pkttype == PACKET_OUTGOING) continue;
        uint32_t len = (uint32_t)n;
        d->seen++;

        uint32_t off = 0, plen = 0;
        if (!is_cip_io(buf, len, &off, &plen)) {
            tx(d, buf, len);           /* ARP, TCP 44818, anything else */
            d->forwarded++;
            continue;
        }
        d->io_seen++;

        if (hits(&d->seed, d->k.drop_pct)) { d->dropped++; continue; }

        if (plen && hits(&d->seed, d->k.trunc_pct)) {
            /* Cut inside the payload: a frame whose CPF items claim more than
             * the datagram carries is exactly the malformed case B4 is after. */
            len = off + (plen > 4 ? rnd(&d->seed, plen - 4) + 2 : 1);
            d->truncated++;
        } else if (plen && hits(&d->seed, d->k.corrupt_pct)) {
            buf[off + rnd(&d->seed, plen)] ^= (uint8_t)(1u << rnd(&d->seed, 8));
            d->corrupted++;
        }

        if (keep_n < REPLAY_KEEP) keep_n++;
        keep[keep_w].len = len;
        memcpy(keep[keep_w].data, buf, len);
        keep_w = (keep_w + 1) % REPLAY_KEEP;

        uint64_t hold = d->k.delay_us;
        if (d->k.jitter_us) hold += rnd(&d->seed, d->k.jitter_us);
        if (hits(&d->seed, d->k.reorder_pct)) {
            hold += d->k.reorder_gap_us;
            d->reordered++;
        }
        if (hold) {
            d->delayed++;
            if (hold > d->max_hold_us) d->max_hold_us = hold;
        }

        if (hold && qn < PENDING_MAX) {
            q[qn].due_us = t + hold;
            q[qn].len    = len;
            memcpy(q[qn].data, buf, len);
            qn++;
        } else {
            tx(d, buf, len);
            d->forwarded++;
        }

        if (hits(&d->seed, d->k.dup_pct)) {
            tx(d, buf, len);
            d->duplicated++;
        }
        if (keep_n > d->k.replay_age && d->k.replay_age &&
            hits(&d->seed, d->k.replay_pct)) {
            const uint32_t idx =
                (keep_w + REPLAY_KEEP - 1 - d->k.replay_age) % REPLAY_KEEP;
            tx(d, keep[idx].data, keep[idx].len);
            d->replayed++;
        }
    }

    for (uint32_t i = 0; i < qn; ++i) { tx(d, q[i].data, q[i].len); d->forwarded++; }
    free(q);
    free(keep);
    return NULL;
}

static int open_if(const char *ifname, int *ifindex) {
    const int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket(AF_PACKET)"); return -1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) { perror("SIOCGIFINDEX"); close(fd); return -1; }
    *ifindex = ifr.ifr_ifindex;

    /* Promiscuous: the frames being forwarded are addressed to the peer, not
     * to this namespace's interfaces. */
    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = *ifindex;
    mr.mr_type    = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
        perror("PACKET_MR_PROMISC");
    }

    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family   = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ALL);
    sa.sll_ifindex  = *ifindex;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    return fd;
}

static void report(const dir_t *d) {
    printf("impair %-4s frames=%llu io=%llu fwd=%llu drop=%llu delayed=%llu "
           "reordered=%llu dup=%llu replay=%llu trunc=%llu corrupt=%llu "
           "tx_err=%llu(%s) max_hold=%lluus\n",
           d->name,
           (unsigned long long)d->seen, (unsigned long long)d->io_seen,
           (unsigned long long)d->forwarded, (unsigned long long)d->dropped,
           (unsigned long long)d->delayed, (unsigned long long)d->reordered,
           (unsigned long long)d->duplicated, (unsigned long long)d->replayed,
           (unsigned long long)d->truncated, (unsigned long long)d->corrupted,
           (unsigned long long)d->tx_err,
           d->last_errno ? strerror(d->last_errno) : "-",
           (unsigned long long)d->max_hold_us);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <if_a> <if_b> <seconds> [delay_us=N jitter_us=N "
            "reorder_pct=N reorder_gap_us=N dup_pct=N replay_pct=N replay_age=N "
            "drop_pct=N trunc_pct=N corrupt_pct=N seed=N]\n", argv[0]);
        return 2;
    }
    const char *if_a = argv[1], *if_b = argv[2];
    const long seconds = strtol(argv[3], NULL, 10);

    knobs_t k;
    memset(&k, 0, sizeof(k));
    k.reorder_gap_us = 20000;   /* 2 x a 10 ms RPI unless told otherwise */
    k.replay_age     = 8;
    unsigned seed = (unsigned)time(NULL);

    for (int i = 4; i < argc; ++i) {
        const char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "bad argument: %s\n", argv[i]); return 2; }
        const unsigned long v = strtoul(eq + 1, NULL, 10);
        const size_t n = (size_t)(eq - argv[i]);
        #define KNOB(name, field) \
            if (n == strlen(name) && strncmp(argv[i], name, n) == 0) { \
                k.field = (uint32_t)v; continue; }
        KNOB("delay_us",        delay_us)
        KNOB("jitter_us",       jitter_us)
        KNOB("reorder_pct",     reorder_pct)
        KNOB("reorder_gap_us",  reorder_gap_us)
        KNOB("dup_pct",         dup_pct)
        KNOB("replay_pct",      replay_pct)
        KNOB("replay_age",      replay_age)
        KNOB("drop_pct",        drop_pct)
        KNOB("trunc_pct",       trunc_pct)
        KNOB("corrupt_pct",     corrupt_pct)
        #undef KNOB
        if (n == 4 && strncmp(argv[i], "seed", 4) == 0) { seed = (unsigned)v; continue; }
        fprintf(stderr, "unknown knob: %.*s\n", (int)n, argv[i]);
        return 2;
    }

    int idx_a = 0, idx_b = 0;
    const int fd_a = open_if(if_a, &idx_a);
    if (fd_a < 0) return EXIT_FAILURE;
    const int fd_b = open_if(if_b, &idx_b);
    if (fd_b < 0) return EXIT_FAILURE;

    dir_t a2b, b2a;
    memset(&a2b, 0, sizeof(a2b));
    memset(&b2a, 0, sizeof(b2a));
    a2b.rx_fd = fd_a; a2b.tx_fd = fd_b; a2b.tx_ifindex = idx_b;
    b2a.rx_fd = fd_b; b2a.tx_fd = fd_a; b2a.tx_ifindex = idx_a;
    a2b.k = b2a.k = k;
    a2b.seed = seed; b2a.seed = seed ^ 0x5bd1u;
    snprintf(a2b.name, sizeof(a2b.name), "a->b");
    snprintf(b2a.name, sizeof(b2a.name), "b->a");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("impair: %s <-> %s for %lds; delay=%uus jitter=%uus reorder=%u%%/%uus "
           "dup=%u%% replay=%u%%@%u drop=%u%% trunc=%u%% corrupt=%u%% seed=%u\n",
           if_a, if_b, seconds, k.delay_us, k.jitter_us, k.reorder_pct,
           k.reorder_gap_us, k.dup_pct, k.replay_pct, k.replay_age,
           k.drop_pct, k.trunc_pct, k.corrupt_pct, seed);
    fflush(stdout);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, pump, &a2b);
    pthread_create(&t2, NULL, pump, &b2a);

    const uint64_t end = now_us() + (uint64_t)seconds * 1000000u;
    while (!g_stop && now_us() < end) usleep(50000);
    g_stop = 1;
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    report(&a2b);
    report(&b2a);
    return EXIT_SUCCESS;
}
