// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/ucassist.c
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 *
 * This provides the kernel a way to configure uclamp values at
 * init and override UCLAMP values during FB_BLANK to save power.
 */
#define pr_fmt(fmt) "ucassist: %s: " fmt, __func__

#include <linux/sched.h>
#include <linux/input.h>
#ifdef CONFIG_FB
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/notifier.h>
#endif

#include "sched.h"

int cpu_uclamp_write_css(struct cgroup_subsys_state *css, char *buf,
					enum uclamp_id clamp_id);
int cpu_uclamp_ls_write_u64(struct cgroup_subsys_state *css,
				   struct cftype *cftype, u64 ls);

struct uclamp_data {
	char uclamp_max[3];
	char uclamp_min[3];
	bool latency_sensitive;
};

struct ucassist_struct {
	const char *name;
	struct uclamp_data data;
	bool initialized;
};

static struct ucassist_struct ucassist_data[] = {
	{
		.name = "top-app",
		.data = { "max", "1", 1 },
	},
	{
		.name = "foreground",
		.data = { "max", "0", 1 },
	},
	{
		.name = "background",
		.data = { "50", "0", 0 },
	},
	{
		.name = "system-background",
		.data = { "50", "0", 0 },
	},
	{
		.name = "dex2oat",
		.data = { "60", "0", 0 },
	},
	{
		.name = "nnapi-hal",
		.data = { "max", "50", 0 },
	},
	{
		.name = "camera-daemon",
		.data = { "max", "1", 1 },
	},
};

static void ucassist_set_uclamp_data(struct cgroup_subsys_state *css,
		struct uclamp_data cdata)
{
	cpu_uclamp_write_css(css, cdata.uclamp_max, 
				UCLAMP_MAX);

	cpu_uclamp_write_css(css, cdata.uclamp_min, 
				UCLAMP_MIN);
	
	cpu_uclamp_ls_write_u64(css, NULL, cdata.latency_sensitive);
}

int cpu_ucassist_init_values(struct cgroup_subsys_state *css)
{
	struct ucassist_struct *uc;
	int i;

	if (!css->cgroup->kn)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ucassist_data); i++) {
		uc = &ucassist_data[i];

		if (uc->initialized)
			continue;

		if (strcmp(css->cgroup->kn->name, uc->name))
			continue;

		pr_info("setting values for %s\n", uc->name);
		ucassist_set_uclamp_data(css, uc->data);

		uc->initialized = true;
	}

	return 0;
}

/* Disable UCLAMP scaling for 5 seconds after last input */
#define UCASSIST_TIMER_JIFFIES msecs_to_jiffies(5000)

#define SCHED_CAPACITY_SCALE_PERC(perc) \
		((SCHED_CAPACITY_SCALE * perc) / 100)

enum {
	INPUT_SLEEP_STATE = 0,
#ifdef CONFIG_FB
	FB_SLEEP_STATE,
#endif
	MAX_STATES,
};

static const unsigned long ucassist_scale_data[] = {
	[INPUT_SLEEP_STATE] = SCHED_CAPACITY_SCALE_PERC(75),
#ifdef CONFIG_FB
	[FB_SLEEP_STATE] = SCHED_CAPACITY_SCALE_PERC(50),
#endif
};

static unsigned long ucassist_sleep_states = 0;

static void ucassist_input_timer_func(unsigned long data)
{
	set_bit(INPUT_SLEEP_STATE, &ucassist_sleep_states);
	pr_debug("input timer expired\n");
}
static DEFINE_TIMER(ucassist_input_timer, ucassist_input_timer_func, 0, 0);

static inline void ucassist_input_trigger_timer(void)
{
	if (!mod_timer(&ucassist_input_timer, jiffies + UCASSIST_TIMER_JIFFIES)) {
		clear_bit(INPUT_SLEEP_STATE, &ucassist_sleep_states);
		pr_debug("input timer set\n");
	}
}

void ucassist_input_trigger_ext(void)
{
	ucassist_input_trigger_timer();
}
EXPORT_SYMBOL(ucassist_input_trigger_ext);

static void ucassist_input_event(struct input_handle *handle,
		unsigned int type, unsigned int code, int value)
{
	ucassist_input_trigger_timer();
}

static int ucassist_input_connect(struct input_handler *handler,
		struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpufreq";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void ucassist_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id ucassist_ids[] = {
	/* multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler ucassist_input_handler = {
	.event          = ucassist_input_event,
	.connect        = ucassist_input_connect,
	.disconnect     = ucassist_input_disconnect,
	.name           = "ucassist",
	.id_table       = ucassist_ids,
};

#ifdef CONFIG_FB
static int ucassist_fb_notifier_callback(struct notifier_block *self, 
				unsigned long event, 
				void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (event != FB_EARLY_EVENT_BLANK)
		return 0;

	if (!evdata || !evdata->data)
		return 0;

	blank = evdata->data;

	if (*blank == FB_BLANK_UNBLANK) {
		clear_bit(FB_SLEEP_STATE, &ucassist_sleep_states);
		/* Trigger input as well to prevent capping wake performance */
		ucassist_input_trigger_timer();
	} else if (*blank == FB_BLANK_POWERDOWN) {
		set_bit(FB_SLEEP_STATE, &ucassist_sleep_states);
	}

	pr_debug("sleep_states = %lu\n", ucassist_sleep_states);

	return 0;
}

static struct notifier_block ucassist_fb_notif = {
	.notifier_call = ucassist_fb_notifier_callback,
};
#endif

void ucassist_sleep_uclamp_scaling(unsigned long *val)
{
	int i;

	if (!*val)
		return;

	/* Start from the deepest state towards the shallowest */
	for (i = MAX_STATES - 1; i >= 0; i--) {
		if (test_bit(i, &ucassist_sleep_states)) {
			/* Apply a ceiling limit to the UCLAMP value */
			*val = min(*val, ucassist_scale_data[i]);
			break;
		}
	}
}

static int __init ucassist_init(void)
{
	int i, ret;

	/* Check values in the scale data for invalid values */
	for (i = MAX_STATES - 1; i >= 0; i--) {
		if (ucassist_scale_data[i] > SCHED_CAPACITY_SCALE) {
			pr_err("Scale value is more than %lu! idx = %d\n",
						SCHED_CAPACITY_SCALE, i);
			return -EINVAL;
		}
	}

	ret = input_register_handler(&ucassist_input_handler);
	if (ret) {
		pr_err("Failed to init input_handler\n");
		return ret;
	}

#ifdef CONFIG_FB
	ret = fb_register_client(&ucassist_fb_notif);
	if (ret) {
		pr_err("Failed to init fb_notifier\n");
		input_unregister_handler(&ucassist_input_handler);
		return ret;
	}
#endif

	return 0;
}
module_init(ucassist_init);
