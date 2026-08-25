// SPDX-License-Identifier: GPL-2.0+
/*
 * Violoop RK3576 VLA11 product board support.
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

/*
 * USBDP PHY 基址。本仓 u-boot 比 ODM 的 violoop_sdk 基线新：
 * drivers/phy/phy-rockchip-usbdp.c 的 rockchip_u3phy_uboot_init 已从
 * 无参改成收一个 fdt_addr_t phy_addr。同芯片的 board/rockchip/evb_rk3576
 * 用的就是这个值。
 */
#define U3PHY_BASE			0x2b010000

/*
 * 恢复按钮（长按 5 秒 = 恢复出厂）。
 *
 * 硬件与检测：按钮接 SARADC 通道 1，按下拉到地。u-boot DTS 的 adc-keys 节点
 * （io-channels = <&saradc 1>）声明的就是它，mach-rockchip 的
 * rockchip_dnl_key_pressed() 也读同一路，阈值同为原始值 0..30 —— 这里刻意与
 * 它保持一致，两处一起改。
 *
 * 为什么要接管：RK 原生流程在「按键按下 + 无 VBUS」时置 reboot_mode=recovery-key，
 * boot_fit 随即改从 recovery 分区加载 FIT。而本产品的分区表没有 recovery 分区
 * （复位靠擦 overlay，见 os-next docs/partitions.md），那条路会走到
 * "No recovery partition" 然后启动失败 —— 也就是说不接管的话，**按一下这个按钮
 * 设备就起不来**。
 *
 * 接管方式：在 rk_board_late_init()（跑在 setup_download_mode() 之后）拦下
 * recovery-key，无条件把 reboot_mode 复位成 normal，再自己做长按确认；确认通过
 * 就往内核 cmdline 追加标记，由 initramfs 的 overlay-root 执行擦除。
 * 「按键 + 插 USB → maskrom 下载」那条原生路径不受影响，整机重刷仍走它。
 */
#define VLA11_RECOVERY_KEY_CHANNEL	1
#define VLA11_RECOVERY_KEY_MAX_VAL	30	/* 同 KEY_DOWN_MAX_VAL */
#define VLA11_RECOVERY_HOLD_MS		5000
#define VLA11_RECOVERY_POLL_MS		100
/* 允许的瞬时抖动：连续这么多次采样读不到按下才判定为松手 */
#define VLA11_RECOVERY_RELEASE_SLACK	3
#define VLA11_RECOVERY_CMDLINE		"violoop.recovery=1"

#define VLA11_LCD_ID_CHANNEL		2
#define VLA11_LCD_ID_SAMPLES		15
#define VLA11_LCD_ID_MIN_VALID_SAMPLES	9
/*
 * SARADC channel 1 is sampled before LCD-ID channel 2 during boot.  Discard
 * the initial conversions and allow the sample-and-hold input to settle so a
 * grounded LCD-ID is not misclassified as a resistor-coded panel.
 */
#define VLA11_LCD_ID_DISCARD		5
#define VLA11_LCD_ID_SETTLE_MS		5
/* Reject a sample window that is still moving instead of guessing a panel. */
#define VLA11_LCD_ID_SPREAD_MAX		120

/*
 * 12-bit SARADC counts with a 1.8 V reference.
 *
 * TTCM03921235 straps LCD-ID to ground.  JN3929595A leaves a resistor-coded
 * non-zero level (nominal boards have measured roughly 0.6--0.9 V depending
 * on the fitted divider).  An empty connector can overlap the Jujing level,
 * so ADC2 alone cannot safely identify "no panel".  Treat every stable
 * resistor-coded high level as Jujing; probing a harmless absent panel is
 * preferable to blanking a fitted Jujing display.
 */
#define VLA11_LCD_ID_FPT_MAX		450
#define VLA11_LCD_ID_JUJING_MIN		950

/* LCD-ID selects both the panel command path and its matching touch driver. */
#define VLA11_FPT_PANEL_COMPAT		"simple-panel-dsi"
#define VLA11_JUJING_PANEL_COMPAT	"jujing,jn3929595a"
#define VLA11_FPT_TOUCH_COMPAT		"focaltech,ft3519"
#define VLA11_JUJING_TOUCH_COMPAT	"hyn,cst3640"
#define VLA11_PANEL_PATH		"/dsi@27d80000/panel@0"
#define VLA11_DSI_ROUTE_PATH		"/display-subsystem/route/route-dsi"

enum vla11_lcd_type {
	VLA11_LCD_FPT,
	VLA11_LCD_JUJING,
	VLA11_LCD_UNKNOWN,
};

static int vla11_adc_single_shot(unsigned int channel, unsigned int *value)
{
	int ret;

	/*
	 * Keep this in step with rockchip_dnl_key_pressed().  Depending on the
	 * U-Boot DT/driver version, the RK3576 SARADC uclass device is registered
	 * as either "saradc" or "adc".  Trying only the former made every LCD-ID
	 * conversion fail on VLA11 even though ADC2 is physically valid.
	 */
	ret = adc_channel_single_shot("saradc", channel, value);
	if (ret)
		ret = adc_channel_single_shot("adc", channel, value);

	return ret;
}

static int vla11_read_lcd_id(unsigned int *result)
{
	unsigned int samples[VLA11_LCD_ID_SAMPLES];
	unsigned int value;
	unsigned int spread;
	int valid = 0;
	int i, j;

	/*
	 * Drain the sample-and-hold remnant left by the channel 1 conversion
	 * before any reading is kept.  One discard was not enough.
	 */
	for (i = 0; i < VLA11_LCD_ID_DISCARD; i++) {
		vla11_adc_single_shot(VLA11_LCD_ID_CHANNEL, &value);
		mdelay(VLA11_LCD_ID_SETTLE_MS);
	}

	for (i = 0; i < VLA11_LCD_ID_SAMPLES; i++) {
		if (!vla11_adc_single_shot(VLA11_LCD_ID_CHANNEL, &value))
			samples[valid++] = value;
		mdelay(VLA11_LCD_ID_SETTLE_MS);
	}

	if (valid < VLA11_LCD_ID_MIN_VALID_SAMPLES)
		return -EIO;

	/* Insertion sort: tiny array, and it yields both median and spread. */
	for (i = 1; i < valid; i++) {
		value = samples[i];
		for (j = i - 1; j >= 0 && samples[j] > value; j--)
			samples[j + 1] = samples[j];
		samples[j + 1] = value;
	}

	spread = samples[valid - 1] - samples[0];

	/* Leave the distribution in the boot log; a mean hid this for months. */
	printf("VLA11 LCD-ID: %d samples min=%u median=%u max=%u spread=%u\n",
	       valid, samples[0], samples[valid / 2], samples[valid - 1],
	       spread);

	if (spread > VLA11_LCD_ID_SPREAD_MAX) {
		printf("VLA11 LCD-ID: spread %u over %u, line still settling\n",
		       spread, VLA11_LCD_ID_SPREAD_MAX);
		return -EIO;
	}

	/*
	 * Median, not mean.  A handful of unsettled conversions must not be
	 * able to drag the result across a classification threshold.
	 */
	*result = samples[valid / 2];
	return 0;
}

static enum vla11_lcd_type vla11_classify_lcd(unsigned int raw)
{
	if (raw <= VLA11_LCD_ID_FPT_MAX)
		return VLA11_LCD_FPT;
	if (raw >= VLA11_LCD_ID_JUJING_MIN)
		return VLA11_LCD_JUJING;

	return VLA11_LCD_UNKNOWN;
}

static int vla11_find_panel(void *blob)
{
	int node;

	node = fdt_path_offset(blob, VLA11_PANEL_PATH);
	if (node < 0)
		node = fdt_node_offset_by_compatible(blob, -1,
						     VLA11_FPT_PANEL_COMPAT);

	return node;
}

static int vla11_set_panel_compatible(void *blob, const char *compatible)
{
	int node = vla11_find_panel(blob);

	if (node < 0)
		return node;

	return fdt_setprop_string(blob, node, "compatible", compatible);
}

static int vla11_set_compatible_status(void *blob, const char *compatible,
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

static const char *vla11_lcd_name(enum vla11_lcd_type type)
{
	switch (type) {
	case VLA11_LCD_FPT:
		return "fpt-ttcm03921235";
	case VLA11_LCD_JUJING:
		return "jujing-jn3929595a";
	default:
		return "unknown";
	}
}

/* 确认过长按、需要把标记传给内核。仅在本次启动内有效。 */
static bool vla11_recovery_armed;

static bool vla11_recovery_key_down(void)
{
	unsigned int val;

	if (vla11_adc_single_shot(VLA11_RECOVERY_KEY_CHANNEL, &val))
		return false;	/* 读不到就当没按，宁可不擦 */

	return val <= VLA11_RECOVERY_KEY_MAX_VAL;
}

/*
 * 长按确认：要求按钮在 VLA11_RECOVERY_HOLD_MS 内保持按下。
 * 中途松手即放弃（擦除不可逆，误触代价是用户数据全没，所以取最严的语义）。
 * 只容忍 VLA11_RECOVERY_RELEASE_SLACK 次连续读不到的瞬时抖动。
 */
static bool vla11_recovery_confirm_hold(void)
{
	int elapsed = 0;
	int misses = 0;
	int last_announced = -1;

	printf("VLA11 recovery: key down, hold %d s to factory reset...\n",
	       VLA11_RECOVERY_HOLD_MS / 1000);

	while (elapsed < VLA11_RECOVERY_HOLD_MS) {
		mdelay(VLA11_RECOVERY_POLL_MS);
		elapsed += VLA11_RECOVERY_POLL_MS;

		if (vla11_recovery_key_down()) {
			misses = 0;
		} else if (++misses > VLA11_RECOVERY_RELEASE_SLACK) {
			printf("VLA11 recovery: released after %d ms, aborted\n",
			       elapsed);
			return false;
		}

		/* 每秒回显一次，让现场知道还要按多久 */
		if (elapsed / 1000 != last_announced) {
			last_announced = elapsed / 1000;
			printf("VLA11 recovery: %d/%d s\n", last_announced,
			       VLA11_RECOVERY_HOLD_MS / 1000);
		}
	}

	printf("VLA11 recovery: confirmed, will wipe overlay on this boot\n");
	return true;
}

int rk_board_late_init(void)
{
	const char *mode = env_get("reboot_mode");

	if (!mode || strcmp(mode, "recovery-key"))
		return 0;

	/*
	 * 无条件复位 reboot_mode —— 本板没有 recovery 分区，留着它 boot_fit 会去
	 * 找不存在的分区然后启动失败。长按确认与否都要清掉。
	 */
	env_set("reboot_mode", "normal");

	if (vla11_recovery_confirm_hold())
		vla11_recovery_armed = true;

	return 0;
}

int ft_board_setup(void *blob, bd_t *bd)
{
	enum vla11_lcd_type type;
	unsigned int raw;
	int panel;
	int fpt_touch;
	int jujing_touch;
	int route_dsi;
	int chosen;
	int ret;

	(void)bd;
	ret = vla11_read_lcd_id(&raw);
	if (ret) {
		/*
		 * Preserve the populated-board default on an ADC error.  A transient
		 * read failure must not disable a fitted display.
		 */
		raw = ~0U;
		type = VLA11_LCD_FPT;
		printf("VLA11 LCD-ID: ADC2 read failed (%d), use FPT fallback\n",
		       ret);
	} else {
		type = vla11_classify_lcd(raw);
		if (type == VLA11_LCD_UNKNOWN) {
			printf("VLA11 LCD-ID: ADC2 raw=%u is unclassified, use FPT fallback\n",
			       raw);
			type = VLA11_LCD_FPT;
		}
	}
	panel = vla11_find_panel(blob);
	fpt_touch = fdt_node_offset_by_compatible(blob, -1,
						 VLA11_FPT_TOUCH_COMPAT);
	jujing_touch = fdt_node_offset_by_compatible(blob, -1,
						    VLA11_JUJING_TOUCH_COMPAT);
	if (panel < 0 || fpt_touch < 0 || jujing_touch < 0) {
		printf("VLA11 LCD-ID: DT nodes missing (%d/%d/%d), keep fallback\n",
		       panel, fpt_touch, jujing_touch);
		return 0;
	}

	switch (type) {
	case VLA11_LCD_FPT:
		vla11_set_panel_compatible(blob, VLA11_FPT_PANEL_COMPAT);
		panel = vla11_find_panel(blob);
		fdt_status_okay(blob, panel);
		vla11_set_compatible_status(blob, VLA11_FPT_TOUCH_COMPAT, true);
		vla11_set_compatible_status(blob, VLA11_JUJING_TOUCH_COMPAT,
					    false);
		route_dsi = fdt_path_offset(blob, VLA11_DSI_ROUTE_PATH);
		if (route_dsi >= 0)
			fdt_status_okay(blob, route_dsi);
		break;
	case VLA11_LCD_JUJING:
		/* Touch status changes can move the later node offsets. */
		vla11_set_compatible_status(blob, VLA11_FPT_TOUCH_COMPAT, false);
		vla11_set_compatible_status(blob, VLA11_JUJING_TOUCH_COMPAT,
					    true);
		vla11_set_panel_compatible(blob, VLA11_JUJING_PANEL_COMPAT);
		panel = vla11_find_panel(blob);
		fdt_status_okay(blob, panel);
		route_dsi = fdt_path_offset(blob, VLA11_DSI_ROUTE_PATH);
		if (route_dsi >= 0)
			fdt_status_okay(blob, route_dsi);
		break;
	case VLA11_LCD_UNKNOWN:
		/* All unknown values are normalized to the FPT fallback above. */
		break;
	}

	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0) {
		fdt_setprop_u32(blob, chosen, "violoop,lcd-id-raw", raw);
		fdt_setprop_string(blob, chosen, "violoop,lcd-panel",
				   vla11_lcd_name(type));
	}

	/*
	 * 恢复出厂标记走 cmdline 追加，不新开 /chosen 属性：initramfs 里读
	 * /proc/cmdline 比解析 DT 简单得多，且 overlay-root 已经是 shell 脚本。
	 * 必须用 fdt_bootargs_append()——bootargs 是 os-next 在构建期烤进
	 * resource.img 内那份 dtb 的（见 build-boot.sh），直接 setprop 会把它整条覆盖掉。
	 */
	if (vla11_recovery_armed) {
		if (fdt_bootargs_append(blob, VLA11_RECOVERY_CMDLINE))
			printf("VLA11 recovery: failed to append cmdline marker\n");
		else
			printf("VLA11 recovery: cmdline += %s\n",
			       VLA11_RECOVERY_CMDLINE);
	}

	printf("VLA11 LCD-ID: ADC2 raw=%u, panel=%s\n", raw,
	       vla11_lcd_name(type));
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
		ret = rockchip_u3phy_uboot_init(U3PHY_BASE);
		if (ret) {
			rkusb_force_to_usb2(true);
			dwc3_device_data.maximum_speed = USB_SPEED_HIGH;
		}
	}
#else
	ret = rockchip_u3phy_uboot_init(U3PHY_BASE);
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
