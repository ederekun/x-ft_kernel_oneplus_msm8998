// SPDX-License-Identifier: GPL-2.0
/*
 * power/supply/qcom/op_cg_uovp.c
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 *
 * This provides USB charger under/overvoltage protection through 
 * current limiting for the OnePlus 5/T.
 */
#define pr_fmt(fmt) "SMBLIB: %s: " fmt, __func__

#include <linux/pmic-voter.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include "smb-reg.h"
#include "smb-lib.h"
#include "op_cg_uovp.h"

#define CURRENT_CEIL_DEFAULT   1500000 /* DCP_CURRENT_UA (normal) = 1.5A */
#define CURRENT_FLOOR_UA       500000  /* SDP_CURRENT_UA = 500mA */
#define CURRENT_DIFF_UA        250000  /* At least 250mA */

#define CHG_SOFT_OVP_HYST_MV   100

#define DETECT_CNT             3

#define NO_CHARGER_BIT         0
#define FAST_CHARGER_BITS \
	(DCP_CHARGER_BIT | FLOAT_CHARGER_BIT | OCP_CHARGER_BIT)

struct op_cg_current_table {
	int max_ichg_ua;
	int apsd_bit;
};

struct op_cg_uovp_data {
	struct smb_charger *chg;

	int uovp_cnt;
	int not_uovp_cnt;

	int vchg_mv;
	int current_ua;

	int apsd_bit;

	bool last_uovp_state;
	bool uovp_state;
	bool not_uovp_limit;
	bool is_overvolt;
	bool config_en;

	bool initialized;
	bool enable;
};

/* Table of max currents uA with their supported apsd bit */
static const struct op_cg_current_table op_cg_current_data[] = {
	{ CURRENT_FLOOR_UA, SDP_CHARGER_BIT   },
	{ 750000,           NO_CHARGER_BIT    },
	{ 1000000,          NO_CHARGER_BIT    },
	{ 1250000,          NO_CHARGER_BIT    },
	{ 1500000,          CDP_CHARGER_BIT   },
	{ 2000000,          NO_CHARGER_BIT    },
	{ 2500000,          NO_CHARGER_BIT    },
	{ 3000000,          FAST_CHARGER_BITS },
};

static struct op_cg_uovp_data op_uovp_data;

static void op_cg_uovp_cutoff(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	if (opdata->uovp_cnt <= DETECT_CNT)
		return;

	pr_info("charger is over voltage, stop charging");
	op_charging_en(chg, false);
	chg->chg_ovp = true;
}

static void op_cg_uovp_restore(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;

	if (opdata->not_uovp_cnt <= DETECT_CNT)
		return;

	pr_info("charger voltage is back to normal");
	op_charging_en(chg, true);
	op_check_battery_temp(chg);
	smblib_rerun_aicl(chg);
	chg->chg_ovp = false;
}

static int op_cg_current_set(struct op_cg_uovp_data *opdata,
				int ichg_ua)
{
	struct smb_charger *chg = opdata->chg;
	int ret = 0;

	pr_info("voting ichg_ua=%d", ichg_ua);

	ret = vote(chg->usb_icl_votable, DCP_VOTER,
					true, ichg_ua);
	if (ret) {
		pr_err("can't set charger max current, ret=%d", ret);
		goto err;
	}

	power_supply_changed(chg->usb_psy);
err:
	return ret;
}

static int op_cg_configure_uvp(struct op_cg_uovp_data *opdata,
				bool enable)
{
	struct smb_charger *chg = opdata->chg;
	int ret = 0;

	if (opdata->config_en == enable)
		return ret;

	/* Don't configure for SDP */
	ret = opdata->apsd_bit & SDP_CHARGER_BIT;
	if (ret)
		return ret;

	pr_info("UVP config, en=%d", enable);

	/* Disable USB suspend on collapse */
	ret = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			SUSPEND_ON_COLLAPSE_USBIN_BIT,
			enable ? 0 : SUSPEND_ON_COLLAPSE_USBIN_BIT);
	if (ret < 0) {
		pr_err("Couldn't set SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n", 
				ret);
		goto err;
	}
	pr_info("Set SUSPEND_ON_COLLAPSE_USBIN_BIT to en=%d", !enable);

	smblib_rerun_apsd_if_required(chg);
	smblib_rerun_aicl(chg);
	opdata->config_en = enable;
err:
	return ret;
}

static int op_cg_current_inc_dec(struct op_cg_uovp_data *opdata,
				bool increase)
{
	struct smb_charger *chg = opdata->chg;
	int ceil_ichg_ua = CURRENT_CEIL_DEFAULT;
	int ichg_ua, i, ret = 0;

	ichg_ua = get_effective_result(chg->usb_icl_votable);

	pr_info("smblib_ichg_ua=%d", ichg_ua);
	opdata->current_ua = ichg_ua;

	if (increase) {
		for (i = 0; i < ARRAY_SIZE(op_cg_current_data); i++) {
			const struct op_cg_current_table *d = &op_cg_current_data[i];

			if (opdata->apsd_bit & d->apsd_bit)
				ceil_ichg_ua = d->max_ichg_ua;
		}
		pr_info("ceil_ichg_ua=%d", ceil_ichg_ua);

		for (i = 0; i < ARRAY_SIZE(op_cg_current_data); i++) {
			const struct op_cg_current_table *d = &op_cg_current_data[i];

			if (d->max_ichg_ua >= ceil_ichg_ua) {
				ichg_ua = ceil_ichg_ua;
				break;
			}

			if (d->max_ichg_ua >= ichg_ua + CURRENT_DIFF_UA) {
				ichg_ua = d->max_ichg_ua;
				break;
			}
		}
	} else {
		for (i = ARRAY_SIZE(op_cg_current_data) - 1; i >= 0; i--) {
			const struct op_cg_current_table *d = &op_cg_current_data[i];

			if (d->max_ichg_ua == CURRENT_FLOOR_UA) {
				ichg_ua = CURRENT_FLOOR_UA;
				break;
			}

			if (d->max_ichg_ua <= ichg_ua - CURRENT_DIFF_UA) {
				ichg_ua = d->max_ichg_ua;
				break;
			}
		}
	}

	if (opdata->current_ua != ichg_ua) {
		ret = op_cg_current_set(opdata, ichg_ua);
	} else {
		pr_err("ichg_ua already at %d mA", (ichg_ua / 1000));
		ret = -EINVAL;
	}
	return ret;
}

static void op_cg_detect_uovp(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	bool is_uovp = false;
	int ret = 0;

	is_uovp = opdata->is_overvolt = (opdata->vchg_mv > CHG_SOFT_OVP_MV);

	if (!opdata->is_overvolt)
		is_uovp = (opdata->vchg_mv <= CHG_SOFT_UVP_MV);

	if (!is_uovp)
		return;

	pr_info("charger is %svoltage count=%d voltage %d",
		opdata->is_overvolt ? "over" : "under", 
		opdata->uovp_cnt, opdata->vchg_mv);

	opdata->uovp_state = true;
	opdata->not_uovp_limit = false;

	if (opdata->not_uovp_cnt)
		opdata->not_uovp_cnt = 0;

	if (opdata->last_uovp_state)
		opdata->uovp_cnt++;

	if (!opdata->is_overvolt && !opdata->config_en) {
		ret = op_cg_configure_uvp(opdata, true);
		if (!ret)
			return;
	}

	pr_info("uovp_state=%d last_uovp_state=%d uovp_cnt=%d",
		opdata->uovp_state, opdata->last_uovp_state, opdata->uovp_cnt);

	/* Increase the current if over, decrease if under */
	ret = op_cg_current_inc_dec(opdata, opdata->is_overvolt);

	/* Only call cutoff if current control fails */
	if (ret && !chg->chg_ovp)
		op_cg_uovp_cutoff(opdata);
}

static void op_cg_detect_normal(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	bool is_uovp = false;
	int ret = 0;

	is_uovp = opdata->is_overvolt = 
		!(opdata->vchg_mv < 
			CHG_SOFT_OVP_MV - 
			CHG_SOFT_OVP_HYST_MV);

	if (!opdata->is_overvolt)
		is_uovp = !(opdata->vchg_mv > 
				CHG_SOFT_UVP_MV + 
				CHG_SOFT_OVP_HYST_MV);

	if (is_uovp)
		return;

	opdata->uovp_state = false;

	if (opdata->uovp_cnt)
		opdata->uovp_cnt = 0;

	if (!opdata->last_uovp_state)
		opdata->not_uovp_cnt++;

	pr_info("uovp_state=%d last_uovp_state=%d not_uovp_cnt=%d",
		opdata->uovp_state, opdata->last_uovp_state,
			opdata->not_uovp_cnt);

	if (chg->chg_ovp) {
		op_cg_uovp_restore(opdata);
	} else if (!opdata->not_uovp_limit) {
		/* Increase the current if not undervolt for @DETECT_CNT iterations */
		if (opdata->not_uovp_cnt >= DETECT_CNT) {
			opdata->not_uovp_cnt = 0;
			ret = op_cg_current_inc_dec(opdata, true);
			if (ret)
				opdata->not_uovp_limit = true;
		}
	}
}

static void op_cg_handle_uovp(struct op_cg_uovp_data *opdata)
{
	op_cg_detect_uovp(opdata);

	/* Check normal if it did not transition from !uovp -> uovp */
	if (!(opdata->uovp_state && !opdata->last_uovp_state))
		op_cg_detect_normal(opdata);

	opdata->last_uovp_state = opdata->uovp_state;
}

void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	pr_info("vchg_mv=%d", vchg_mv);

	if (!opdata->initialized)
		return;

	if (!chg->vbus_present) {
		pr_info("no vbus present, skip uovp");
		return;
	}

	/* Wait for the charger to settle */
	if (!opdata->enable) {
		opdata->enable = true;
		return;
	}

	opdata->vchg_mv = vchg_mv;
	op_cg_handle_uovp(opdata);
}

void op_cg_uovp_enable(struct smb_charger *chg, bool chg_present)
{
	struct op_cg_uovp_data *opdata = &op_uovp_data;

	if (opdata->initialized == chg_present)
		return;

	if (chg_present) {
		opdata->chg = chg;
		opdata->apsd_bit = op_get_apsd_bit(chg);
		opdata->initialized = true;
		pr_info("UOVP is enabled, apsd_bit=0x%d", opdata->apsd_bit);
	} else {
		chg->chg_ovp = false;
		op_cg_configure_uvp(&op_uovp_data, false);
		memset(opdata, 0, sizeof(*opdata));
		pr_info("UOVP is disabled");
	}
}
