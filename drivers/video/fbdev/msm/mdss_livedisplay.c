/*
 * Copyright (c) 2015 The CyanogenMod Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * MDSS DSI Panel Modes support and Auto HBM by Edrick Sinsuan
 * (c) 2024 Edrick Sinsuan <evcsinsuan@gmail.com>
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "mdss_dsi.h"
#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "mdss_mdp_pp.h"
#include "mdss_livedisplay.h"

/*
 * LiveDisplay is the display management service in CyanogenMod. It uses
 * various capabilities of the hardware and software in order to
 * optimize the experience for ambient conditions and time of day.
 *
 * This module is initialized by mdss_fb for each panel, and creates
 * several new controls in /sys/class/graphics/fbX based on the
 * configuration in the devicetree.
 *
 * cabc: Content Adaptive Backlight Control. Must be configured
 *      in the panel devicetree. Up to three levels.
 * sre: Sunlight Readability Enhancement. Must be configured in
 *      the panel devicetree. Up to three levels.
 * aco: Automatic Contrast Optimization. Must be configured in
 *      the panel devicetree. Boolean.
 *
 * hbm: Auto High Brightness Mode. Common for OLED panels. Boolean.
 *      Automatically turns on at 80% and above of max backlight level.
 *
 * preset: Arbitrary DSI commands, up to 10 may be configured.
 *      Useful for gamma calibration.
 *
 * color_enhance: Hardware color enhancement. Must be configured
 *      in the panel devicetree. Boolean.
 */

static inline bool mdss_livedisplay_get_hbm_flag(struct mdss_livedisplay_ctx *mlc, 
		unsigned int flag)
{
	return (mlc->hbm_flags & flag);
}

static inline bool mdss_livedisplay_hbm_should_update(struct mdss_livedisplay_ctx *mlc)
{
	bool hbm_active, hbm_allowed;

	hbm_active = mdss_livedisplay_get_hbm_flag(mlc, HBM_ACTIVE);
	hbm_allowed = mdss_livedisplay_get_hbm_flag(mlc, HBM_ENABLED | HBM_ALLOWED);

	return hbm_active != hbm_allowed;
}

static inline void mdss_livedisplay_set_hbm_flag(struct mdss_livedisplay_ctx *mlc, 
		unsigned int flag, bool state)
{
	DEFINE_RAW_SPINLOCK(hbm_write_lock);

	raw_spin_lock(&hbm_write_lock);
	if (state)
		mlc->hbm_flags |= flag;
	else
		mlc->hbm_flags &= ~flag;
	raw_spin_unlock(&hbm_write_lock);
}

extern void mdss_dsi_panel_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl,
		struct dsi_panel_cmds *pcmds, u32 flags);

static int parse_dsi_cmds(struct mdss_livedisplay_ctx *mlc,
		struct dsi_panel_cmds *pcmds, const uint8_t *cmd, int blen)
{
	int len;
	char *buf, *bp;
	struct dsi_ctrl_hdr *dchdr;
	int i, cnt;

	buf = kzalloc(blen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, cmd, blen);

	/* scan dcs commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len >= sizeof(*dchdr)) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		dchdr->dlen = ntohs(dchdr->dlen);
		if (dchdr->dlen > len) {
			pr_err("%s: dtsi cmd=%x error, len=%d\n",
				__func__, dchdr->dtype, dchdr->dlen);
			goto exit_free;
		}
		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
	}

	if (len != 0) {
		pr_err("%s: dcs_cmd=%x len=%d error!\n",
				__func__, buf[0], blen);
		goto exit_free;
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc),
			GFP_KERNEL);
	if (!pcmds->cmds)
		goto exit_free;

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
	}

	pcmds->link_state = mlc->link_state;

	pr_debug("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
		pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	return 0;

exit_free:
	kfree(buf);
	return -ENOMEM;
}

/*
 * Update all or a subset of parameters
 */
int mdss_livedisplay_update(struct mdss_dsi_ctrl_pdata *ctrl_pdata,
		int types)
{
	int ret = 0, i;
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_livedisplay_ctx *mlc = NULL;
	unsigned int len = 0, dlen = 0;
	struct dsi_panel_cmds dsi_cmds;
	uint8_t cabc_value = 0;
	uint8_t *cmd_buf;
	bool hbm_state;

	if (ctrl_pdata == NULL)
		return -ENODEV;

	pinfo = &(ctrl_pdata->panel_data.panel_info);
	if (pinfo == NULL)
		return -ENODEV;

	mlc = pinfo->livedisplay;
	if (mlc == NULL)
		return -ENODEV;

	if (!mlc->caps || !mdss_panel_is_power_on_interactive(pinfo->panel_power_state))
		return 0;

	// First find the length of the command array
	if ((mlc->caps & MODE_PRESET) && (types & MODE_PRESET)) {
		for (i = 0; i < mlc->num_presets; i++) {
			if (mlc->preset & BIT(i))
				len += mlc->presets_len[i];
		}
	}

	if ((mlc->caps & MODE_COLOR_ENHANCE) && (types & MODE_COLOR_ENHANCE))
		len += mlc->ce_enabled ? mlc->ce_on_cmds_len : mlc->ce_off_cmds_len;

	if ((mlc->caps & MODE_HIGH_BRIGHTNESS) && (types & MODE_HIGH_BRIGHTNESS)) {
		if (mdss_livedisplay_hbm_should_update(mlc)) {
			hbm_state = !mdss_livedisplay_get_hbm_flag(mlc, HBM_ACTIVE);
			len += hbm_state ? mlc->hbm_on_cmds_len : mlc->hbm_off_cmds_len;
		}
	}

	if (is_cabc_cmd(types) && is_cabc_cmd(mlc->caps)) {

		// The CABC command on most modern panels is also responsible for
		// other features such as SRE and ACO.  The register fields are bits
		// and are OR'd together and sent in a single DSI command.
		if (mlc->cabc_level == CABC_UI) {
			if (mlc->unified_cabc_cmds)
				cabc_value |= mlc->cabc_ui_value;
			else
				len += mlc->cabc_ui_cmds_len;
		} else if (mlc->cabc_level == CABC_IMAGE) {
			if (mlc->unified_cabc_cmds)
				cabc_value |= mlc->cabc_image_value;
			else
				len += mlc->cabc_image_cmds_len;
		} else if (mlc->cabc_level == CABC_VIDEO) {
			if (mlc->unified_cabc_cmds)
				cabc_value |= mlc->cabc_video_value;
			else
				len += mlc->cabc_video_cmds_len;
		}

		if (mlc->sre_level == SRE_WEAK)
			cabc_value |= mlc->sre_weak_value;
		else if (mlc->sre_level == SRE_MEDIUM)
			cabc_value |= mlc->sre_medium_value;
		else if (mlc->sre_level == SRE_STRONG)
			cabc_value |= mlc->sre_strong_value;

		if (mlc->aco_enabled)
			cabc_value |= mlc->aco_value;

		if (cabc_value || mlc->cabc_level == CABC_OFF)
			len += mlc->cabc_cmds_len;

		pr_debug("%s cabc=%d sre=%d aco=%d cmd=%d\n", __func__,
				mlc->cabc_level, mlc->sre_level, mlc->aco_enabled,
				cabc_value);
	}

	len += mlc->post_cmds_len;

	if (len == 0)
		return 0;

	memset(&dsi_cmds, 0, sizeof(struct dsi_panel_cmds));
	cmd_buf = kzalloc(len + 1, GFP_KERNEL);
	if (!cmd_buf)
		return -ENOMEM;

	// Build the command as a single chain, preset first
	if ((mlc->caps & MODE_PRESET) && (types & MODE_PRESET)) {
		for (i = 0; i < mlc->num_presets; i++) {
			if (mlc->preset & BIT(i)) {
				memcpy(cmd_buf + dlen, mlc->presets[i], mlc->presets_len[i]);
				dlen += mlc->presets_len[i];
			}
		}
	}

	// Color enhancement
	if ((mlc->caps & MODE_COLOR_ENHANCE) && (types & MODE_COLOR_ENHANCE)) {
		if (mlc->ce_enabled) {
			memcpy(cmd_buf + dlen, mlc->ce_on_cmds, mlc->ce_on_cmds_len);
			dlen += mlc->ce_on_cmds_len;
		} else {
			memcpy(cmd_buf + dlen, mlc->ce_off_cmds, mlc->ce_off_cmds_len);
			dlen += mlc->ce_off_cmds_len;
		}
	}

	// High brightness mode
	if ((mlc->caps & MODE_HIGH_BRIGHTNESS) && (types & MODE_HIGH_BRIGHTNESS)) {
		if (mdss_livedisplay_hbm_should_update(mlc)) {
			mdss_livedisplay_set_hbm_flag(mlc, HBM_ACTIVE, hbm_state);
			if (hbm_state) {
				memcpy(cmd_buf + dlen, mlc->hbm_on_cmds, mlc->hbm_on_cmds_len);
				dlen += mlc->hbm_on_cmds_len;
			} else {
				memcpy(cmd_buf + dlen, mlc->hbm_off_cmds, mlc->hbm_off_cmds_len);
				dlen += mlc->hbm_off_cmds_len;
			}
		}
	}

	// CABC/SRE/ACO features
	if (is_cabc_cmd(types) && mlc->cabc_cmds_len) {
		if (cabc_value || mlc->cabc_level == CABC_OFF) {
			memcpy(cmd_buf + dlen, mlc->cabc_cmds, mlc->cabc_cmds_len);
			dlen += mlc->cabc_cmds_len;
			// The CABC command parameter is the last value in the sequence
			cmd_buf[dlen - 1] = cabc_value;
		}

		if (!mlc->unified_cabc_cmds) {
			if (mlc->cabc_level == CABC_UI && mlc->cabc_ui_cmds_len) {
				memcpy(cmd_buf + dlen, mlc->cabc_ui_cmds, mlc->cabc_ui_cmds_len);
				dlen += mlc->cabc_ui_cmds_len;
			} else if (mlc->cabc_level == CABC_IMAGE && mlc->cabc_image_cmds_len) {
				memcpy(cmd_buf + dlen, mlc->cabc_image_cmds, mlc->cabc_image_cmds_len);
				dlen += mlc->cabc_image_cmds_len;
			} else if (mlc->cabc_level == CABC_VIDEO && mlc->cabc_video_cmds_len) {
				memcpy(cmd_buf + dlen, mlc->cabc_video_cmds, mlc->cabc_video_cmds_len);
				dlen += mlc->cabc_video_cmds_len;
			}
		}
	}

	// And the post_cmd, can be used to turn on the panel
	if (mlc->post_cmds_len) {
		memcpy(cmd_buf + dlen, mlc->post_cmds, mlc->post_cmds_len);
		dlen += mlc->post_cmds_len;
	}

	// Parse the command and send it
	ret = parse_dsi_cmds(mlc, &dsi_cmds, (const uint8_t *)cmd_buf, len);
	if (ret == 0) {
		mdss_dsi_panel_cmds_send(ctrl_pdata, &dsi_cmds,
				CMD_REQ_COMMIT | CMD_CLK_CTRL);
		kfree(dsi_cmds.buf);
		kfree(dsi_cmds.cmds);
	} else {
		pr_err("%s: error parsing DSI command! ret=%d", __func__, ret);
	}

	kfree(cmd_buf);

	return ret;
}

int mdss_livedisplay_event(struct msm_fb_data_type *mfd, int types)
{
	int rc = 0;
	struct mdss_panel_data *pdata;

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!pdata)
		return -ENODEV;

	do {
		if (pdata->event_handler)
			rc = pdata->event_handler(pdata, MDSS_EVENT_UPDATE_LIVEDISPLAY,
					(void *)(unsigned long) types);

		pdata = pdata->next;
	} while (!rc && pdata);

	return rc;
}

void mdss_livedisplay_set_panel_hbm_allowed(struct mdss_dsi_ctrl_pdata *ctrl_pdata, 
		int is_allowed)
{
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_livedisplay_ctx *mlc = NULL;

	pinfo = &(ctrl_pdata->panel_data.panel_info);
	if (pinfo == NULL)
		return;

	mlc = pinfo->livedisplay;
	if (mlc == NULL)
		return;

	mdss_livedisplay_set_hbm_flag(mlc, HBM_ALLOWED, !!is_allowed);
	if (mdss_livedisplay_hbm_should_update(mlc))
		mdss_livedisplay_update(ctrl_pdata, MODE_HIGH_BRIGHTNESS);
}

int mdss_livedisplay_get_panel_mode(struct mdss_dsi_ctrl_pdata *ctrl_pdata, 
		int mode)
{
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_livedisplay_ctx *mlc = NULL;

	pinfo = &(ctrl_pdata->panel_data.panel_info);
	if (pinfo == NULL)
		return false;

	mlc = pinfo->livedisplay;
	if (mlc == NULL)
		return false;

	return !!(mlc->preset & BIT(mode));
}

static void mdss_livedisplay_exclusive_presets(struct mdss_livedisplay_ctx *mlc,
		int mode)
{
	switch (mode) {
		case DSI_PANEL_MODE_SRGB:
			mlc->preset &= ~BIT(DSI_PANEL_MODE_DCI_P3);
			break;
		case DSI_PANEL_MODE_DCI_P3:
			mlc->preset &= ~BIT(DSI_PANEL_MODE_SRGB);
			break;
	}
}

void mdss_livedisplay_set_panel_mode(struct mdss_dsi_ctrl_pdata *ctrl_pdata, 
		int mode, int enable)
{
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_livedisplay_ctx *mlc = NULL;

	pinfo = &(ctrl_pdata->panel_data.panel_info);
	if (pinfo == NULL)
		return;

	mlc = pinfo->livedisplay;
	if (mlc == NULL)
		return;

	mutex_lock(&ctrl_pdata->panel_mode_lock);
	if (!ctrl_pdata->is_panel_on) {
		mutex_unlock(&ctrl_pdata->panel_mode_lock);
		return;
	}

	if (!!enable) {
		mlc->preset |= BIT(mode);
		mdss_livedisplay_exclusive_presets(mlc, mode);
	} else {
		mlc->preset &= ~BIT(mode);
		mlc->preset |= BIT(DSI_PANEL_MODE_OFF);
	}

	mdss_livedisplay_update(ctrl_pdata, MODE_PRESET);

	// Always clear panel mode off when using panel modes
	mlc->preset &= ~BIT(DSI_PANEL_MODE_OFF);

	mutex_unlock(&ctrl_pdata->panel_mode_lock);
}

static ssize_t mdss_livedisplay_get_cabc(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mlc->cabc_level);
}

static ssize_t mdss_livedisplay_set_cabc(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int level = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	sscanf(buf, "%du", &level);
	if (level >= CABC_OFF && level < CABC_MAX &&
				level != mlc->cabc_level) {
		mlc->cabc_level = level;
		mdss_livedisplay_event(mfd, MODE_CABC);
	}

	return count;
}

static ssize_t mdss_livedisplay_get_sre(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mlc->sre_level);
}

static ssize_t mdss_livedisplay_set_sre(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int level = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	sscanf(buf, "%du", &level);
	if (level >= SRE_OFF && level < SRE_MAX &&
				level != mlc->sre_level) {
		mlc->sre_level = level;
		mdss_livedisplay_event(mfd, MODE_SRE);
	}

	return count;
}

static ssize_t mdss_livedisplay_get_hbm(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mdss_livedisplay_get_hbm_flag(mlc, HBM_ENABLED));
}

static ssize_t mdss_livedisplay_set_hbm(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int value = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);
	int hbm_enabled = mdss_livedisplay_get_hbm_flag(mlc, HBM_ENABLED);

	sscanf(buf, "%du", &value);
	if ((value == 0 || value == 1)
			&& value != hbm_enabled) {
		mdss_livedisplay_set_hbm_flag(mlc, HBM_ENABLED, !!value);
		if (mdss_livedisplay_hbm_should_update(mlc))
			mdss_livedisplay_event(mfd, MODE_HIGH_BRIGHTNESS);
	}

	return count;
}

static ssize_t mdss_livedisplay_get_color_enhance(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mlc->ce_enabled);
}

static ssize_t mdss_livedisplay_set_color_enhance(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int value = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	sscanf(buf, "%du", &value);
	if ((value == 0 || value == 1)
			&& value != mlc->ce_enabled) {
		mlc->ce_enabled = value;
		mdss_livedisplay_event(mfd, MODE_COLOR_ENHANCE);
	}

	return count;
}

static ssize_t mdss_livedisplay_get_aco(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mlc->aco_enabled);
}

static ssize_t mdss_livedisplay_set_aco(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int value = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	sscanf(buf, "%du", &value);
	if ((value == 0 || value == 1)
			&& value != mlc->aco_enabled) {
		mlc->aco_enabled = value;
		mdss_livedisplay_event(mfd, MODE_AUTO_CONTRAST);
	}

	return count;
}

static ssize_t mdss_livedisplay_get_preset(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);
	int i;

	// Highest preset takes precedence
	for (i = mlc->num_presets - 1; i >= 0; i--) {
		if (mlc->preset & BIT(i))
			break;
	}

	return sprintf(buf, "%d\n", i);
}

static ssize_t mdss_livedisplay_set_preset(struct device *dev,
							   struct device_attribute *attr,
							   const char *buf, size_t count)
{
	int value = 0;
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	sscanf(buf, "%du", &value);
	if (value < 0 || value >= mlc->num_presets)
		return -EINVAL;

	// Livedisplay preset does not allow stacking
	mlc->preset = BIT(value);
	mdss_livedisplay_event(mfd, MODE_PRESET);

	return count;
}

static ssize_t mdss_livedisplay_get_num_presets(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct fb_info *fbi = dev_get_drvdata(dev);
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)fbi->par;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	return sprintf(buf, "%d\n", mlc->num_presets);
}

static DEVICE_ATTR(cabc, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_cabc, mdss_livedisplay_set_cabc);
static DEVICE_ATTR(sre, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_sre, mdss_livedisplay_set_sre);
static DEVICE_ATTR(color_enhance, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_color_enhance, mdss_livedisplay_set_color_enhance);
static DEVICE_ATTR(aco, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_aco, mdss_livedisplay_set_aco);
static DEVICE_ATTR(preset, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_preset, mdss_livedisplay_set_preset);
static DEVICE_ATTR(num_presets, S_IRUGO, mdss_livedisplay_get_num_presets, NULL);
static DEVICE_ATTR(hbm, S_IRUGO | S_IWUSR | S_IWGRP, mdss_livedisplay_get_hbm, mdss_livedisplay_set_hbm);

struct of_property_switch_cmds {
	char cmd_on_name[128];
	char cmd_off_name[128];
};

struct of_property_cmd {
	int idx;
	char cmd_name[128];
};

static const struct of_property_switch_cmds hbm_props[] = {
	{
		.cmd_on_name = "cm,mdss-livedisplay-hbm-on-cmd",
		.cmd_off_name = "cm,mdss-livedisplay-hbm-off-cmd"
	},
	{
		.cmd_on_name = "qcom,mdss-dsi-panel-hbm-on-command",
		.cmd_off_name = "qcom,mdss-dsi-panel-hbm-off-command"
	},
	{ }
};

static const struct of_property_cmd dsi_presets[] = {
	{ 
		.idx = DSI_PANEL_MODE_OFF,
		.cmd_name = "qcom,mdss-dsi-panel-srgb-off-command",
	},
	{ 
		.idx = DSI_PANEL_MODE_SRGB,
		.cmd_name = "qcom,mdss-dsi-panel-srgb-on-command",
	},
	{ 
		.idx = DSI_PANEL_MODE_DCI_P3,
		.cmd_name = "qcom,mdss-dsi-panel-dci-p3-on-command",
	},
	{ 
		.idx = DSI_PANEL_MODE_ONEPLUS,
		.cmd_name = "qcom,mdss-dsi-panel-oneplus-mode-on-command",
	},
	{ 
		.idx = DSI_PANEL_MODE_ADAPTION,
		.cmd_name = "qcom,mdss-dsi-panel-adaption-mode-on-command",
	},
	{ }
};

static void mdss_livedisplay_parse_dsi_presets(struct device_node *np, 
		struct mdss_livedisplay_ctx *mlc, int idx)
{
	int i;

	if (idx >= DSI_PANEL_MODE_MAX)
		return;

	for (i = DSI_PANEL_MODE_OFF; i < DSI_PANEL_MODE_MAX; i++) {
		if (dsi_presets[i].idx != idx)
			continue;

		mlc->presets[mlc->num_presets] = of_get_property(np, 
					dsi_presets[i].cmd_name,
					&mlc->presets_len[mlc->num_presets]);
		if (mlc->presets_len[mlc->num_presets] > 0)
			mlc->num_presets++;
	}
}

int mdss_livedisplay_parse_dt(struct device_node *np, struct mdss_panel_info *pinfo)
{
	int rc = 0, i = 0;
	struct mdss_livedisplay_ctx *mlc;
	char preset_name[64];
	const char *link_state;
	uint32_t tmp = 0;

	if (pinfo == NULL)
		return -ENODEV;

	mlc = kzalloc(sizeof(struct mdss_livedisplay_ctx), GFP_KERNEL);
	mutex_init(&mlc->lock);

	link_state = of_get_property(np, "cm,mdss-livedisplay-command-state", NULL);
	if (link_state && !strcmp(link_state, "dsi_lp_mode"))
		mlc->link_state = DSI_LP_MODE;
	else
		mlc->link_state = DSI_HS_MODE;

	mlc->cabc_cmds = of_get_property(np,
			"cm,mdss-livedisplay-cabc-cmd", &mlc->cabc_cmds_len);

	if (mlc->cabc_cmds_len > 0) {
		rc = of_property_read_u32(np, "cm,mdss-livedisplay-cabc-ui-value", &tmp);
		if (rc == 0) {
			// Read unified CABC cmds first
			mlc->caps |= MODE_CABC;
			mlc->unified_cabc_cmds = true;
			mlc->cabc_ui_value = (uint8_t)(tmp & 0xFF);
			of_property_read_u32(np, "cm,mdss-livedisplay-cabc-image-value", &tmp);
			mlc->cabc_image_value = (uint8_t)(tmp & 0xFF);
			of_property_read_u32(np, "cm,mdss-livedisplay-cabc-video-value", &tmp);
			mlc->cabc_video_value = (uint8_t)(tmp & 0xFF);
		} else {
			// If unified CABC cmds don't exist, try independent cmds
			mlc->cabc_ui_cmds = of_get_property(np,
					"cm,mdss-livedisplay-cabc-ui-cmd", &mlc->cabc_ui_cmds_len);
			if (mlc->cabc_ui_cmds_len > 0) {
				mlc->caps |= MODE_CABC;
				mlc->cabc_image_cmds = of_get_property(np,
						"cm,mdss-livedisplay-cabc-image-cmd", &mlc->cabc_image_cmds_len);
				mlc->cabc_video_cmds = of_get_property(np,
						"cm,mdss-livedisplay-cabc-video-cmd", &mlc->cabc_video_cmds_len);
			}
		}
		rc = of_property_read_u32(np, "cm,mdss-livedisplay-sre-medium-value", &tmp);
		if (rc == 0) {
			mlc->caps |= MODE_SRE;
			mlc->sre_medium_value = (uint8_t)(tmp & 0xFF);
			of_property_read_u32(np, "cm,mdss-livedisplay-sre-weak-value", &tmp);
			mlc->sre_weak_value = (uint8_t)(tmp & 0xFF);
			of_property_read_u32(np, "cm,mdss-livedisplay-sre-strong-value", &tmp);
			mlc->sre_strong_value = (uint8_t)(tmp & 0xFF);
		}
		rc = of_property_read_u32(np, "cm,mdss-livedisplay-aco-value", &tmp);
		if (rc == 0) {
			mlc->caps |= MODE_AUTO_CONTRAST;
			mlc->aco_value = (uint8_t)(tmp & 0xFF);
		}
	}

	for (i = 0; i < ARRAY_SIZE(hbm_props); i++) {
		if (!mlc->hbm_on_cmds_len)
			mlc->hbm_on_cmds = of_get_property(np,
					hbm_props[i].cmd_on_name, &mlc->hbm_on_cmds_len);

		if (!mlc->hbm_off_cmds_len)
			mlc->hbm_off_cmds = of_get_property(np,
					hbm_props[i].cmd_off_name, &mlc->hbm_off_cmds_len);	

		if (mlc->hbm_on_cmds_len && mlc->hbm_off_cmds_len) {
			mlc->caps |= MODE_HIGH_BRIGHTNESS;
			break;
		}
	}

	mlc->ce_on_cmds = of_get_property(np,
			"cm,mdss-livedisplay-color-enhance-on", &mlc->ce_on_cmds_len);
	if (mlc->ce_on_cmds_len) {
		mlc->ce_off_cmds = of_get_property(np,
				"cm,mdss-livedisplay-color-enhance-off", &mlc->ce_off_cmds_len);
		if (mlc->ce_off_cmds_len)
			mlc->caps |= MODE_COLOR_ENHANCE;
	}

	for (i = 0; i < MAX_PRESETS; i++) {
		memset(preset_name, 0, sizeof(preset_name));
		snprintf(preset_name, 64, "%s-%d", "cm,mdss-livedisplay-preset", i);
		mlc->presets[mlc->num_presets] = of_get_property(np, preset_name,
				&mlc->presets_len[mlc->num_presets]);
		if (mlc->presets_len[mlc->num_presets] > 0)
			mlc->num_presets++;
		else
			mdss_livedisplay_parse_dsi_presets(np, mlc, i);
	}

	if (mlc->num_presets)
		mlc->caps |= MODE_PRESET;

	mlc->post_cmds = of_get_property(np,
			"cm,mdss-livedisplay-post-cmd", &mlc->post_cmds_len);

	pinfo->livedisplay = mlc;
	return 0;
}

int mdss_livedisplay_create_sysfs(struct msm_fb_data_type *mfd)
{
	int rc = 0;
	struct mdss_livedisplay_ctx *mlc = get_ctx(mfd);

	if (mlc == NULL)
		return 0;

	if (mlc->caps & MODE_CABC) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_cabc.attr);
		if (rc)
			goto sysfs_err;
	}

	if (mlc->caps & MODE_SRE) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_sre.attr);
		if (rc)
			goto sysfs_err;
	}

	if (mlc->caps & MODE_AUTO_CONTRAST) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_aco.attr);
		if (rc)
			goto sysfs_err;
	}

	if (mlc->caps & MODE_COLOR_ENHANCE) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_color_enhance.attr);
		if (rc)
			goto sysfs_err;
	}

	if (mlc->caps & MODE_HIGH_BRIGHTNESS) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_hbm.attr);
		if (rc)
			goto sysfs_err;
	}

	if (mlc->caps & MODE_PRESET) {
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_preset.attr);
		if (rc)
			goto sysfs_err;
		rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_num_presets.attr);
		if (rc)
			goto sysfs_err;
	}

	mlc->mfd = mfd;

	return rc;

sysfs_err:
	pr_err("%s: sysfs creation failed, rc=%d", __func__, rc);
	return rc;
}

