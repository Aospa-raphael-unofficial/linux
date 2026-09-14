/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_RPMHPD_H
#define __SOC_QCOM_RPMHPD_H

#include <linux/device.h>

#if IS_ENABLED(CONFIG_QCOM_RPMHPD)

bool qcom_rpmhpd_is_synced(void);
int qcom_rpmhpd_wait_sync(unsigned long timeout_ms);

#else

static inline bool qcom_rpmhpd_is_synced(void)
{
	return true;
}

static inline int qcom_rpmhpd_wait_sync(unsigned long timeout_ms)
{
	return 0;
}

#endif

#endif
