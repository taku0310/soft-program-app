/* SPDX-License-Identifier: Apache-2.0 */
/** @file softplc_status.h
 *  @brief `softplc --status`: one JSON object describing a running instance.
 *
 *  Exit status is the useful half: 0 ONLINE and fully connected, 3 reachable
 *  but not ready, 4 nothing published under that name. A container health
 *  check can use it without parsing anything.
 */
#ifndef SOFTPLC_APP_STATUS_H
#define SOFTPLC_APP_STATUS_H

int softplc_print_status(const char *instance);

#endif /* SOFTPLC_APP_STATUS_H */
