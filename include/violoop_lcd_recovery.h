/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __VIOLOOP_LCD_RECOVERY_H__
#define __VIOLOOP_LCD_RECOVERY_H__

#if defined(CONFIG_VIOLOOP_VLA11_LCD_RECOVERY) && \
    defined(CONFIG_TARGET_VIOLOOP_RK3576_VLA11) && \
    !defined(CONFIG_SPL_BUILD) && !defined(CONFIG_TPL_BUILD)

#include <linux/types.h>

struct display_state;

enum vla11_lcd_profile {
	VLA11_LCD_PROFILE_UNKNOWN,
	VLA11_LCD_PROFILE_FPT,
	VLA11_LCD_PROFILE_JUJING,
};

struct vla11_lcd_dsi_snapshot {
	u32 power;
	u32 reset;
	u32 mode;
	u32 core;
	u32 phy_clock;
	u32 video;
	u32 general;
	u32 ipi;
	u32 vc;
};

int vla11_lcd_boot_identity(unsigned int *raw,
			   enum vla11_lcd_profile *profile);
int vla11_lcd_dsi_snapshot(struct display_state *state,
			  struct vla11_lcd_dsi_snapshot *snapshot);
int vla11_lcd_dsi_read(struct display_state *state, u8 command, u8 *value);
int vla11_lcd_dsi_write(struct display_state *state, const u8 *payload,
			unsigned int length, bool lpm);
int vla11_lcd_recovery_check(struct display_state *state);
int vla11_lcd_reinit_logo(struct display_state *state);
void vla11_lcd_record_init_command(u8 command, int result);
void vla11_lcd_record_reset_release(void);
void vla11_lcd_recovery_report_fdt(void *blob);

#endif
#endif
