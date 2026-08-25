/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __CONFIGS_VIOLOOP_RK3576_VLA11_H
#define __CONFIGS_VIOLOOP_RK3576_VLA11_H

#include <configs/rk3576_common.h>

#ifndef CONFIG_SPL_BUILD

#undef ROCKCHIP_DEVICE_SETTINGS
#ifdef CONFIG_DM_VIDEO
#define ROCKCHIP_DEVICE_SETTINGS \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0"
#else
#define ROCKCHIP_DEVICE_SETTINGS \
		"stdout=serial\0" \
		"stderr=serial\0"
#endif

#define CONFIG_SYS_MMC_ENV_DEV		0

#undef CONFIG_BOOTCOMMAND
#ifdef CONFIG_DM_VIDEO
#define CONFIG_BOOTCOMMAND	RKIMG_BOOTCOMMAND
#else
#define CONFIG_BOOTCOMMAND	"boot_fit;"
#endif

#endif /* CONFIG_SPL_BUILD */
#endif /* __CONFIGS_VIOLOOP_RK3576_VLA11_H */
