// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <typec.h>
#include <usb_dp.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>

#include <linux/gpio.h>
#include "tcpm.h"

#include "tcpm.h"

#define K_EMERG	(1<<7)
#define K_QMU	(1<<7)
#define K_ALET		(1<<6)
#define K_CRIT		(1<<5)
#define K_ERR		(1<<4)
#define K_WARNIN	(1<<3)
#define K_NOTICE	(1<<2)
#define K_INFO		(1<<1)
#define K_DEBUG	(1<<0)

#define TB132FU_DP_MUX_REG		0x06
#define TB132FU_DP_MUX_SAFE		0xA8
#define TB132FU_DP_MUX_NORMAL		0xFB
#define TB132FU_DP_MUX_FLIPPED		0xFF

#define usbc_dbg(level, fmt, args...) do { \
		if (debug_level & level) { \
			pr_info("[USB_DP]" fmt, ## args); \
		} \
	} while (0)

static u32 debug_level = (255 - K_DEBUG);

static struct i2c_client *usbdp_client;
static struct notifier_block dp_nb;
static struct tcpc_device *dp_tcpc_dev;
static int hdp_state;
static bool dp_sw_connect;

#define CHECK_HPD_DELAY 2000
static struct delayed_work check_wk;

/*
 * TB132FU uses the PI3DPX1205A's Lenovo board interface: one control byte at
 * register 0x06.  The donor 4.19 implementation treats this part as a
 * thirteen-byte register block, which makes probe fail on the tablet and
 * leaves 6-000a unbound.
 */
static int pi3dpx1205a_read_config(struct i2c_client *client)
{
	if (!client)
		return -ENODEV;

	return i2c_smbus_read_byte_data(client, TB132FU_DP_MUX_REG);
}

static int pi3dpx1205a_write_config(struct i2c_client *client, u8 value)
{
	int ret;

	if (!client)
		return -ENODEV;

	ret = i2c_smbus_write_byte_data(client, TB132FU_DP_MUX_REG, value);
	if (ret < 0)
		usbc_dbg(K_ERR, "mux write 0x%02x failed: %d\n", value, ret);
	else
		usbc_dbg(K_INFO, "mux config=0x%02x\n", value);

	return ret;
}

static int pi3dpx1205a_set_orientation(struct i2c_client *client,
				       bool flipped)
{
	return pi3dpx1205a_write_config(client, flipped ?
		TB132FU_DP_MUX_FLIPPED : TB132FU_DP_MUX_NORMAL);
}

/*
 * Lenovo's TB132FU 4.14 driver writes the normal DP mapping immediately
 * before synthesizing HPD when the sink did not generate an HPD event.  This
 * looks redundant after the polarity notification, but it is part of the
 * board's original PI3DPX1205A handoff and is required before AUX training is
 * attempted.  Keep this scoped to the forced-HPD fallback so ordinary USB3
 * orientation handling remains polarity aware.
 */
static int pi3dpx1205a_prepare_forced_hpd(struct i2c_client *client)
{
	int ret;
	int config;

	ret = pi3dpx1205a_write_config(client, TB132FU_DP_MUX_NORMAL);
	if (ret < 0)
		return ret;

	config = pi3dpx1205a_read_config(client);
	if (config < 0) {
		usbc_dbg(K_ERR, "forced HPD mux readback failed: %d\n", config);
		return config;
	}

	usbc_dbg(K_INFO, "forced HPD mux readback=0x%02x\n", config);
	if (config != TB132FU_DP_MUX_NORMAL)
		usbc_dbg(K_WARNIN,
			 "forced HPD mux mismatch: expected=0x%02x actual=0x%02x\n",
			 TB132FU_DP_MUX_NORMAL, config);

	return 0;
}

static int pi3dpx1205a_init(struct i2c_client *client)
{
	int ret;

	ret = pi3dpx1205a_read_config(client);
	if (ret < 0)
		return ret;

	return pi3dpx1205a_write_config(client, TB132FU_DP_MUX_SAFE);
}

static int usbdp_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int ret;

	ret = i2c_check_functionality(client->adapter,
						  I2C_FUNC_SMBUS_BYTE_DATA);
	usbc_dbg(K_INFO, "%s I2C functionality : %s\n", __func__,
		ret ? "ok" : "fail");

	if (!ret)
		return -EOPNOTSUPP;

	ret = pi3dpx1205a_init(client);
	if (ret < 0)
		return ret;

	usbdp_client = client;
	return 0;
}

static int usbdp_i2c_remove(struct i2c_client *client)
{
	return 0;
}

static void usbdp_shutdown(struct i2c_client *client)
{
}

/*
 * xxxxx1xx = Pin Assignment C is supported. 4 lanes
 * xxx1xxxx = Pin Assignment E is supported. 4 lanes
 * xxxx1xxx = Pin Assignment D is supported. 2 lanes
 * xx1xxxxx = Pin Assignment F is supported. 2 lanes
 */
static int chg_tcp_notifier_call(struct notifier_block *nb,
	unsigned long event, void *data)
{
	/*
	 * struct tcp_ny_ama_dp_state {
	 * uint8_t sel_config; sel_config: 0(SW_USB) / 1(SW_DFP_D) / 2(SW_UFP_D)
	 * uint8_t signal;
	 * uint8_t pin_assignment;
	 * uint8_t polarity; polarity: 0 for up side, 1 back side.
	 * uint8_t active;
	 * };
	 */
	struct tcp_notify *noti = data;

	usbc_dbg(K_INFO, "%s event=%x", __func__, event);

	if (event == TCP_NOTIFY_AMA_DP_STATE) {
		uint8_t signal = noti->ama_dp_state.signal;
		uint8_t pin = noti->ama_dp_state.pin_assignment;
		uint8_t polarity = noti->ama_dp_state.polarity;
		uint8_t active = noti->ama_dp_state.active;

		if (!active) {
			usbc_dbg(K_INFO, "%s Not active", __func__);
			return NOTIFY_OK;
		}

		usbc_dbg(K_INFO, "TCP_NOTIFY_AMA_DP_STATE signal:%x pin:%x polarity:%x\n",
			signal, pin, polarity);

		if (pin != 4 && pin != 8 && pin != 16 && pin != 32) {
			usbc_dbg(K_INFO, "%s: pin_assignment not support\n",
				__func__);
			return NOTIFY_OK;
		}

		pi3dpx1205a_set_orientation(usbdp_client, polarity);

		hdp_state = 0;
		schedule_delayed_work(&check_wk, msecs_to_jiffies(CHECK_HPD_DELAY));
	} else if (event == TCP_NOTIFY_AMA_DP_HPD_STATE) {
		uint8_t irq = noti->ama_dp_hpd_state.irq;
		uint8_t state = noti->ama_dp_hpd_state.state;

		usbc_dbg(K_INFO, "TCP_NOTIFY_AMA_DP_HPD_STATE irq:%x state:%x\n",
			irq, state);

		hdp_state = state;

		if (state) {
			if (irq) {
				if (dp_sw_connect == false) {
					usbc_dbg(K_INFO, "Force connect\n");
					mtk_dp_SWInterruptSet(0x4);
					dp_sw_connect = true;
				}
				mtk_dp_SWInterruptSet(0x8);
			} else {
				mtk_dp_SWInterruptSet(0x4);
				dp_sw_connect = true;
			}
		} else {
			mtk_dp_SWInterruptSet(0x2);
			dp_sw_connect = false;
		}
	} else if (event == TCP_NOTIFY_TYPEC_STATE) {
		pi3dpx1205a_set_orientation(usbdp_client,
			noti->typec_state.polarity);

		if ((noti->typec_state.old_state == TYPEC_ATTACHED_SRC ||
			noti->typec_state.old_state == TYPEC_ATTACHED_SNK) &&
			noti->typec_state.new_state == TYPEC_UNATTACHED) {
			usbc_dbg(K_INFO, "Plug out\n");
			mtk_dp_SWInterruptSet(0x2);
			dp_sw_connect = false;
			pi3dpx1205a_write_config(usbdp_client,
				TB132FU_DP_MUX_SAFE);
		}
	}

	return NOTIFY_OK;
}

static const struct i2c_device_id pi3dpx1205a_id_table[] = {
	{"PI3DPX1205A", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, pi3dpx1205a_id_table);

static const struct of_device_id usb_dp_ids[] = {
	{.compatible = "mediatek,usb_dp",},
	{},
};

static struct i2c_driver usb_dp_driver = {
	.driver = {
		.name = "usb_dp",
		.owner = THIS_MODULE,
		.of_match_table = usb_dp_ids,
		.pm = (NULL),
	},
	.probe = usbdp_i2c_probe,
	.remove = usbdp_i2c_remove,
	.shutdown = usbdp_shutdown,
	.id_table = pi3dpx1205a_id_table,
};

static void check_hpd(struct work_struct *work)
{
	if (hdp_state == 0) {
		int ret;

		usbc_dbg(K_INFO, "%s No HPD connection event\n", __func__);
		ret = pi3dpx1205a_prepare_forced_hpd(usbdp_client);
		if (ret < 0)
			usbc_dbg(K_ERR, "%s mux preparation failed: %d\n",
				 __func__, ret);
		mtk_dp_SWInterruptSet(0x4);
		dp_sw_connect = true;
	}
}

static int __init usb_dp_init(void)
{
	struct device_node *np;

	usbc_dbg(K_INFO, "%s: initializing...\n", __func__);

	dp_tcpc_dev = tcpc_dev_get_by_name("type_c_port0");
	if (!dp_tcpc_dev) {
		usbc_dbg(K_ERR, "%s get tcpc device type_c_port0 fail\n",
			__func__);
		return -ENODEV;
	}

	dp_nb.notifier_call = chg_tcp_notifier_call;
	register_tcp_dev_notifier(dp_tcpc_dev, &dp_nb, TCP_NOTIFY_TYPE_MODE |
		TCP_NOTIFY_TYPE_VBUS | TCP_NOTIFY_TYPE_USB |
		TCP_NOTIFY_TYPE_MISC);

	np = of_find_node_by_name(NULL, "usb_dp");
	if (np != NULL)
		usbc_dbg(K_INFO, "usb_dp node found...\n");
	else
		usbc_dbg(K_ERR, "usb_dp node not found...\n");

	INIT_DEFERRABLE_WORK(&check_wk, check_hpd);
	dp_sw_connect = false;

	return i2c_add_driver(&usb_dp_driver);
}
late_initcall(usb_dp_init);

static void __exit usb_dp_exit(void)
{
	i2c_del_driver(&usb_dp_driver);
}
module_exit(usb_dp_exit);

void usb3_switch_dps_en(bool en)
{
	usbc_dbg(K_DEBUG, "%s en=%d\n", __func__, en);
}

void usb3_switch_ctrl_sel(int sel)
{
	int res = 0;

	usbc_dbg(K_INFO, "en=%d\n", sel);

	if (sel == CC1_SIDE)
		res = pi3dpx1205a_set_orientation(usbdp_client, false);
	else if (sel == CC2_SIDE)
		res = pi3dpx1205a_set_orientation(usbdp_client, true);

	if (res < 0)
		usbc_dbg(K_ERR, "%s fail\n", __func__);
}

struct usbdp_pin_ctrl dp_pctrl;

static int usbdp_pinctrl_probe(struct platform_device *pdev)
{
	int ret = 0;
	int pwr_ret;
	struct device_node *np;
	const char *platform;

	usbc_dbg(K_INFO, "%s: initializing...\n", __func__);

	dp_pctrl.pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(dp_pctrl.pinctrl)) {
		usbc_dbg(K_ERR, "Cannot find usb pinctrl!\n");
		return -EINVAL;
	}

	dp_pctrl.pwr_en = pinctrl_lookup_state(dp_pctrl.pinctrl, "dp_pwr_high");

	if (IS_ERR(dp_pctrl.pwr_en)) {
		usbc_dbg(K_ERR, "Can *NOT* find pwr_en\n");
		ret = -1;
	} else {
		usbc_dbg(K_DEBUG, "Find pwr_en\n");
		pinctrl_select_state(dp_pctrl.pinctrl, dp_pctrl.pwr_en);
	}

	dp_pctrl.ext_pwr_en = pinctrl_lookup_state(dp_pctrl.pinctrl,
		"dp_ext_pwr_high");

	if (IS_ERR(dp_pctrl.ext_pwr_en)) {
		usbc_dbg(K_ERR, "Can *NOT* find ext_pwr_en\n");
		ret = -1;
	} else {
		usbc_dbg(K_DEBUG, "Find ext_pwr_en\n");
		pinctrl_select_state(dp_pctrl.pinctrl, dp_pctrl.ext_pwr_en);
	}

	/*
	 * Both states control GPIO191 on TB132FU: dp_pwr_high drives it high,
	 * while the misleadingly named dp_ext_pwr_high drives it low.  Lenovo's
	 * original sequence deliberately finishes by selecting dp_pwr_high a
	 * second time.  Without this final transition the DP retimer remains
	 * unpowered and every AUX transaction times out.
	 */
	if (!IS_ERR(dp_pctrl.pwr_en)) {
		pwr_ret = pinctrl_select_state(dp_pctrl.pinctrl,
					       dp_pctrl.pwr_en);
		if (pwr_ret < 0) {
			usbc_dbg(K_ERR, "final DP power enable failed: %d\n",
				 pwr_ret);
			if (!ret)
				ret = pwr_ret;
		} else {
			usbc_dbg(K_INFO, "final DP power enable selected\n");
		}
	}

	np = of_find_node_by_name(pdev->dev.of_node, "usb_dp-data");
	if (np) {
		usbc_dbg(K_DEBUG, "%s: find usb_dp-data\n", __func__);
		ret = of_property_read_string(np, "platform", &platform);

		if (ret < 0)
			usbc_dbg(K_ERR, "Get platform info fail\n");

		usbc_dbg(K_DEBUG, "%s: find platform info %s\n",
			__func__, platform);
	}

	return ret;
}

static const struct of_device_id usb_pinctrl_ids[] = {
	{.compatible = "mediatek,usb_dp_pinctrl",},
	{},
};

static struct platform_driver usb_dp_pinctrl_driver = {
	.probe = usbdp_pinctrl_probe,
	.driver = {
		.name = "usbdp_pinctrl",
#ifdef CONFIG_OF
		.of_match_table = usb_pinctrl_ids,
#endif
	},
};

module_platform_driver(usb_dp_pinctrl_driver);
