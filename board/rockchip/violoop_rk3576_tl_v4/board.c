// SPDX-License-Identifier: GPL-2.0+
/*
 * Violoop RK3576 TL V4 product board support.
 *
 * This file is intentionally board-owned. Keep hardware-specific USB and
 * early GPIO policy out of the other Violoop board targets.
 */

#include <common.h>
#include <adc.h>
#include <dwc3-uboot.h>
#include <fdt_support.h>
#include <usb.h>
#include <linux/usb/phy-rockchip-usbdp.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <rockusb.h>

DECLARE_GLOBAL_DATA_PTR;

#define TL_V4_LCD_ID_CHANNEL		2
#define TL_V4_LCD_ID_SAMPLES		8
#define TL_V4_LCD_ID_MIN_VALID_SAMPLES	5

/*
 * 12-bit SARADC counts with a 1.8 V reference.
 *
 * The TL V4 main board has 10 kOhm pull-up and pull-down resistors, so an
 * empty connector reads about 0.9 V (raw 2048).  JN3929595A adds another
 * 10 kOhm pull-down on the panel FPC; the effective 5 kOhm lower leg reads
 * about 0.6 V (raw 1365).  TTCM03921235 is the measured low-ID population.
 */
#define TL_V4_LCD_ID_FPT_MAX		450
#define TL_V4_LCD_ID_JUJING_MIN		950
#define TL_V4_LCD_ID_JUJING_MAX		1700
#define TL_V4_LCD_ID_NO_PANEL_MIN	1800

#define TL_V4_FPT_PANEL_COMPAT		"fpt,ttcm03921235"
#define TL_V4_JUJING_PANEL_COMPAT	"jujing,jn3929595a"
#define TL_V4_FPT_TOUCH_COMPAT		"focaltech,ft3519"
#define TL_V4_JUJING_TOUCH_COMPAT	"hyn,cst3640"
#define TL_V4_DSI_ROUTE_PATH		"/display-subsystem/route/route-dsi"

enum tl_v4_lcd_type {
	TL_V4_LCD_FPT,
	TL_V4_LCD_JUJING,
	TL_V4_LCD_NONE,
	TL_V4_LCD_UNKNOWN,
};

static int tl_v4_adc_single_shot(unsigned int channel, unsigned int *value)
{
	int ret;

	/*
	 * Keep this in step with rockchip_dnl_key_pressed().  Depending on the
	 * U-Boot DT/driver version, the RK3576 SARADC uclass device is registered
	 * as either "saradc" or "adc".  Trying only the former made every LCD-ID
	 * conversion fail on TL V4 even though ADC2 is physically valid.
	 */
	ret = adc_channel_single_shot("saradc", channel, value);
	if (ret)
		ret = adc_channel_single_shot("adc", channel, value);

	return ret;
}

static int tl_v4_read_lcd_id(unsigned int *average)
{
	unsigned long sum = 0;
	unsigned int value;
	int valid = 0;
	int i;

	/* Discard the first conversion after the ADC becomes active. */
	tl_v4_adc_single_shot(TL_V4_LCD_ID_CHANNEL, &value);

	for (i = 0; i < TL_V4_LCD_ID_SAMPLES; i++) {
		if (!tl_v4_adc_single_shot(TL_V4_LCD_ID_CHANNEL, &value)) {
			sum += value;
			valid++;
		}
		mdelay(2);
	}

	if (valid < TL_V4_LCD_ID_MIN_VALID_SAMPLES)
		return -EIO;

	*average = DIV_ROUND_CLOSEST(sum, valid);
	return 0;
}

static enum tl_v4_lcd_type tl_v4_classify_lcd(unsigned int raw)
{
	if (raw <= TL_V4_LCD_ID_FPT_MAX)
		return TL_V4_LCD_FPT;
	if (raw >= TL_V4_LCD_ID_JUJING_MIN &&
	    raw <= TL_V4_LCD_ID_JUJING_MAX)
		return TL_V4_LCD_JUJING;
	if (raw >= TL_V4_LCD_ID_NO_PANEL_MIN)
		return TL_V4_LCD_NONE;

	return TL_V4_LCD_UNKNOWN;
}

static int tl_v4_find_panel(void *blob)
{
	int node;

	node = fdt_node_offset_by_compatible(blob, -1,
					     TL_V4_FPT_PANEL_COMPAT);
	if (node < 0)
		node = fdt_node_offset_by_compatible(blob, -1,
					     TL_V4_JUJING_PANEL_COMPAT);

	return node;
}

static int tl_v4_set_compatible_status(void *blob, const char *compatible,
				       bool enable)
{
	int node;

	/*
	 * Never retain a libfdt node offset across another setprop operation:
	 * changing a property's length moves every following node in the blob.
	 */
	node = fdt_node_offset_by_compatible(blob, -1, compatible);
	if (node < 0)
		return node;

	return enable ? fdt_status_okay(blob, node) :
		fdt_status_disabled(blob, node);
}

static const char *tl_v4_lcd_name(enum tl_v4_lcd_type type)
{
	switch (type) {
	case TL_V4_LCD_FPT:
		return "fpt-ttcm03921235";
	case TL_V4_LCD_JUJING:
		return "jujing-jn3929595a";
	case TL_V4_LCD_NONE:
		return "no-panel";
	default:
		return "unknown";
	}
}

int ft_board_setup(void *blob, bd_t *bd)
{
	enum tl_v4_lcd_type type;
	unsigned int raw;
	int panel;
	int fpt_touch;
	int jujing_touch;
	int route_dsi;
	int chosen;
	int ret;

	(void)bd;
	ret = tl_v4_read_lcd_id(&raw);
	if (ret) {
		/*
		 * Preserve the DT's safe, populated-board default on an ADC driver
		 * error.  Only a valid high ADC reading is allowed to mean no panel;
		 * a transient read failure must never blank a fitted display.
		 */
		raw = ~0U;
		type = TL_V4_LCD_FPT;
		printf("TL V4 LCD-ID: ADC2 read failed (%d), use FPT fallback\n",
		       ret);
	} else {
		type = tl_v4_classify_lcd(raw);
		if (type == TL_V4_LCD_UNKNOWN) {
			printf("TL V4 LCD-ID: ADC2 raw=%u is unclassified, use FPT fallback\n",
			       raw);
			type = TL_V4_LCD_FPT;
		}
	}
	panel = tl_v4_find_panel(blob);
	fpt_touch = fdt_node_offset_by_compatible(blob, -1,
						 TL_V4_FPT_TOUCH_COMPAT);
	jujing_touch = fdt_node_offset_by_compatible(blob, -1,
						    TL_V4_JUJING_TOUCH_COMPAT);
	if (panel < 0 || fpt_touch < 0 || jujing_touch < 0) {
		printf("TL V4 LCD-ID: DT nodes missing (%d/%d/%d), keep fallback\n",
		       panel, fpt_touch, jujing_touch);
		return 0;
	}

	switch (type) {
	case TL_V4_LCD_FPT:
		panel = tl_v4_find_panel(blob);
		fdt_status_okay(blob, panel);
		tl_v4_set_compatible_status(blob, TL_V4_FPT_TOUCH_COMPAT, true);
		tl_v4_set_compatible_status(blob, TL_V4_JUJING_TOUCH_COMPAT,
					    false);
		route_dsi = fdt_path_offset(blob, TL_V4_DSI_ROUTE_PATH);
		if (route_dsi >= 0)
			fdt_status_okay(blob, route_dsi);
		break;
	case TL_V4_LCD_JUJING:
		/* Touch status changes can move the later node offsets. */
		tl_v4_set_compatible_status(blob, TL_V4_FPT_TOUCH_COMPAT, false);
		tl_v4_set_compatible_status(blob, TL_V4_JUJING_TOUCH_COMPAT,
					    true);
		panel = tl_v4_find_panel(blob);
		fdt_status_okay(blob, panel);
		ret = fdt_setprop_string(blob, panel, "compatible",
					 TL_V4_JUJING_PANEL_COMPAT);
		if (ret) {
			printf("TL V4 LCD-ID: panel compatible update failed: %d\n",
			       ret);
			return 0;
		}
		route_dsi = fdt_path_offset(blob, TL_V4_DSI_ROUTE_PATH);
		if (route_dsi >= 0)
			fdt_status_okay(blob, route_dsi);
		break;
	case TL_V4_LCD_NONE:
		/* Only a valid open/no-panel voltage may disable the display path. */
		tl_v4_set_compatible_status(blob, TL_V4_FPT_TOUCH_COMPAT, false);
		tl_v4_set_compatible_status(blob, TL_V4_JUJING_TOUCH_COMPAT,
					    false);
		panel = tl_v4_find_panel(blob);
		fdt_status_disabled(blob, panel);
		route_dsi = fdt_path_offset(blob, TL_V4_DSI_ROUTE_PATH);
		if (route_dsi >= 0)
			fdt_status_disabled(blob, route_dsi);
		break;
	case TL_V4_LCD_UNKNOWN:
		/* All unknown values are normalized to the FPT fallback above. */
		break;
	}

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0) {
		fdt_setprop_u32(blob, chosen, "violoop,lcd-id-raw", raw);
		fdt_setprop_string(blob, chosen, "violoop,lcd-panel",
				   tl_v4_lcd_name(type));
	}

	printf("TL V4 LCD-ID: ADC2 raw=%u, panel=%s\n", raw,
	       tl_v4_lcd_name(type));
	return 0;
}

#ifdef CONFIG_SPL_BUILD
int rk_spl_board_init(void)
{
	int ret;

	ret = gpio_hog_probe_all();
	if (ret)
		debug("Violoop GPIO hog probe failed: %d\n", ret);

	return ret;
}
#endif

#ifdef CONFIG_USB_DWC3
#define CRU_BASE		0x27200000
#define CRU_SOFTRST_CON47	0x0abc

static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_SUPER,
	.base = 0x23000000,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.dis_u2_susphy_quirk = 1,
	.dis_u1u2_quirk = 1,
	.usb2_phyif_utmi_width = 16,
};

int usb_gadget_handle_interrupts(int index)
{
	dwc3_uboot_handle_interrupt(0);
	return 0;
}

bool rkusb_usb3_capable(void)
{
	return true;
}

static void usb_reset_otg_controller(void)
{
	writel(0x00200020, CRU_BASE + CRU_SOFTRST_CON47);
	mdelay(1);
	writel(0x00200000, CRU_BASE + CRU_SOFTRST_CON47);
	mdelay(1);
}

int board_usb_init(int index, enum usb_init_type init)
{
	u32 ret = 0;

	usb_reset_otg_controller();

#if defined(CONFIG_SUPPORT_USBPLUG)
	dwc3_device_data.maximum_speed = USB_SPEED_HIGH;

	if (rkusb_switch_usb3_enabled()) {
		dwc3_device_data.maximum_speed = USB_SPEED_SUPER;
		ret = rockchip_u3phy_uboot_init();
		if (ret) {
			rkusb_force_to_usb2(true);
			dwc3_device_data.maximum_speed = USB_SPEED_HIGH;
		}
	}
#else
	ret = rockchip_u3phy_uboot_init();
	if (ret) {
		rkusb_force_to_usb2(true);
		dwc3_device_data.maximum_speed = USB_SPEED_HIGH;
	}
#endif

	return dwc3_uboot_init(&dwc3_device_data);
}

#if defined(CONFIG_SUPPORT_USBPLUG)
int board_usb_cleanup(int index, enum usb_init_type init)
{
	dwc3_uboot_exit(index);
	return 0;
}
#endif

#endif
