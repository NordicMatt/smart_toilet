/* 2026-10-15T14:56:12.379362 */

/*
* Copyright (c) 2026 Nordic Semiconductor ASA
* SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
*/

#ifndef _NRF_EDGEAI_USER_MODEL_LABELS_H_
#define _NRF_EDGEAI_USER_MODEL_LABELS_H_

#include <nrf_edgeai/nrf_edgeai_ctypes.h>

#ifdef   __cplusplus
extern "C"
{
#endif

typedef enum nrf_edgeai_user_label_e {
    MODEL_USER_LABEL_OTHER,
    MODEL_USER_LABEL_SILENCE,
    MODEL_USER_LABEL_ABRACADABRA,

    MODEL_USER_LABEL_COUNT,
} nrf_edgeai_user_label_t;

static const char* NRF_EDGEAI_USER_LABELS_NAME[] = {
    "OTHER", "SILENCE", "abracadabra"
};

#ifdef   __cplusplus
}
#endif

#endif /* _NRF_EDGEAI_USER_MODEL_LABELS_H_ */
