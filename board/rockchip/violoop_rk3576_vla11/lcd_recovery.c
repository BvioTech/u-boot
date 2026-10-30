// SPDX-License-Identifier: GPL-2.0+
/* Bounded, board-owned validation of the display inherited by Linux. */
#include <common.h>
#include <violoop_lcd_recovery.h>

#if defined(CONFIG_VIOLOOP_VLA11_LCD_RECOVERY) && \
    defined(CONFIG_TARGET_VIOLOOP_RK3576_VLA11) && \
    !defined(CONFIG_SPL_BUILD) && !defined(CONFIG_TPL_BUILD)

#include <fdt_support.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "../../../drivers/video/drm/rockchip_display.h"
#include "../../../drivers/video/drm/rockchip_connector.h"

#define LCD_REPORT_VERSION	1
#define LCD_RECOVERY_BUDGET_MS	3000
#define LCD_SLEEP_OUT_MS		120
#define LCD_DISPLAY_ON_MS	40
#define LCD_INIT_EVENTS		32
#define LCD_RECOVERY_EVENTS	32
#define LCD_POWER_SLEEP_OUT	BIT(4)
#define LCD_POWER_DISPLAY_ON	BIT(2)
#define LCD_POWER_EXPECTED	(LCD_POWER_SLEEP_OUT | LCD_POWER_DISPLAY_ON)
#define LCD_IMAGE_MASK		0x38
#define LCD_IMAGE_ALL_PIXELS	0x18
#define LCD_IMAGE_INVERT		BIT(5)

enum lcd_command_bit {
	LCD_CMD_SLEEP_OUT = BIT(0),
	LCD_CMD_NORMAL = BIT(1),
	LCD_CMD_INVERT_OFF = BIT(2),
	LCD_CMD_DISPLAY_ON = BIT(3),
	LCD_CMD_REINIT = BIT(8),
};

struct lcd_status {
	bool power_valid;
	bool image_valid;
	u8 power;
	u8 image;
};

struct lcd_init_event {
	ulong ms;
	int result;
	u8 command;
};

enum lcd_recovery_op {
	LCD_TRACE_RX,
	LCD_TRACE_TX,
	LCD_TRACE_RESET_RELEASE,
	LCD_TRACE_REINIT,
};

struct lcd_recovery_event {
	ulong started_ms;
	ulong completed_ms;
	int result;
	enum lcd_recovery_op op;
	u8 command;
	u8 value;
	bool value_present;
	bool state_valid;
};

struct lcd_boot_report {
	bool checked;
	bool busy;
	bool identity_valid;
	bool snapshot_before_valid;
	bool snapshot_after_valid;
	bool abnormal_seen;
	bool needs_attention;
	bool reset_valid;
	enum vla11_lcd_profile profile;
	unsigned int raw;
	const char *state;
	const char *reason;
	ulong started_ms;
	ulong finished_ms;
	ulong reset_ms;
	ulong sleep_out_ms;
	ulong display_on_ms;
	unsigned int reset_count;
	unsigned int commands;
	unsigned int commands_ok;
	unsigned int reinit_count;
	int error;
	int soft_error;
	int reinit_error;
	struct lcd_status before;
	struct lcd_status after;
	struct vla11_lcd_dsi_snapshot snapshot_before;
	struct vla11_lcd_dsi_snapshot snapshot_after;
	struct lcd_init_event init[LCD_INIT_EVENTS];
	unsigned int init_count;
	unsigned int init_dropped;
	struct lcd_recovery_event recovery[LCD_RECOVERY_EVENTS];
	unsigned int recovery_count;
	unsigned int recovery_dropped;
};

static struct lcd_boot_report lcd_report = {
	.state = "not-checked",
	.reason = "not-called",
};

static struct lcd_recovery_event *lcd_trace_begin(enum lcd_recovery_op op,
						 u8 command)
{
	struct lcd_recovery_event *event;

	if (lcd_report.recovery_count >= LCD_RECOVERY_EVENTS) {
		lcd_report.recovery_dropped++;
		return NULL;
	}
	event = &lcd_report.recovery[lcd_report.recovery_count++];
	memset(event, 0, sizeof(*event));
	event->started_ms = get_timer(0);
	event->op = op;
	event->command = command;
	return event;
}

static void lcd_trace_end(struct lcd_recovery_event *event, int result,
			  u8 value, bool value_present, bool state_valid)
{
	if (!event)
		return;
	event->completed_ms = get_timer(0);
	event->result = result;
	event->value_present = value_present;
	event->state_valid = state_valid;
	event->value = value_present ? value : 0;
}

static int lcd_trace_format(const struct lcd_recovery_event *event,
			    char *buf, size_t size)
{
	char value[8];

	if (event->op == LCD_TRACE_RX) {
		if (event->value_present)
			snprintf(value, sizeof(value), "0x%02x", event->value);
		else
			strcpy(value, "none");
		return snprintf(buf, size,
			"start_ms=%lu end_ms=%lu op=RX cmd=%02x io_ret=%d value=%s state_valid=%u",
			event->started_ms, event->completed_ms, event->command,
			event->result, value, event->state_valid);
	}
	if (event->op == LCD_TRACE_TX)
		return snprintf(buf, size,
			"start_ms=%lu end_ms=%lu op=TX cmd=%02x host_ret=%d panel_ack=not-observed",
			event->started_ms, event->completed_ms,
			event->command, event->result);
	return snprintf(buf, size, "start_ms=%lu end_ms=%lu op=%s result=%d",
			event->started_ms, event->completed_ms,
			event->op == LCD_TRACE_REINIT ? "REINIT_LOGO" : "RESET_RELEASE",
			event->result);
}

/* Emit only after all recovery timing and physical read-back have finished. */
static void lcd_trace_print(void)
{
	char line[192];
	unsigned int i;

	printf("VLA11 LCD recovery trace: count=%u dropped=%u\n",
	       lcd_report.recovery_count, lcd_report.recovery_dropped);
	for (i = 0; i < lcd_report.recovery_count; i++) {
		lcd_trace_format(&lcd_report.recovery[i], line, sizeof(line));
		printf("VLA11 LCD event: %s\n", line);
	}
}

/* RAM only: do not put serial output or waits into the original init path. */
void vla11_lcd_record_init_command(u8 command, int result)
{
	struct lcd_init_event *event;

	if (lcd_report.init_count >= LCD_INIT_EVENTS) {
		lcd_report.init_dropped++;
		return;
	}
	event = &lcd_report.init[lcd_report.init_count++];
	event->ms = get_timer(0);
	event->command = command;
	event->result = result;
}

void vla11_lcd_record_reset_release(void)
{
	struct lcd_recovery_event *event;

	lcd_report.reset_ms = get_timer(0);
	lcd_report.reset_valid = true;
	lcd_report.reset_count++;
	event = lcd_trace_begin(LCD_TRACE_RESET_RELEASE, 0);
	lcd_trace_end(event, 0, 0, false, false);
}

static const char *lcd_profile_name(enum vla11_lcd_profile profile)
{
	switch (profile) {
	case VLA11_LCD_PROFILE_FPT: return "fpt-ttcm03921235";
	case VLA11_LCD_PROFILE_JUJING: return "jujing-jn3929595a";
	default: return "unknown";
	}
}

static bool lcd_identity_valid(unsigned int raw, enum vla11_lcd_profile profile)
{
	if (profile == VLA11_LCD_PROFILE_FPT)
		return raw <= 450;
	if (profile == VLA11_LCD_PROFILE_JUJING)
		return raw >= 950 && raw <= 4095;
	return false;
}

static int lcd_budget(void)
{
	if (get_timer(lcd_report.started_ms) >= LCD_RECOVERY_BUDGET_MS) {
		lcd_report.reason = "time-budget";
		return -ETIMEDOUT;
	}
	return 0;
}

static int lcd_wait(unsigned int ms)
{
	if (get_timer(lcd_report.started_ms) + ms >= LCD_RECOVERY_BUDGET_MS) {
		lcd_report.reason = "time-budget";
		return -ETIMEDOUT;
	}
	mdelay(ms);
	return lcd_budget();
}

/* Only the documented normal-mode power profile is actionable here. */
static bool lcd_power_valid(u8 power)
{
	return !(power & 0x03) && (power & BIT(3)) &&
	       !(power & (BIT(5) | BIT(6)));
}

static bool lcd_image_valid(u8 image)
{
	return !(image & ~LCD_IMAGE_MASK) &&
	       (image & LCD_IMAGE_ALL_PIXELS) != LCD_IMAGE_ALL_PIXELS;
}

static int lcd_read_power(struct display_state *state, struct lcd_status *status)
{
	struct lcd_recovery_event *event;
	int ret;

	status->power_valid = false;
	ret = lcd_budget();
	if (ret)
		return ret;
	event = lcd_trace_begin(LCD_TRACE_RX, 0x0a);
	ret = vla11_lcd_dsi_read(state, 0x0a, &status->power);
	lcd_trace_end(event, ret, status->power, !ret,
		      !ret && lcd_power_valid(status->power));
	if (ret) {
		lcd_report.reason = "power-read-failed";
		return ret;
	}
	if (!lcd_power_valid(status->power)) {
		lcd_report.reason = "power-value-unsupported";
		return -EPROTO;
	}
	status->power_valid = true;
	return 0;
}

static int lcd_read_status(struct display_state *state, struct lcd_status *status)
{
	struct lcd_recovery_event *event;
	int ret;

	memset(status, 0, sizeof(*status));
	ret = lcd_read_power(state, status);
	if (ret)
		return ret;
	ret = lcd_budget();
	if (ret)
		return ret;
	event = lcd_trace_begin(LCD_TRACE_RX, 0x0d);
	ret = vla11_lcd_dsi_read(state, 0x0d, &status->image);
	lcd_trace_end(event, ret, status->image, !ret,
		      !ret && lcd_image_valid(status->image));
	if (ret) {
		lcd_report.reason = "image-read-failed";
		return ret;
	}
	if (!lcd_image_valid(status->image)) {
		lcd_report.reason = "image-value-unsupported";
		return -EPROTO;
	}
	status->image_valid = true;
	return 0;
}

static int lcd_write(struct display_state *state, const u8 *payload,
		     unsigned int length, bool lpm, unsigned int command)
{
	struct lcd_recovery_event *event;
	int ret = lcd_budget();

	if (ret)
		return ret;
	lcd_report.commands |= command;
	/* An earlier read is not post-command evidence, even on TX timeout. */
	lcd_report.after.power_valid = false;
	lcd_report.after.image_valid = false;
	event = lcd_trace_begin(LCD_TRACE_TX, payload[0]);
	ret = vla11_lcd_dsi_write(state, payload, length, lpm);
	lcd_trace_end(event, ret, 0, false, false);
	if (ret) {
		lcd_report.reason = "command-write-failed";
		return ret;
	}
	lcd_report.commands_ok |= command;
	return 0;
}

static int lcd_command(struct display_state *state, u8 command,
		       unsigned int bit)
{
	return lcd_write(state, &command, 1, true, bit);
}

static int lcd_soft_recover(struct display_state *state, struct lcd_status *status,
			    bool *verified_unrestored)
{
	bool power_repair;
	ulong elapsed;
	int ret;

	*verified_unrestored = false;
	power_repair = (status->power & LCD_POWER_EXPECTED) != LCD_POWER_EXPECTED;
	if (!power_repair && !status->image)
		return 0;
	lcd_report.abnormal_seen = true;
	if (!(status->power & LCD_POWER_SLEEP_OUT)) {
		/* Unknown reset time is treated conservatively, never as elapsed. */
		elapsed = lcd_report.reset_valid ?
			get_timer(lcd_report.reset_ms) : 0;
		if (elapsed < LCD_SLEEP_OUT_MS) {
			ret = lcd_wait(LCD_SLEEP_OUT_MS - elapsed);
			if (ret)
				return ret;
		}
		lcd_report.sleep_out_ms = get_timer(0);
		ret = lcd_command(state, 0x11, LCD_CMD_SLEEP_OUT);
		if (ret)
			return ret;
		ret = lcd_wait(LCD_SLEEP_OUT_MS);
		if (ret)
			return ret;
		/* Match the verified recovery: confirm SleepOut before display-on. */
		ret = lcd_read_power(state, status);
		if (ret)
			return ret;
		if (!(status->power & LCD_POWER_SLEEP_OUT)) {
			*verified_unrestored = true;
			lcd_report.reason = "sleep-out-not-confirmed";
			return -EIO;
		}
	}
	if (!(status->power & LCD_POWER_DISPLAY_ON)) {
		lcd_report.display_on_ms = get_timer(0);
		ret = lcd_command(state, 0x29, LCD_CMD_DISPLAY_ON);
		if (ret)
			return ret;
		ret = lcd_wait(LCD_DISPLAY_ON_MS);
		if (ret)
			return ret;
	}
	if (power_repair) {
		/* Image mode is judged only from a fresh post-wake/on response. */
		ret = lcd_read_status(state, status);
		if (ret)
			return ret;
		if ((status->power & LCD_POWER_EXPECTED) != LCD_POWER_EXPECTED) {
			*verified_unrestored = true;
			lcd_report.reason = "state-not-restored";
			return -EIO;
		}
		if (!status->image)
			return 0;
	}
	if (status->image & LCD_IMAGE_ALL_PIXELS) {
		ret = lcd_command(state, 0x13, LCD_CMD_NORMAL);
		if (ret)
			return ret;
	}
	if (status->image & LCD_IMAGE_INVERT) {
		ret = lcd_command(state, 0x20, LCD_CMD_INVERT_OFF);
		if (ret)
			return ret;
	}
	ret = lcd_read_status(state, status);
	if (ret)
		return ret;
	if ((status->power & LCD_POWER_EXPECTED) != LCD_POWER_EXPECTED ||
	    status->image) {
		*verified_unrestored = true;
		lcd_report.reason = "state-not-restored";
		return -EIO;
	}
	return 0;
}

int vla11_lcd_recovery_check(struct display_state *state)
{
	struct rockchip_connector *connector;
	struct vla11_lcd_dsi_snapshot reinit_gate;
	struct lcd_recovery_event *reinit_event;
	bool verified_unrestored = false;
	int ret;

	if (!state || !state->conn_state.connector)
		return 0;
	connector = state->conn_state.connector;
	/* Other outputs do not consume the DSI0 one-shot. */
	if (state->conn_state.type != DRM_MODE_CONNECTOR_DSI ||
	    connector->id != 0 || connector->split_mode ||
	    connector->dual_channel_mode)
		return 0;
	if (lcd_report.busy)
		return -EINPROGRESS;
	if (lcd_report.checked)
		return lcd_report.error;
	if (!state->is_init || !state->is_enable)
		return -EAGAIN;

	lcd_report.checked = true;
	lcd_report.busy = true;
	lcd_report.started_ms = get_timer(0);
	lcd_report.state = "checking";
	ret = vla11_lcd_boot_identity(&lcd_report.raw, &lcd_report.profile);
	if (ret || !lcd_identity_valid(lcd_report.raw, lcd_report.profile)) {
		lcd_report.state = "skipped";
		lcd_report.reason = "identity-not-qualified";
		ret = 0;
		goto out;
	}
	lcd_report.identity_valid = true;
	ret = vla11_lcd_dsi_snapshot(state, &lcd_report.snapshot_before);
	if (ret) {
		lcd_report.reason = "snapshot-before-failed";
		goto failed;
	}
	lcd_report.snapshot_before_valid = true;
	ret = lcd_read_status(state, &lcd_report.before);
	if (ret)
		goto failed;
	lcd_report.after = lcd_report.before;
	/* The unchanged normal path has no second physical status read. */
	lcd_report.after.power_valid = false;
	lcd_report.after.image_valid = false;
	ret = lcd_soft_recover(state, &lcd_report.after, &verified_unrestored);
	if (ret)
		lcd_report.soft_error = ret;
	if (ret && verified_unrestored && lcd_report.abnormal_seen) {
		ret = lcd_budget();
		if (ret)
			goto failed;
		ret = vla11_lcd_dsi_snapshot(state, &reinit_gate);
		if (ret) {
			lcd_report.reason = "pre-reinit-snapshot-failed";
			goto failed;
		}
		/* Never reset after a transport/cleanup failure or a pending BTA. */
		if (reinit_gate.core & 0x70000) {
			ret = -EBUSY;
			lcd_report.reason = "pre-reinit-transport-busy";
			goto failed;
		}
		/* The callback coordinates stream stop/init/logo; no raw GPIO here. */
		lcd_report.reinit_count = 1;
		lcd_report.commands |= LCD_CMD_REINIT;
		lcd_report.after.power_valid = false;
		lcd_report.after.image_valid = false;
		reinit_event = lcd_trace_begin(LCD_TRACE_REINIT, 0);
		lcd_report.reinit_error = vla11_lcd_reinit_logo(state);
		lcd_trace_end(reinit_event, lcd_report.reinit_error, 0, false, false);
		if (lcd_report.reinit_error) {
			ret = lcd_report.reinit_error;
			lcd_report.reason = "reinit-logo-failed";
			goto failed;
		}
		lcd_report.commands_ok |= LCD_CMD_REINIT;
		ret = lcd_read_status(state, &lcd_report.after);
		if (!ret)
			ret = lcd_soft_recover(state, &lcd_report.after,
					       &verified_unrestored);
	}
	if (ret)
		goto failed;
	ret = vla11_lcd_dsi_snapshot(state, &lcd_report.snapshot_after);
	if (ret) {
		lcd_report.reason = "snapshot-after-failed";
		goto failed;
	}
	lcd_report.snapshot_after_valid = true;
	lcd_report.state = lcd_report.reinit_count ? "reinit-recovered" :
		lcd_report.commands ? "recovered" : "normal";
	lcd_report.reason = "registers-verified";
	goto out;

failed:
	lcd_report.state = "failed";
	lcd_report.needs_attention = true;
out:
	lcd_report.error = ret;
	lcd_report.finished_ms = get_timer(0);
	lcd_report.busy = false;
	printf("VLA11 LCD health: state=%s panel=%s raw=%u before_valid=%u/%u before=%02x/%02x after_valid=%u/%u after=%02x/%02x cmds=0x%x reinit=%u error=%d reason=%s duration=%lums\n",
	       lcd_report.state, lcd_profile_name(lcd_report.profile), lcd_report.raw,
	       lcd_report.before.power_valid, lcd_report.before.image_valid,
	       lcd_report.before.power, lcd_report.before.image,
	       lcd_report.after.power_valid, lcd_report.after.image_valid,
	       lcd_report.after.power, lcd_report.after.image, lcd_report.commands,
	       lcd_report.reinit_count, ret, lcd_report.reason,
	       lcd_report.finished_ms - lcd_report.started_ms);
	lcd_trace_print();
	return ret;
}

void vla11_lcd_recovery_report_fdt(void *blob)
{
	char summary[1200], trace[1100], snapshots[600];
	char recovery_trace[4096], event_line[192];
	unsigned int i, serialized = 0;
	bool text_truncated = false;
	size_t used = 0, available;
	int chosen, ret, written;

	if (!blob || fdt_check_header(blob))
		return;
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen < 0)
		chosen = fdt_add_subnode(blob, 0, "chosen");
	if (chosen < 0)
		return;
	snprintf(summary, sizeof(summary),
		 "version=%u state=%s checked=%u panel=%s raw=%u id_valid=%u reason=%s before_0a_valid=%u before_0d_valid=%u before_0a=0x%02x before_0d=0x%02x after_0a_valid=%u after_0d_valid=%u after_0a=0x%02x after_0d=0x%02x cmds=0x%x cmds_ok=0x%x error=%d soft_error=%d reinit_error=%d reinit_count=%u needs_attention=%u started_ms=%lu finished_ms=%lu reset_valid=%u reset_ms=%lu reset_count=%u sleep_out_ms=%lu display_on_ms=%lu snapshot_before_valid=%u snapshot_after_valid=%u init_count=%u init_dropped=%u recovery_count=%u recovery_dropped=%u",
		 LCD_REPORT_VERSION, lcd_report.state, lcd_report.checked,
		 lcd_profile_name(lcd_report.profile), lcd_report.raw,
		 lcd_report.identity_valid, lcd_report.reason,
		 lcd_report.before.power_valid, lcd_report.before.image_valid,
		 lcd_report.before.power, lcd_report.before.image,
		 lcd_report.after.power_valid, lcd_report.after.image_valid,
		 lcd_report.after.power, lcd_report.after.image,
		 lcd_report.commands, lcd_report.commands_ok, lcd_report.error,
		 lcd_report.soft_error, lcd_report.reinit_error,
		 lcd_report.reinit_count, lcd_report.needs_attention,
		 lcd_report.started_ms, lcd_report.finished_ms,
		 lcd_report.reset_valid, lcd_report.reset_ms, lcd_report.reset_count,
		 lcd_report.sleep_out_ms, lcd_report.display_on_ms,
		 lcd_report.snapshot_before_valid, lcd_report.snapshot_after_valid,
		 lcd_report.init_count, lcd_report.init_dropped,
		 lcd_report.recovery_count, lcd_report.recovery_dropped);
	ret = fdt_setprop_string(blob, chosen, "violoop,lcd-boot-health", summary);
	if (ret) {
		printf("VLA11 LCD health: FDT summary failed: %d\n", ret);
		return;
	}
	trace[0] = '\0';
	for (i = 0; i < lcd_report.init_count && used < sizeof(trace) - 1; i++) {
		available = sizeof(trace) - used;
		written = snprintf(trace + used, available, "%s%lu:%02x:%d",
				   i ? ";" : "", lcd_report.init[i].ms,
				   lcd_report.init[i].command,
				   lcd_report.init[i].result);
		if (written < 0)
			break;
		if ((size_t)written >= available) {
			used = sizeof(trace) - 1;
			break;
		}
		used += written;
	}
	/* Property insertion can move node offsets: look /chosen up again. */
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0 && fdt_setprop_string(blob, chosen,
					     "violoop,lcd-boot-init-trace", trace))
		printf("VLA11 LCD health: FDT init trace unavailable\n");
	snprintf(snapshots, sizeof(snapshots),
		 "before_valid=%u before_power=%08x before_reset=%08x before_mode=%08x before_core=%08x before_phy_clock=%08x before_video=%08x before_general=%08x before_ipi=%08x before_vc=%08x after_valid=%u after_power=%08x after_reset=%08x after_mode=%08x after_core=%08x after_phy_clock=%08x after_video=%08x after_general=%08x after_ipi=%08x after_vc=%08x",
		 lcd_report.snapshot_before_valid,
		 lcd_report.snapshot_before.power, lcd_report.snapshot_before.reset,
		 lcd_report.snapshot_before.mode, lcd_report.snapshot_before.core,
		 lcd_report.snapshot_before.phy_clock, lcd_report.snapshot_before.video,
		 lcd_report.snapshot_before.general, lcd_report.snapshot_before.ipi,
		 lcd_report.snapshot_before.vc, lcd_report.snapshot_after_valid,
		 lcd_report.snapshot_after.power, lcd_report.snapshot_after.reset,
		 lcd_report.snapshot_after.mode, lcd_report.snapshot_after.core,
		 lcd_report.snapshot_after.phy_clock, lcd_report.snapshot_after.video,
		 lcd_report.snapshot_after.general, lcd_report.snapshot_after.ipi,
		 lcd_report.snapshot_after.vc);
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0 && fdt_setprop_string(blob, chosen,
					     "violoop,lcd-boot-dsi-snapshots", snapshots))
		printf("VLA11 LCD health: FDT DSI snapshots unavailable\n");
	used = snprintf(recovery_trace, sizeof(recovery_trace),
			"version=1 count=%u dropped=%u",
			lcd_report.recovery_count, lcd_report.recovery_dropped);
	for (i = 0; i < lcd_report.recovery_count; i++) {
		written = lcd_trace_format(&lcd_report.recovery[i], event_line,
					   sizeof(event_line));
		available = sizeof(recovery_trace) - used;
		/* Reserve room for the explicit serialization/truncation trailer. */
		if (written < 0 || (size_t)written >= sizeof(event_line) ||
		    available <= 64 || (size_t)written + 2 >= available - 64) {
			text_truncated = true;
			break;
		}
		used += snprintf(recovery_trace + used, available, ";%s", event_line);
		serialized++;
	}
	snprintf(recovery_trace + used, sizeof(recovery_trace) - used,
		 ";serialized=%u text_truncated=%u", serialized, text_truncated);
	chosen = fdt_path_offset(blob, "/chosen");
	if (chosen >= 0 && fdt_setprop_string(blob, chosen,
					     "violoop,lcd-boot-recovery-trace", recovery_trace))
		printf("VLA11 LCD health: FDT recovery trace unavailable\n");
}

#endif
