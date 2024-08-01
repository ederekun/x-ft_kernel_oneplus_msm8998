// SPDX-License-Identifier: GPL-2.0
/*
 * power/supply/qcom/op_cg_uovp.h
 *
 * Copyright (C) 2024, Edrick Vince Sinsuan
 * 
 */
#ifndef _OP_CG_UOVP_
#define _OP_CG_UOVP_

#define CHG_SOFT_OVP_MV         5800
#define CHG_SOFT_UVP_MV         4300

void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv);
void op_cg_uovp_enable(struct smb_charger *chg, bool chg_present);

#endif /* _OP_CG_UOVP_ */