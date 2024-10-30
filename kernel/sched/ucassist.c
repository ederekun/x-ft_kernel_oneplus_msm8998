// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/sched/ucassist.c
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 *
 * This provides the kernel a way to configure uclamp
 * values at init.
 */
#define pr_fmt(fmt) "ucassist: %s: " fmt, __func__

#include <linux/sched.h>

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

		pr_info("setting values for %s", uc->name);
		ucassist_set_uclamp_data(css, uc->data);

		uc->initialized = true;
	}

	return 0;
}
