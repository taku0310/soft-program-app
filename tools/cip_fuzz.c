/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file cip_fuzz.c
 * @brief Sends deliberately malformed CIP I/O datagrams at a live stack.
 *
 * Acceptance check B4. The class 1 I/O port is a UDP port on a plant network:
 * anything that can route to the PLC can put a datagram on it, and nothing in
 * CIP authenticates one. So the only safe assumption is that the parser will
 * be handed garbage, and the requirement is that garbage costs one dropped
 * frame.
 *
 * It did not. `Buffer::operator>>(std::vector<uint8_t>&)` copied a length
 * taken straight from the datagram without comparing it to what had actually
 * arrived, so a common packet item claiming more bytes than were present made
 * std::copy run off the end of the heap. One datagram, SIGSEGV, measured -
 * see patches/eipscanner-bounds.patch.
 *
 *   cip_fuzz <ip> <count> [seed]
 *
 * The cases are shaped rather than random: a fuzzer that mostly sends noise
 * spends its time being rejected by the first length check. These aim at the
 * places where a parser trusts a number it was given.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CIP_IO_PORT 2222

static uint32_t rnd(unsigned *seed, uint32_t bound) {
    return bound ? (uint32_t)(rand_r(seed) % bound) : 0;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/** Builds one malformed datagram; returns its length. */
static uint32_t build(unsigned *seed, uint32_t which, uint8_t *b, uint32_t cap) {
    memset(b, 0, cap);
    switch (which % 8u) {
    case 0:
        /* Nothing at all. The parse starts by reading an item count. */
        return 0;
    case 1:
        /* Half of the item count. */
        return 1;
    case 2:
        /* Item count says two, no items follow. */
        put16(b, 2);
        return 2;
    case 3:
        /* A sequenced address item whose length is larger than the datagram:
         * the case that read off the end of the heap. */
        put16(b + 0, 2);        /* item count            */
        put16(b + 2, 0x8002);   /* sequenced address     */
        put16(b + 4, 0xFFFF);   /* claimed length        */
        return 6;
    case 4:
        /* Well-formed address item, connected data item claiming 64 KiB. */
        put16(b + 0, 2);
        put16(b + 2, 0x8002);
        put16(b + 4, 8);
        put32(b + 6, 0x12345678u);  /* connection id  */
        put32(b + 10, 1);           /* sequence count */
        put16(b + 14, 0x00B1);      /* connected data */
        put16(b + 16, 0xFFFF);
        return 18;
    case 5:
        /* An item count of 65535 with one short item behind it. */
        put16(b + 0, 0xFFFF);
        put16(b + 2, 0x8002);
        put16(b + 4, 2);
        return 8;
    case 6: {
        /* A plausible frame cut at a random point. */
        put16(b + 0, 2);
        put16(b + 2, 0x8002);
        put16(b + 4, 8);
        put32(b + 6, 0x12345678u);
        put32(b + 10, 1);
        put16(b + 14, 0x00B1);
        put16(b + 16, 32);
        for (uint32_t i = 0; i < 32; ++i) b[18 + i] = (uint8_t)i;
        return 2 + rnd(seed, 48);
    }
    default: {
        /* Noise, for the paths the shaped cases do not reach. */
        const uint32_t n = 1 + rnd(seed, 64);
        for (uint32_t i = 0; i < n; ++i) b[i] = (uint8_t)rnd(seed, 256);
        return n;
    }
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ip> <count> [seed]\n", argv[0]);
        return 2;
    }
    const char *ip = argv[1];
    const long count = strtol(argv[2], NULL, 10);
    unsigned seed = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 10)
                               : (unsigned)time(NULL);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return EXIT_FAILURE; }

    /* Source port 2222 as well: class 1 I/O is fixed at that port on both
     * ends, and a stack may check it. */
    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port   = htons(CIP_IO_PORT);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) < 0) {
        /* Not fatal: the real scanner may already hold it on this host. */
        perror("bind(2222), continuing from an ephemeral port");
    }

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons(CIP_IO_PORT);
    if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) {
        fprintf(stderr, "bad address: %s\n", ip);
        return EXIT_FAILURE;
    }

    uint8_t buf[2048];
    long sent = 0;
    for (long i = 0; i < count; ++i) {
        const uint32_t len = build(&seed, (uint32_t)i, buf, sizeof(buf));
        if (sendto(fd, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) >= 0) sent++;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    close(fd);
    printf("cip_fuzz: sent %ld of %ld malformed datagrams to %s:%d (seed %u)\n",
           sent, count, ip, CIP_IO_PORT, seed);
    return EXIT_SUCCESS;
}
