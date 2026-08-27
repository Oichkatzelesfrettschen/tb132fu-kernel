/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SOC_MEDIATEK_MTK_DP_AUDIO_H
#define __LINUX_SOC_MEDIATEK_MTK_DP_AUDIO_H

#include <linux/kconfig.h>

#if IS_ENABLED(CONFIG_MTK_HDMI_SUPPORT)
void mtk_dp_audio_pcm_start(void);
void mtk_dp_audio_pcm_stop(void);
#else
static inline void mtk_dp_audio_pcm_start(void)
{
}

static inline void mtk_dp_audio_pcm_stop(void)
{
}
#endif

#endif /* __LINUX_SOC_MEDIATEK_MTK_DP_AUDIO_H */
