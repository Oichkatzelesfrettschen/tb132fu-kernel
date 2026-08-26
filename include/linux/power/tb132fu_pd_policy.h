/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_POWER_TB132FU_PD_POLICY_H
#define _LINUX_POWER_TB132FU_PD_POLICY_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_CHARGER_TB132FU_PD_POLICY)
int tb132fu_pd_policy_set_charge_hold(bool hold);
#else
static inline int tb132fu_pd_policy_set_charge_hold(bool hold)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_POWER_TB132FU_PD_POLICY_H */
