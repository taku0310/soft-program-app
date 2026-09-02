/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file plc_status.h
 * @brief Status codes shared by the PLC core and every protocol adapter.
 */
#ifndef SOFTPLC_PLC_STATUS_H
#define SOFTPLC_PLC_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum plc_status {
    PLC_OK              =  0,
    PLC_ERR_INVAL       = -1,  /**< bad argument                            */
    PLC_ERR_NOMEM       = -2,  /**< allocation or shared-memory sizing failed */
    PLC_ERR_IO          = -3,  /**< underlying transport reported an error  */
    PLC_ERR_TIMEOUT     = -4,  /**< peer did not answer within the budget   */
    PLC_ERR_PROTO       = -5,  /**< framing / ABI mismatch                  */
    PLC_ERR_STATE       = -6,  /**< call not legal in the current state     */
    PLC_ERR_UNSUPPORTED = -7,  /**< optional feature not implemented        */
    PLC_ERR_AGAIN       = -8,  /**< would block; retry on the next scan     */
    PLC_ERR_NOTFOUND    = -9   /**< no such adapter / symbol / instance     */
} plc_status_t;

/** Short static description of @p st.  Never NULL. */
const char *plc_strerror(plc_status_t st);

#ifdef __cplusplus
}
#endif
#endif /* SOFTPLC_PLC_STATUS_H */
