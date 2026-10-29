# SPDX-License-Identifier: GPL-2.0+

obj-y += board.o

ifndef CONFIG_SPL_BUILD
ifndef CONFIG_TPL_BUILD
obj-$(CONFIG_VIOLOOP_VLA11_LCD_RECOVERY) += lcd_recovery.o
ifeq ($(CONFIG_VIOLOOP_VLA11_LCD_RECOVERY),y)
ccflags-y += -I$(srctree)/drivers/video/drm
endif
endif
endif
