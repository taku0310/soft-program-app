/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file eip_scanner_shm_layout_public.h
 * @brief The parts of the scanner layout a backend legitimately needs.
 *
 * eip_scanner_shm_layout.h is C-only on purpose - it contains the `_Atomic`
 * rings the C++ side must never touch. But a backend does need the health-byte
 * vocabulary it reports. Those constants live here, in a header both languages
 * can include, so that the C-only rule costs nothing.
 */
#ifndef SOFTPLC_EIP_SCANNER_SHM_LAYOUT_PUBLIC_H
#define SOFTPLC_EIP_SCANNER_SHM_LAYOUT_PUBLIC_H

#define EIP_DEVICE_OFFLINE   0u  /**< no CIP connection established     */
#define EIP_DEVICE_ONLINE    1u  /**< connected, data fresh             */
#define EIP_DEVICE_FAILSAFE  2u  /**< connection lost, failsafe applied */

#endif /* SOFTPLC_EIP_SCANNER_SHM_LAYOUT_PUBLIC_H */
