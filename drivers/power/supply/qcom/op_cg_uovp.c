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

#include <linux/power_supply.h>
#include <linux/slab.h>
#include "smb-reg.h"
#include "smb-lib.h"
#include "op_cg_uovp.h"

#define CURRENT_CEIL_DEFAULT   1500000 /* DCP_CURRENT_UA (normal) = 1.5A */
#define CURRENT_FLOOR_UA       500000  /* SDP_CURRENT_UA = 500mA */

#define CHG_SOFT_OVP_HYST_MV   100

#define DETECT_CNT             3

#define NO_CHARGER_BIT         0

struct op_cg_current_table {
	int apsd_bit;
	int max_ichg_ua;
};

struct op_cg_uovp_data {
	struct smb_charger *chg;

	int uovp_cnt;
	int not_uovp_cnt;

	int vchg_mv;
	int current_ua;
	int no_uovp_current_ua;

	bool last_uovp_state;
	bool uovp_state;
	bool is_overvolt;
	bool lock_current;
	bool enable;
};

/* Table of apsd bits with their corresponding max current uA */
static const struct op_cg_current_table op_cg_current_data[] = {
	{ SDP_CHARGER_BIT, 		CURRENT_FLOOR_UA 	},
 	{ NO_CHARGER_BIT, 		900000 				},
 	{ CDP_CHARGER_BIT, 		1500000 			},
	{ DCP_CHARGER_BIT | 
	  FLOAT_CHARGER_BIT | 
	  OCP_CHARGER_BIT, 		3000000 			},
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

	ret = smblib_set_icl_current(chg, ichg_ua);
	if (ret)
		pr_err("can't set charger max current, ret=%d", ret);

	return ret;
}

static int op_cg_current_inc_dec(struct op_cg_uovp_data *opdata,
				bool increase)
{
	struct smb_charger *chg = opdata->chg;
	int ichg_ua, i, ret = 0;

	ret = smblib_get_icl_current(chg, &ichg_ua);
	if (ret) {
		pr_err("can't get charger max current, ret=%d", ret);
		goto err;
	}

	pr_info("smblib_ichg_ua=%d", ichg_ua);
	opdata->current_ua = ichg_ua;

	if (increase) {
		int apsd_bit = op_get_apsd_bit(chg);
		int ceil_ichg_ua = CURRENT_CEIL_DEFAULT;
		bool ichg_ua_set = false;

		for (i = 0; i < ARRAY_SIZE(op_cg_current_data); i++) {
			const struct op_cg_current_table *d = &op_cg_current_data[i];

			if (apsd_bit & d->apsd_bit)
				ceil_ichg_ua = d->max_ichg_ua;

			if (ichg_ua_set)
				continue;

			if (d->max_ichg_ua > ichg_ua) {
				ichg_ua = d->max_ichg_ua;
				ichg_ua_set = true;
			}
		}
		ichg_ua = min(ichg_ua, ceil_ichg_ua);
		if (ichg_ua == ceil_ichg_ua && opdata->not_uovp_cnt >= DETECT_CNT) {
			opdata->lock_current = true;
			pr_info("max current has been reached - locked ichg_ua=%d", 
					opdata->no_uovp_current_ua);
		}
	} else {
		for (i = ARRAY_SIZE(op_cg_current_data) - 1; i >= 0; i--) {
			const struct op_cg_current_table *d = &op_cg_current_data[i];

			if (d->max_ichg_ua < ichg_ua) {
				ichg_ua = d->max_ichg_ua;
				break;
			}
		}
		ichg_ua = max(CURRENT_FLOOR_UA, ichg_ua);
	}

	if (opdata->current_ua != ichg_ua) {
		ret = op_cg_current_set(opdata, ichg_ua);
	} else {
		pr_err("ichg_ua already at %d mA", (ichg_ua / 1000));
		ret = -EINVAL;
	}
err:
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

	if (opdata->not_uovp_cnt)
		opdata->not_uovp_cnt = 0;

	if (opdata->last_uovp_state)
		opdata->uovp_cnt++;

	/* Restore last known good current and lock */
	if (!opdata->last_uovp_state && opdata->no_uovp_current_ua) {
		ret = op_cg_current_set(opdata, opdata->no_uovp_current_ua);
		if (ret)
			goto err;
		opdata->lock_current = true;
		pr_info("current has been locked to %d", 
				opdata->no_uovp_current_ua);
		return;
	}

	pr_info("uovp_state=%d last_uovp_state=%d uovp_cnt=%d",
		opdata->uovp_state, opdata->last_uovp_state, opdata->uovp_cnt);

	/* Increase the current if over, decrease if under */
	ret = op_cg_current_inc_dec(opdata, opdata->is_overvolt);
err:
	/* Only call cutoff if current control fails */
	if (ret && !chg->chg_ovp)
		op_cg_uovp_cutoff(opdata);
}

static void op_cg_detect_normal(struct op_cg_uovp_data *opdata)
{
	struct smb_charger *chg = opdata->chg;
	bool is_uovp = false;

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

	if (opdata->lock_current)
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
	} else {
		/* Increase the current if not undervolt for @DETECT_CNT iterations */
		if (opdata->not_uovp_cnt >= DETECT_CNT) {
			opdata->no_uovp_current_ua = opdata->current_ua;
			op_cg_current_inc_dec(opdata, true);
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
	pr_info("vchg_mv=%d", vchg_mv);

	if (!op_uovp_data.enable)
		return;

	if (!chg->vbus_present) {
		pr_info("no vbus present, skip uovp");
		return;
	}

	op_uovp_data.vchg_mv = vchg_mv;
	op_cg_handle_uovp(&op_uovp_data);
}

void op_cg_uovp_enable(struct smb_charger *chg, bool chg_present)
{
	if (op_uovp_data.enable == chg_present)
		return;

	if (chg_present) {
		op_uovp_data.chg = chg;
		op_uovp_data.enable = true;
		pr_info("UOVP is enabled");
	} else {
		op_uovp_data.chg = NULL;
		op_uovp_data.uovp_cnt = 0;
		op_uovp_data.not_uovp_cnt = 0;
		op_uovp_data.vchg_mv = 0;
		op_uovp_data.current_ua = 0;
		op_uovp_data.no_uovp_current_ua = 0;
		op_uovp_data.last_uovp_state = false;
		op_uovp_data.uovp_state = false;
		op_uovp_data.is_overvolt = false;
		op_uovp_data.lock_current = false;
		op_uovp_data.enable = false;
		pr_info("UOVP is disabled");
	}
}
