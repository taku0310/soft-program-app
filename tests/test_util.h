/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test_util.h
 * @brief Assertion helpers.
 *
 * No external framework: the point of this suite is to be runnable inside the
 * same minimal container the runtime ships in, with nothing installed.
 */
#ifndef SOFTPLC_TEST_UTIL_H
#define SOFTPLC_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                       \
    do {                                                                     \
        g_checks++;                                                          \
        const long long a_ = (long long)(actual);                            \
        const long long e_ = (long long)(expected);                          \
        if (a_ != e_) {                                                      \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n",       \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
        }                                                                    \
    } while (0)

#define CHECK_MEM_EQ(a, b, n)                                                \
    do {                                                                     \
        g_checks++;                                                          \
        if (memcmp((a), (b), (n)) != 0) {                                    \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s != %s over %zu bytes\n",         \
                    __FILE__, __LINE__, #a, #b, (size_t)(n));                \
        }                                                                    \
    } while (0)

#define TEST_REPORT(name)                                                    \
    do {                                                                     \
        printf("%s: %d checks, %d failures\n", (name), g_checks, g_failures); \
        return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;                     \
    } while (0)

#endif /* SOFTPLC_TEST_UTIL_H */
