// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 */


#include <linux/device.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#if IS_ENABLED(CONFIG_REGULATOR_MT6359P)
#include <linux/mfd/mt6359p/registers.h>
#endif
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>


#define VIB_DEVICE "regulator_vibrator"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " %s(%d) :" fmt, __func__, __LINE__

#define DEFAULT_MIN_LIMIT 15
#define VIBR_MAX_LEVELS 48
/* RG_VIBR_VOCAL raises the VIBR output in 10 mV steps, codes 0 to 10. */
#define VIBR_TRIM_STEP_UV 10000
#define VIBR_TRIM_MAX 10
/* Duty-cycle period; an ERM rotor time constant of tens of ms smooths it. */
#define VIBR_PWM_PERIOD_MS 20
#define VIBR_DUTY_MAX 100

struct reg_vibr_config {
	unsigned int min_limit;
	unsigned int max_limit;
	unsigned int min_volt;
	unsigned int max_volt;
	/* Output levels inside [min_volt, max_volt], ascending, in microvolts:
	 * each is a regulator voltage (base_uv) plus trim * VIBR_TRIM_STEP_UV.
	 */
	unsigned int levels[VIBR_MAX_LEVELS];
	unsigned int base_uv[VIBR_MAX_LEVELS];
	u8 trim[VIBR_MAX_LEVELS];
	unsigned int nlevels;
	/* PMIC register map for RG_VIBR_VOCAL; NULL disables trim levels. */
	struct regmap *trim_map;
	unsigned int applied_trim;
	struct regulator *reg;
};

struct reg_vibr {
	struct workqueue_struct *vibr_queue;
	struct work_struct vibr_work;
	struct hrtimer vibr_timer;
	spinlock_t vibr_lock;
	unsigned int vibr_dur;
	bool vibr_active;
	bool vibr_state;
	bool vibr_oc_state;
	bool reg_status;
	bool vibr_shutdown;
	struct reg_vibr_config vibr_conf;
	struct notifier_block oc_handle;
	/* Index into vibr_conf.levels applied at the next enable. */
	unsigned int level;
	unsigned int applied_uv;
	/* Percent of each VIBR_PWM_PERIOD_MS the supply stays on. */
	unsigned int duty;
	bool pwm_on;
	struct hrtimer pwm_timer;
};

static int mt_vibra_parse_dt(struct device *dev,
		struct reg_vibr_config *vibr_conf)
{
	int ret;

	ret = of_property_read_u32(dev->of_node, "min-limit",
		&(vibr_conf->min_limit));
	if (ret)
		vibr_conf->min_limit = DEFAULT_MIN_LIMIT;
	vibr_conf->min_limit = max_t(unsigned int,
		vibr_conf->min_limit, DEFAULT_MIN_LIMIT);

	ret = of_property_read_u32(dev->of_node, "max-limit",
		&(vibr_conf->max_limit));
	if (ret)
		vibr_conf->max_limit = 0;

	if (!vibr_conf->max_limit &&
		vibr_conf->max_limit < vibr_conf->min_limit) {
		pr_notice("Error load dts: get error limitation(min > max)!\n");
		ret = -EINVAL;
		return ret;
	}

	vibr_conf->reg = devm_regulator_get(dev, "vib");
	if (IS_ERR(vibr_conf->reg)) {
		ret = PTR_ERR(vibr_conf->reg);
		pr_notice("Error load dts: get regulator return %d\n", ret);
		vibr_conf->reg = NULL;
		//return ret;
	}

	ret = of_property_read_u32(dev->of_node, "max-volt",
		&vibr_conf->max_volt);
	if (ret) {
		pr_notice("Error load dts: get max-volt failed!\n");
		ret = -EINVAL;
		return ret;
	}

	ret = of_property_read_u32(dev->of_node, "min-volt",
		&vibr_conf->min_volt);
	if (ret) {
		pr_notice("Error load dts: get min-volt failed!\n");
		ret = -EINVAL;
		return ret;
	}

	if (vibr_conf->min_volt > vibr_conf->max_volt) {
		pr_notice("Error load dts: get error voltage(min > max)!\n");
		ret = -EINVAL;
		return ret;
	}

	pr_info("vibr_conf = %u, %u, %u-%u\n",
		vibr_conf->min_limit, vibr_conf->max_limit,
		vibr_conf->min_volt, vibr_conf->max_volt);

	return ret;

}

static void vibr_insert_level(struct reg_vibr_config *conf,
		unsigned int base, unsigned int trim)
{
	unsigned int uv = base + trim * VIBR_TRIM_STEP_UV;
	unsigned int j;

	if (uv < conf->min_volt || uv > conf->max_volt ||
	    conf->nlevels >= VIBR_MAX_LEVELS)
		return;
	for (j = 0; j < conf->nlevels; j++)
		if (conf->levels[j] == uv)
			return;
	j = conf->nlevels++;
	while (j > 0 && conf->levels[j - 1] > uv) {
		conf->levels[j] = conf->levels[j - 1];
		conf->base_uv[j] = conf->base_uv[j - 1];
		conf->trim[j] = conf->trim[j - 1];
		j--;
	}
	conf->levels[j] = uv;
	conf->base_uv[j] = base;
	conf->trim[j] = trim;
}

/*
 * The trim field shares VIBR_ANA_CON0 with the voltage select field. Trim
 * levels open only when the select code read back through the regulator's
 * register map names the voltage the regulator reports, so a different
 * register layout leaves the plain regulator steps in place, and only when
 * the trim reads zero, so a factory calibration stays untouched.
 */
static struct regmap *vibr_trim_probe(struct reg_vibr_config *conf)
{
#if IS_ENABLED(CONFIG_REGULATOR_MT6359P)
	struct regmap *map;
	unsigned int val, vosel, vocal;
	int cur;

	map = regulator_get_regmap(conf->reg);
	if (IS_ERR_OR_NULL(map))
		return NULL;
	if (regmap_read(map, PMIC_RG_VIBR_VOSEL_ADDR, &val))
		return NULL;
	vosel = (val >> PMIC_RG_VIBR_VOSEL_SHIFT) & PMIC_RG_VIBR_VOSEL_MASK;
	cur = regulator_get_voltage(conf->reg);
	if (cur <= 0 || regulator_list_voltage(conf->reg, vosel) != cur) {
		pr_notice("trim disabled: vosel %u does not select %d uV\n",
			  vosel, cur);
		return NULL;
	}
	if (regmap_read(map, PMIC_RG_VIBR_VOCAL_ADDR, &val))
		return NULL;
	vocal = (val >> PMIC_RG_VIBR_VOCAL_SHIFT) & PMIC_RG_VIBR_VOCAL_MASK;
	if (vocal) {
		pr_notice("trim disabled: calibration code %u in use\n", vocal);
		return NULL;
	}
	return map;
#else
	return NULL;
#endif
}

static int vibr_write_trim(struct reg_vibr_config *conf, unsigned int trim)
{
#if IS_ENABLED(CONFIG_REGULATOR_MT6359P)
	if (conf->trim_map == NULL || trim == conf->applied_trim)
		return 0;
	if (regmap_update_bits(conf->trim_map, PMIC_RG_VIBR_VOCAL_ADDR,
			       PMIC_RG_VIBR_VOCAL_MASK <<
			       PMIC_RG_VIBR_VOCAL_SHIFT,
			       trim << PMIC_RG_VIBR_VOCAL_SHIFT))
		return -EIO;
	conf->applied_trim = trim;
#endif
	return 0;
}

static void vibr_collect_levels(struct reg_vibr_config *conf)
{
	int count, i, uv;
	unsigned int t, u;

	conf->nlevels = 0;
	conf->applied_trim = 0;
	if (conf->reg == NULL)
		return;
	conf->trim_map = vibr_trim_probe(conf);

	count = regulator_count_voltages(conf->reg);
	for (i = 0; i < count && conf->nlevels < VIBR_MAX_LEVELS; i++) {
		/* regulator_list_voltage() returns 0 for table holes. */
		uv = regulator_list_voltage(conf->reg, i);
		if (uv <= 0)
			continue;
		u = uv;
		if (u < conf->min_volt || u > conf->max_volt)
			continue;
		vibr_insert_level(conf, u, 0);
	}
	/* Trimmed levels after every plain step, so a plain step wins a tie. */
	for (i = 0; conf->trim_map && i < count; i++) {
		uv = regulator_list_voltage(conf->reg, i);
		if (uv <= 0)
			continue;
		for (t = 1; t <= VIBR_TRIM_MAX; t++)
			vibr_insert_level(conf, uv, t);
	}
	pr_info("output levels in range: %u, trim %s\n", conf->nlevels,
		conf->trim_map ? "on" : "off");
}

static int vibr_apply_level(struct reg_vibr *vibr)
{
	unsigned int level = READ_ONCE(vibr->level);
	unsigned int uv;
	int ret;
	struct reg_vibr_config *conf = &vibr->vibr_conf;

	if (vibr->vibr_conf.reg == NULL || !vibr->vibr_conf.nlevels)
		return 0;
	if (level >= vibr->vibr_conf.nlevels)
		level = vibr->vibr_conf.nlevels - 1;
	uv = conf->base_uv[level];
	if (uv != vibr->applied_uv) {
		ret = regulator_set_voltage(conf->reg, uv, uv);
		if (ret < 0) {
			pr_notice("set voltage %u fail, ret = %d\n", uv, ret);
			return ret;
		}
		vibr->applied_uv = uv;
	}
	ret = vibr_write_trim(conf, conf->trim[level]);
	if (ret < 0)
		pr_notice("set trim %u fail, ret = %d\n", conf->trim[level], ret);
	return ret;
}

static int vibr_power_set(struct reg_vibr *vibr)
{
	int ret = 0;

	if (vibr->vibr_conf.reg == NULL)
		return ret;

	if (vibr->vibr_conf.nlevels)
		return vibr_apply_level(vibr);

	pr_info("set voltage = %u-%u\n",
		vibr->vibr_conf.min_volt, vibr->vibr_conf.max_volt);
	ret = regulator_set_voltage(vibr->vibr_conf.reg,
		vibr->vibr_conf.min_volt, vibr->vibr_conf.max_volt);
	if (ret < 0)
		pr_notice("set voltage fail, ret = %d\n", ret);

	return ret;
}

static void vibr_enable(struct reg_vibr *vibr)
{
	if (vibr->vibr_conf.reg == NULL)
		return;

	if (!vibr->reg_status) {
		if (regulator_enable(vibr->vibr_conf.reg))
			pr_notice("set vibr_reg enable failed!\n");
		else
			vibr->reg_status = 1;
	} else {
		pr_debug("vibr_reg already enabled.\n");
	}
}

static void vibr_disable(struct reg_vibr *vibr)
{
	if (vibr->vibr_conf.reg == NULL)
		return;

	if (vibr->reg_status) {
		if (regulator_disable(vibr->vibr_conf.reg))
			pr_notice("set vibr_reg disable failed!\n");
		else
			vibr->reg_status = 0;
	} else {
		pr_debug("vibr_reg already disabled.\n");
	}
}

static void update_vibrator(struct work_struct *work)
{

	struct reg_vibr *vibr = container_of(work, struct reg_vibr, vibr_work);

	pr_debug("vibr_state = %d pwm_on = %d\n", vibr->vibr_state,
		vibr->pwm_on);

	if (vibr->vibr_oc_state) {
		vibr->vibr_oc_state = false;
		vibr_disable(vibr);
		return;
	}

	if (!READ_ONCE(vibr->vibr_state) || !READ_ONCE(vibr->pwm_on)) {
		vibr_disable(vibr);
		return;
	}
	vibr_apply_level(vibr);
	vibr_enable(vibr);
}

static u64 vibr_pwm_on_ns(unsigned int duty)
{
	return (u64)VIBR_PWM_PERIOD_MS * NSEC_PER_MSEC * duty / VIBR_DUTY_MAX;
}

static enum hrtimer_restart vibr_pwm_timer_func(struct hrtimer *timer)
{
	struct reg_vibr *vibr = container_of(timer, struct reg_vibr, pwm_timer);
	unsigned int duty = READ_ONCE(vibr->duty);
	u64 on_ns = vibr_pwm_on_ns(duty);
	u64 next_ns;

	if (!READ_ONCE(vibr->vibr_state))
		return HRTIMER_NORESTART;

	vibr->pwm_on = !vibr->pwm_on;
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
	next_ns = vibr->pwm_on ? on_ns :
		(u64)VIBR_PWM_PERIOD_MS * NSEC_PER_MSEC - on_ns;
	hrtimer_forward_now(timer, ns_to_ktime(next_ns));
	return HRTIMER_RESTART;
}

static void vibrator_enable(struct reg_vibr *vibr,
	unsigned int dur, unsigned int activate)
{
	unsigned long flags;

	pr_info("cancel hrtimer, cust:%u-%u, dur:%u, shutdown:%d\n",
		vibr->vibr_conf.min_limit, vibr->vibr_conf.max_limit,
		dur, vibr->vibr_shutdown);
	spin_lock_irqsave(&vibr->vibr_lock, flags);
	hrtimer_cancel(&vibr->vibr_timer);
	hrtimer_cancel(&vibr->pwm_timer);

	if (!activate || vibr->vibr_shutdown || !dur) {
		vibr->vibr_state = 0;
		vibr->pwm_on = false;
	} else {
		dur = max(vibr->vibr_conf.min_limit, dur);
		if (vibr->vibr_conf.max_limit)
			dur = min(dur, vibr->vibr_conf.max_limit);
		vibr->vibr_state = 1;
		vibr->pwm_on = true;
		hrtimer_start(&vibr->vibr_timer,
			      ktime_set(dur / 1000, (dur % 1000) * 1000000),
			      HRTIMER_MODE_REL);
		if (vibr->duty < VIBR_DUTY_MAX)
			hrtimer_start(&vibr->pwm_timer,
				      ns_to_ktime(vibr_pwm_on_ns(vibr->duty)),
				      HRTIMER_MODE_REL);
	}
	spin_unlock_irqrestore(&vibr->vibr_lock, flags);
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
}

static enum hrtimer_restart mtk_vibrator_timer_func(struct hrtimer *timer)
{
	struct reg_vibr *vibr = container_of(timer,
		struct reg_vibr, vibr_timer);

	vibr->vibr_state = 0;
	queue_work(vibr->vibr_queue, &vibr->vibr_work);
	return HRTIMER_NORESTART;
}

static const struct of_device_id vibr_of_ids[] = {
	{ .compatible = "regulator-vibrator", },
	{}
};

static ssize_t activate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%d\n", vibr->vibr_active);
}

static ssize_t activate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int activate, duration;
	ssize_t ret;
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	ret = kstrtouint(buf, 10, &activate);
	if (ret) {
		pr_notice("set activate fail\n");
		return ret;
	}
	duration = vibr->vibr_dur;
	pr_info("set activate duration = %u, %u\n",
		activate, duration);
	vibrator_enable(vibr, duration, activate);

	ret = size;
	return ret;
}

static ssize_t state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%d\n", vibr->vibr_state);
}

static ssize_t state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int state;
	ssize_t ret;
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		pr_notice("set state fail\n");
		return ret;
	}

	vibr->vibr_state = state;
	ret = size;
	return ret;
}

static ssize_t duration_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%u\n", vibr->vibr_dur);
}

static ssize_t duration_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int activate, duration;
	ssize_t ret;
	struct reg_vibr *vibr;

	ret = kstrtouint(buf, 10, &duration);
	if (ret) {
		pr_notice("set duration fail!\n");
		return ret;
	}
	vibr = dev_get_drvdata(dev->parent);
	vibr->vibr_dur = duration;
	activate = vibr->vibr_active;
	pr_debug("set activate duration = %u, %u\n",
		activate, duration);

	ret = size;
	return ret;
}

static ssize_t level_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%u\n", READ_ONCE(vibr->level));
}

static ssize_t level_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);
	unsigned int level;
	ssize_t ret;

	ret = kstrtouint(buf, 10, &level);
	if (ret)
		return ret;
	if (level >= max_t(unsigned int, vibr->vibr_conf.nlevels, 1))
		return -EINVAL;
	WRITE_ONCE(vibr->level, level);
	return size;
}

static ssize_t levels_uv_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < vibr->vibr_conf.nlevels; i++)
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%u",
				 i ? " " : "", vibr->vibr_conf.levels[i]);
	len += scnprintf(buf + len, PAGE_SIZE - len, "\n");
	return len;
}

static ssize_t duty_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);

	return sprintf(buf, "%u\n", READ_ONCE(vibr->duty));
}

static ssize_t duty_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct reg_vibr *vibr = dev_get_drvdata(dev->parent);
	unsigned int duty;
	ssize_t ret;

	ret = kstrtouint(buf, 10, &duty);
	if (ret)
		return ret;
	if (!duty || duty > VIBR_DUTY_MAX)
		return -EINVAL;
	/* Takes effect at the next activation. */
	WRITE_ONCE(vibr->duty, duty);
	return size;
}

static DEVICE_ATTR_RW(activate);
static DEVICE_ATTR_RW(state);
static DEVICE_ATTR_RW(duration);
static DEVICE_ATTR_RW(level);
static DEVICE_ATTR_RO(levels_uv);
static DEVICE_ATTR_RW(duty);

static struct attribute *activate_attrs[] = {
	&dev_attr_activate.attr,
	&dev_attr_level.attr,
	&dev_attr_levels_uv.attr,
	&dev_attr_duty.attr,
	NULL,
};

static struct attribute *state_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};

static struct attribute *duration_attrs[] = {
	&dev_attr_duration.attr,
	NULL,
};

static struct attribute_group activate_group = {
	.attrs = activate_attrs,
};

static struct attribute_group state_group = {
	.attrs = state_attrs,
};

static struct attribute_group duration_group = {
	.attrs = duration_attrs,
};

static const struct attribute_group *vibr_group[] = {
	&activate_group,
	&state_group,
	&duration_group,
	NULL
};

static struct led_classdev led_vibr = {
	.name		= "vibrator",
	.groups		= vibr_group,
};

static int regulator_oc_event(struct notifier_block *nb,
	unsigned long event, void *data)
{
	struct reg_vibr *vibr = container_of(nb, struct reg_vibr, oc_handle);

	switch (event) {
	case REGULATOR_EVENT_OVER_CURRENT:
	case REGULATOR_EVENT_FAIL:
		pr_info("get regulator oc event: %lu", event);
		vibr->vibr_oc_state = true;
		schedule_work(&vibr->vibr_work);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static int vib_probe(struct platform_device *pdev)
{
	int ret;
	struct reg_vibr *m_vibr;

	pr_info("probe start +++");
	m_vibr = devm_kzalloc(&pdev->dev, sizeof(struct reg_vibr), GFP_KERNEL);
	if (!m_vibr) {
		ret = -ENOMEM;
		goto err;
	}
	m_vibr->vibr_queue = create_singlethread_workqueue(VIB_DEVICE);
	if (!m_vibr->vibr_queue) {
		ret = -ENOMEM;
		pr_notice("unable to create workqueue!\n");
		goto err;
	}

	ret = mt_vibra_parse_dt(&(pdev->dev), &(m_vibr->vibr_conf));
	if (ret) {
		pr_notice("failed to parse devicetree(%d)!\n", ret);
		goto err;
	}

	INIT_WORK(&m_vibr->vibr_work, update_vibrator);
	spin_lock_init(&m_vibr->vibr_lock);
	m_vibr->vibr_shutdown = 0;
	m_vibr->duty = VIBR_DUTY_MAX;
	m_vibr->pwm_on = false;
	vibr_collect_levels(&m_vibr->vibr_conf);
	m_vibr->oc_handle.notifier_call = regulator_oc_event;
	if (m_vibr->vibr_conf.reg == NULL) {
		m_vibr->reg_status = 0;
	} else {
		if (regulator_is_enabled(m_vibr->vibr_conf.reg))
			m_vibr->reg_status = 1;
		else
			m_vibr->reg_status = 0;
		/* register oc notification for this regulator */
		if (devm_regulator_register_notifier(m_vibr->vibr_conf.reg,
			&m_vibr->oc_handle))
			pr_info("regulator notifier request failed\n");
	}
	hrtimer_init(&(m_vibr->vibr_timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	m_vibr->vibr_timer.function = mtk_vibrator_timer_func;
	hrtimer_init(&m_vibr->pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	m_vibr->pwm_timer.function = vibr_pwm_timer_func;
	ret = devm_led_classdev_register(&pdev->dev, &led_vibr);
	if (ret < 0) {
		pr_info("led class register fail\n");
		goto err;
	}

	platform_set_drvdata(pdev, m_vibr);
	ret = vibr_power_set(m_vibr);
	if (ret < 0) {
		pr_info("set voltage for regulator fail\n");
		goto err;
	}
	pr_info("probe success, end ---");
	return 0;

err:
	pr_notice("probe failed(%d), end ---!\n", ret);
	return ret;

}

static int vib_remove(struct platform_device *pdev)
{
	struct reg_vibr *vibr = platform_get_drvdata(pdev);

	hrtimer_cancel(&vibr->pwm_timer);
	hrtimer_cancel(&vibr->vibr_timer);
	cancel_work_sync(&vibr->vibr_work);
	vibr_write_trim(&vibr->vibr_conf, 0);
	devm_led_classdev_unregister(&pdev->dev, &led_vibr);

	return 0;
}

static void vib_shutdown(struct platform_device *pdev)
{
	unsigned long flags;
	struct reg_vibr *vibr = platform_get_drvdata(pdev);

	pr_info("shutdown: enter!\n");
	spin_lock_irqsave(&vibr->vibr_lock, flags);
	vibr->vibr_shutdown = 1;
	hrtimer_cancel(&vibr->pwm_timer);
	if (vibr->vibr_state) {
		vibr->vibr_state = 0;
		spin_unlock_irqrestore(&vibr->vibr_lock, flags);
		pr_info("vibrator will disable!\n");
		vibr_disable(vibr);
	} else {
		spin_unlock_irqrestore(&vibr->vibr_lock, flags);
	}
	vibr_write_trim(&vibr->vibr_conf, 0);
}

static struct platform_driver vibrator_driver = {
	.probe = vib_probe,
	.remove = vib_remove,
	.shutdown = vib_shutdown,
	.driver = {
			.name = VIB_DEVICE,
			.of_match_table = vibr_of_ids,
		   },
};

module_platform_driver(vibrator_driver);
MODULE_AUTHOR("Mediatek Corporation");
MODULE_DESCRIPTION("MTK Vibrator Driver (VIB)");
MODULE_LICENSE("GPL");
