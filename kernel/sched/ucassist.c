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

#ifdef CONFIG_FB
#define UCASSIST_SLEEP_UCLAMP_MIN	0
#define UCASSIST_SLEEP_UCLAMP_MAX	(SCHED_CAPACITY_SCALE >> 2) /* 25% of max capacity */

static bool ucassist_sleep_state = false;

static int ucassist_fb_notifier_callback(struct notifier_block *self, 
				unsigned long event, 
				void *data)
{
	struct fb_event *evdata = data;
	bool prev_state = ucassist_sleep_state;
	int *blank;

	if (event != FB_EARLY_EVENT_BLANK)
		return 0;

	if (!evdata || !evdata->data)
		return 0;

	blank = evdata->data;

	if (*blank == FB_BLANK_UNBLANK)
		ucassist_sleep_state = false;
	else if (*blank == FB_BLANK_POWERDOWN)
		ucassist_sleep_state = true;

	if (prev_state != ucassist_sleep_state)
		pr_info("sleep state = %d\n", ucassist_sleep_state);

	return 0;
}

static int __init ucassist_init(void)
{
	struct notifier_block *fb_notif;
	int ret;

	fb_notif = kzalloc(sizeof(*fb_notif), GFP_KERNEL);
	if (!fb_notif) {
		pr_err("Failed to allocate fb_notif\n");
		return 0;
	}

	fb_notif->notifier_call = ucassist_fb_notifier_callback;
	ret = fb_register_client(fb_notif);
	if (ret) {
		pr_err("Failed to init fb_notifier\n");
		kfree(fb_notif);
	}

	return 0;
}
module_init(ucassist_init);

bool ucassist_sleep_uclamp_override(enum uclamp_id clamp_id)
{
	return ucassist_sleep_state;
}

unsigned long ucassist_sleep_uclamp_val(enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return UCASSIST_SLEEP_UCLAMP_MIN;

	return UCASSIST_SLEEP_UCLAMP_MAX;
}
#else
bool ucassist_sleep_uclamp_override(enum uclamp_id clamp_id)
{
	return false;
}

unsigned long ucassist_sleep_uclamp_val(enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}
#endif
