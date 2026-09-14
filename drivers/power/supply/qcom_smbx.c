// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023, Linaro Ltd.
 * Author: Casey Connolly <casey.connolly@linaro.org>
 *
 * This driver is for the switch-mode battery charger and boost
 * hardware found in pmi8998 and related PMICs.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/workqueue.h>

enum smb_generation {
	SMB2,
	SMB5,
};

#define SMB_REG_OFFSET(smb) (smb->gen == SMB2 ? 0x600 : 0x100)

/* clang-format off */
#define BATTERY_CHARGER_STATUS_1			0x06
#define BATTERY_CHARGER_STATUS_MASK			GENMASK(2, 0)

#define BATTERY_CHARGER_STATUS_2			0x07
#define SMB2_CHARGER_ERROR_STATUS_BAT_OV_BIT		BIT(5)
#define SMB2_BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT	BIT(3)
#define SMB2_BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT	BIT(2)
#define SMB2_BAT_TEMP_STATUS_TOO_HOT_BIT		BIT(1)
#define SMB5_CHARGER_ERROR_STATUS_BAT_OV_BIT		BIT(1)
#define SMB2_BAT_TEMP_STATUS_TOO_COLD_BIT		BIT(0)

#define BATTERY_CHARGER_STATUS_7			0x0D
#define SMB5_BAT_TEMP_STATUS_HOT_SOFT_BIT		BIT(5)
#define SMB5_BAT_TEMP_STATUS_COLD_SOFT_BIT		BIT(4)
#define SMB5_BAT_TEMP_STATUS_TOO_HOT_BIT		BIT(3)
#define SMB5_BAT_TEMP_STATUS_TOO_COLD_BIT		BIT(2)

#define CHARGING_ENABLE_CMD				0x42
#define CHARGING_ENABLE_CMD_BIT				BIT(0)

#define CHGR_CFG2					0x51
#define CHG_EN_SRC_BIT					BIT(7)
#define CHG_EN_POLARITY_BIT				BIT(6)
#define PRETOFAST_TRANSITION_CFG_BIT			BIT(5)
#define BAT_OV_ECC_BIT					BIT(4)
#define I_TERM_BIT					BIT(3)
#define AUTO_RECHG_BIT					BIT(2)
#define EN_ANALOG_DROP_IN_VBATT_BIT			BIT(1)
#define CHARGER_INHIBIT_BIT				BIT(0)

#define PRE_CHARGE_CURRENT_CFG				0x60
#define PRE_CHARGE_CURRENT_SETTING_MASK			GENMASK(5, 0)

#define FAST_CHARGE_CURRENT_CFG				0x61
#define FAST_CHARGE_CURRENT_SETTING_MASK		GENMASK(7, 0)

#define FLOAT_VOLTAGE_CFG				0x70
#define FLOAT_VOLTAGE_SETTING_MASK			GENMASK(7, 0)

#define SMB2_FG_UPDATE_CFG_2_SEL			0x7D
#define SMB2_SOC_LT_CHG_RECHARGE_THRESH_SEL_BIT		BIT(2)
#define SMB2_VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT		BIT(1)

#define SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_REG		0x7D
#define SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_MASK		GENMASK(7, 0)

#define OTG_CFG						0x153
#define OTG_EN_SRC_CFG_BIT				BIT(1)

#define APSD_STATUS					0x307
#define APSD_DTC_STATUS_DONE_BIT			BIT(0)
#define QC_CHARGER_BIT					BIT(1)

#define APSD_RESULT_STATUS				0x308
#define APSD_RESULT_STATUS_MASK				GENMASK(6, 0)
#define FLOAT_CHARGER_BIT				BIT(4)
#define DCP_CHARGER_BIT					BIT(3)
#define CDP_CHARGER_BIT					BIT(2)
#define OCP_CHARGER_BIT					BIT(1)
#define SDP_CHARGER_BIT					BIT(0)
#define QC_3P0_BIT					BIT(6)
#define QC_2P0_BIT					BIT(5)

#define USBIN_CMD_IL					0x340
#define USBIN_SUSPEND_BIT				BIT(0)

#define CMD_APSD					0x341
#define APSD_RERUN_BIT					BIT(0)

#define CMD_ICL_OVERRIDE				0x342
#define ICL_OVERRIDE_BIT				BIT(0)

#define TYPE_C_CFG					0x358
#define APSD_START_ON_CC_BIT				BIT(7)
#define FACTORY_MODE_DETECTION_EN_BIT			BIT(5)
#define TYPE_C_OR_U_USB_BIT				BIT(0)
#define VCONN_OC_CFG_BIT				BIT(1)

#define USBIN_ADAPTER_ALLOW_CFG				0x360
#define USBIN_ADAPTER_ALLOW_OVERRIDE			0x344
#define ADAPTER_ALLOW_FORCE_NULL			0
#define ADAPTER_ALLOW_FORCE_5V				BIT(0)
#define ADAPTER_ALLOW_FORCE_9V				BIT(1)
#define ADAPTER_ALLOW_CONTINUOUS			BIT(3)
#define USBIN_OPTIONS_1_CFG				0x362
#define BC1P2_SRC_DETECT_BIT				BIT(3)
#define HVDCP_EN_BIT					BIT(2)
#define HVDCP_AUTH_ALG_EN_CFG_BIT			BIT(6)
#define HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT		BIT(5)
#define CMD_HVDCP_2_REG				0x343
#define SINGLE_INCREMENT_BIT				BIT(0)
#define SINGLE_DECREMENT_BIT				BIT(1)
#define FORCE_5V_BIT					BIT(3)
#define FORCE_9V_BIT					BIT(4)
#define FORCE_12V_BIT					BIT(5)
#define TYPEC_U_USB_CFG_REG				0x570
#define EN_MICRO_USB_FACTORY_MODE_BIT			BIT(1)
#define EN_MICRO_USB_MODE_BIT				BIT(0)
#define QC_CHANGE_STATUS_REG				0x309
#define QC_5V_BIT					BIT(0)
#define QC_9V_BIT					BIT(1)

#define USBIN_LOAD_CFG					0x365
#define ICL_OVERRIDE_AFTER_APSD_BIT			BIT(4)
#define USBIN_VOLTAGE_LSB_REG				0x347
/* Each LSB = 25uV for USBIN voltage ADC */

#define USBIN_ICL_OPTIONS				0x366
#define USB51_MODE_BIT					BIT(1)
#define USBIN_MODE_CHG_BIT				BIT(0)

/* PMI8998 only */
#define TYPE_C_INTRPT_ENB_SOFTWARE_CTRL			0x368
#define SMB2_VCONN_EN_SRC_BIT				BIT(4)
#define VCONN_EN_VALUE_BIT				BIT(3)
#define TYPEC_POWER_ROLE_CMD_MASK			GENMASK(2, 0)
#define SMB5_EN_SNK_ONLY_BIT				BIT(1)

#define USBIN_CURRENT_LIMIT_CFG				0x370

#define USBIN_AICL_OPTIONS_CFG				0x380
#define SUSPEND_ON_COLLAPSE_USBIN_BIT			BIT(7)
#define USBIN_AICL_START_AT_MAX_BIT			BIT(5)
#define USBIN_AICL_PERIODIC_RERUN_EN_BIT		BIT(4)
#define USBIN_AICL_ADC_EN_BIT				BIT(3)
#define USBIN_AICL_EN_BIT				BIT(2)
#define USBIN_HV_COLLAPSE_RESPONSE_BIT			BIT(1)
#define USBIN_LV_COLLAPSE_RESPONSE_BIT			BIT(0)

// FIXME: drop these and their programming, no need to set min to 4.3v
#define USBIN_5V_AICL_THRESHOLD_CFG			0x381
#define USBIN_5V_AICL_THRESHOLD_CFG_MASK		GENMASK(2, 0)

#define USBIN_CONT_AICL_THRESHOLD_CFG			0x384
#define USBIN_CONT_AICL_THRESHOLD_CFG_MASK		GENMASK(5, 0)

#define ICL_STATUS(smb)					(SMB_REG_OFFSET(smb) + 0x07)
#define INPUT_CURRENT_LIMIT_MASK			GENMASK(7, 0)

#define POWER_PATH_STATUS(smb)				(SMB_REG_OFFSET(smb) + 0x0B)
#define P_PATH_USE_USBIN_BIT				BIT(4)
#define P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT		BIT(0)

/* 0x5xx region is PM8150b only Type-C registers */

/* Bits 2:0 match PMI8998 TYPE_C_INTRPT_ENB_SOFTWARE_CTRL */
#define SMB5_TYPE_C_MODE_CFG				0x544
#define SMB5_EN_TRY_SNK_BIT				BIT(4)
#define SMB5_EN_SNK_ONLY_BIT				BIT(1)

#define SMB5_TYPEC_TYPE_C_VCONN_CONTROL			0x546
#define SMB5_VCONN_EN_ORIENTATION_BIT			BIT(2)
#define SMB5_VCONN_EN_VALUE_BIT				BIT(1)
#define SMB5_VCONN_EN_SRC_BIT				BIT(0)


#define SMB5_TYPE_C_DEBUG_ACCESS_SINK			0x54a
#define SMB5_TYPEC_DEBUG_ACCESS_SINK_MASK		GENMASK(4, 0)

#define SMB5_DEBUG_ACCESS_SRC_CFG			0x54C
#define SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT	BIT(0)

#define SMB5_TYPE_C_EXIT_STATE_CFG			0x550
#define SMB5_BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT	BIT(3)
#define SMB5_SEL_SRC_UPPER_REF_BIT			BIT(2)
#define SMB5_EXIT_SNK_BASED_ON_CC_BIT			BIT(0)

/* Common */

#define BARK_BITE_WDOG_PET				0x643
#define BARK_BITE_WDOG_PET_BIT				BIT(0)

#define WD_CFG						0x651
#define WATCHDOG_TRIGGER_AFP_EN_BIT			BIT(7)
#define BARK_WDOG_INT_EN_BIT				BIT(6)
#define WDOG_TIMER_EN_ON_PLUGIN_BIT			BIT(1)

#define SNARL_BARK_BITE_WD_CFG				0x653

#define AICL_RERUN_TIME_CFG				0x661
#define AICL_RERUN_TIME_MASK				GENMASK(1, 0)
#define AIC_RERUN_TIME_3_SECS				0x0

/* FIXME: probably remove this so we get parallel charging? */
#define STAT_CFG					0x690
#define STAT_SW_OVERRIDE_CFG_BIT			BIT(6)

/*
 * PM8150B MISC (absolute 0x16xx) accessed as charger@1000 + offset.
 * MISC_SMB_CFG @ 0x1690 shares the same relative offset as SMB2 STAT_CFG.
 */
#define MISC_SMB_EN_CMD					0x648
#define EN_CP_CMD_BIT					BIT(0)
#define SMB_EN_OVERRIDE_BIT				BIT(3)
#define SMB_EN_OVERRIDE_VALUE_BIT			BIT(4)
#define EN_STAT_CMD_BIT					BIT(2)

#define MISC_SMB_CFG					0x690
#define SMB_EN_SEL_BIT					BIT(4)

#define SDP_CURRENT_UA					500000
#define CDP_CURRENT_UA					1500000
#define DCP_CURRENT_UA					1500000
#define CURRENT_MAX_UA					DCP_CURRENT_UA
/* Align with Android Raphael qcom,usb-icl-ua */
#define USB_ICL_MAX_UA					2800000
/* Android qcom,fcc-max-ua when charge pump is active */
#define FCC_CP_MAX_UA					5100000
/* Enable SMB1390 when negotiated Vbus is at least 9V */
#define CP_MIN_VBUS_UV					8000000

/* pmi8998 registers represent current in increments of 1/40th of an amp */
#define CURRENT_SCALE_FACTOR				25000
/* clang-format on */

enum charger_status {
	TRICKLE_CHARGE = 0,
	PRE_CHARGE,
	FAST_CHARGE,
	FULLON_CHARGE,
	TAPER_CHARGE,
	TERMINATE_CHARGE,
	INHIBIT_CHARGE,
	DISABLE_CHARGE,
};

struct smb_init_register {
	u16 addr;
	u8 mask;
	u8 val;
};

/**
 * struct smb_chip - smb chip structure
 * @dev:		Device reference for power_supply
 * @name:		The platform device name
 * @base:		Base address for smb registers
 * @regmap:		Register map
 * @batt_info:		Battery data from DT
 * @status_change_work: Worker to handle plug/unplug events
 * @cable_irq:		USB plugin IRQ
 * @wakeup_enabled:	If the cable IRQ will cause a wakeup
 * @usb_in_i_chan:	USB_IN current measurement channel
 * @usb_in_v_chan:	USB_IN voltage measurement channel
 * @chg_psy:		Charger power supply instance
 * @cp_psy:		Optional SMB1390 charge-pump supply
 * @nb:			Notifier for TCPM / charge-pump psy changes
 * @lock:		Protects ICL/FCC/CP coordination
 * @pd_icl_ua:		Negotiated PD input current (0 = use APSD)
 * @pd_vbus_uv:		Negotiated PD voltage
 * @cp_enabled:		Charge-pump path currently requested
 * @cp_ramp_step:		Current ramp-up step for CP ILIM
 */
struct smb_chip {
	struct device *dev;
	const char *name;
	unsigned int base;
	struct regmap *regmap;
	struct power_supply_battery_info *batt_info;
	enum smb_generation gen;

	struct delayed_work status_change_work;
	int cable_irq;
	bool wakeup_enabled;

	struct iio_channel *usb_in_i_chan;
	struct iio_channel *usb_in_v_chan;

	struct power_supply *chg_psy;
	struct power_supply *cp_psy;
	struct notifier_block nb;
	struct mutex lock;

	unsigned int pd_icl_ua;
	unsigned int pd_vbus_uv;
	bool cp_enabled;
	int cp_ramp_step;
	bool hvdcp_detected;
	bool qc_negotiated;
	bool apsd_rerun_done;
	int apsd_retry_count;
	int qc3_wait_count;
	struct regulator *dpdm_reg;
	bool dpdm_enabled;
	bool is_qc3;
};

struct smb_match_data {
	const char *name;
	enum smb_generation gen;
	size_t init_seq_len;
	const struct smb_init_register *init_seq;
};

static enum power_supply_property smb_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static int smb_get_prop_usb_online(struct smb_chip *chip, int *val)
{
	unsigned int stat;
	int rc;

	rc = regmap_read(chip->regmap, chip->base + POWER_PATH_STATUS(chip), &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read power path status: %d\n", rc);
		return rc;
	}

	*val = (stat & P_PATH_USE_USBIN_BIT) &&
	       (stat & P_PATH_VALID_INPUT_POWER_SOURCE_STS_BIT);
	return 0;
}

/*
 * Qualcomm "automatic power source detection" aka APSD
 * tells us what type of charger we're connected to.
 */
static int smb_apsd_get_charger_type(struct smb_chip *chip, int *val)
{
	unsigned int apsd_stat, stat;
	int usb_online = 0;
	int rc;

	rc = smb_get_prop_usb_online(chip, &usb_online);
	if (!usb_online) {
		*val = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return rc;
	}

	rc = regmap_read(chip->regmap, chip->base + APSD_STATUS, &apsd_stat);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read apsd status, rc = %d", rc);
		return rc;
	}
	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT)) {
		pr_info("smb: APSD not ready, stat=0x%02x\n", apsd_stat);
		return -EAGAIN;
	}

	rc = regmap_read(chip->regmap, chip->base + APSD_RESULT_STATUS, &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read apsd result, rc = %d", rc);
		return rc;
	}

	stat &= APSD_RESULT_STATUS_MASK;
	pr_info("smb: APSD done, result=0x%02x\n", stat);

	/* Debug: read APSD_STATUS for QC_CHARGER_BIT */
	regmap_read(chip->regmap, chip->base + APSD_STATUS, &apsd_stat);
	pr_info("smb: APSD_STATUS=0x%02x\n", apsd_stat);
	regmap_read(chip->regmap, chip->base + 0x358, &apsd_stat);
	pr_info("smb: TYPE_C_CFG=0x%02x\n", apsd_stat);
	regmap_read(chip->regmap, chip->base + 0x362, &apsd_stat);
	pr_info("smb: USBIN_OPTIONS_1_CFG=0x%02x\n", apsd_stat);
	regmap_read(chip->regmap, chip->base + 0x570, &apsd_stat);
	pr_info("smb: TYPEC_U_USB_CFG(sid3)=0x%02x\n", apsd_stat);

	/* Debug: read sid3 Type-C status registers (offset = abs - 0x1000) */
	{
		unsigned int val;
		/* TYPE_C_MISC_STATUS (0x150B) - CC status */
		regmap_read(chip->regmap, chip->base + 0x50B, &val);
		pr_info("smb: TYPE_C_MISC_STATUS=0x%02x\n", val);
		/* TYPEC_U_USB_STATUS (0x150F) - uUSB D+/D- status */
		regmap_read(chip->regmap, chip->base + 0x50F, &val);
		pr_info("smb: TYPEC_U_USB_STATUS=0x%02x\n", val);
		/* TYPE_C_MODE_CFG (0x1544) - port mode */
		regmap_read(chip->regmap, chip->base + 0x544, &val);
		pr_info("smb: TYPE_C_MODE_CFG=0x%02x\n", val);
		/* USBIN_ADAPTER_ALLOW_OVERRIDE (0x1344) */
		regmap_read(chip->regmap, chip->base + 0x344, &val);
		pr_info("smb: ADAPTER_ALLOW_OVERRIDE=0x%02x\n", val);
	}

	/* Check for HVDCP/QC chargers first (per vendor driver logic) */
	if (stat & QC_3P0_BIT) {
		pr_info("smb: QC3.0 charger detected\n");
		*val = POWER_SUPPLY_USB_TYPE_DCP;
		chip->hvdcp_detected = true;
		chip->is_qc3 = true;
	} else if (stat & QC_2P0_BIT) {
		pr_info("smb: QC2.0 charger detected\n");
		*val = POWER_SUPPLY_USB_TYPE_DCP;
		chip->hvdcp_detected = true;
		chip->is_qc3 = false;
	} else if (stat & CDP_CHARGER_BIT) {
		*val = POWER_SUPPLY_USB_TYPE_CDP;
	} else if (stat & (DCP_CHARGER_BIT | OCP_CHARGER_BIT | FLOAT_CHARGER_BIT)) {
		*val = POWER_SUPPLY_USB_TYPE_DCP;
	} else /* SDP_CHARGER_BIT (or others) */
		*val = POWER_SUPPLY_USB_TYPE_SDP;

	return 0;
}

/* Return 1 when in overvoltage state, else 0 or -errno */
static int smbx_ov_status(struct smb_chip *chip)
{
	u16 reg;
	u8 mask;
	int rc;
	u32 val;

	switch (chip->gen) {
	case SMB2:
		reg = BATTERY_CHARGER_STATUS_2;
		mask = SMB2_CHARGER_ERROR_STATUS_BAT_OV_BIT;
		break;
	case SMB5:
		reg = BATTERY_CHARGER_STATUS_7;
		mask = SMB5_CHARGER_ERROR_STATUS_BAT_OV_BIT;
		break;
	}

	rc = regmap_read(chip->regmap, chip->base + reg, &val);
	if (rc)
		return rc;

	return !!(reg & mask);
}

static int smb_get_prop_status(struct smb_chip *chip, int *val)
{
	u32 stat;
	int usb_online = 0;
	int rc;

	rc = smb_get_prop_usb_online(chip, &usb_online);
	if (!usb_online) {
		*val = POWER_SUPPLY_STATUS_DISCHARGING;
		return rc;
	}

	rc = regmap_read(chip->regmap,
			      chip->base + BATTERY_CHARGER_STATUS_1, &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Failed to read charging status ret=%d\n",
			rc);
		return rc;
	}

	rc = smbx_ov_status(chip);
	if (rc < 0)
		return rc;

	/* In overvoltage state */
	if (rc == 1) {
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FAST_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		*val = POWER_SUPPLY_STATUS_CHARGING;
		return rc;
	case DISABLE_CHARGE:
		*val = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return rc;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		*val = POWER_SUPPLY_STATUS_FULL;
		return rc;
	default:
		*val = POWER_SUPPLY_STATUS_UNKNOWN;
		return rc;
	}
}

static inline int smb_get_current_limit(struct smb_chip *chip,
					 unsigned int *val)
{
	int rc = regmap_read(chip->regmap, chip->base + ICL_STATUS(chip), val);

	if (rc >= 0)
		*val *= CURRENT_SCALE_FACTOR;
	return rc;
}

static int smb_set_current_limit(struct smb_chip *chip, unsigned int val)
{
	unsigned char val_raw;

	if (val > 4800000) {
		dev_err(chip->dev,
			"Can't set current limit higher than 4800000uA");
		return -EINVAL;
	}
	val_raw = val / CURRENT_SCALE_FACTOR;

	return regmap_write(chip->regmap, chip->base + USBIN_CURRENT_LIMIT_CFG,
			    val_raw);
}

static int smb_set_fcc(struct smb_chip *chip, unsigned int ua)
{
	if (ua > FCC_CP_MAX_UA)
		ua = FCC_CP_MAX_UA;

	return regmap_write(chip->regmap, chip->base + FAST_CHARGE_CURRENT_CFG,
			    ua / CURRENT_SCALE_FACTOR);
}

static unsigned int smb_default_fcc_ua(struct smb_chip *chip)
{
	int ua = chip->batt_info->constant_charge_current_max_ua;

	if (ua <= 0 || ua == -EINVAL)
		return 1950000;

	return ua;
}

static int smb_cp_hw_enable(struct smb_chip *chip, bool enable)
{
	int rc;

	if (chip->gen != SMB5)
		return 0;

	if (enable) {
		/* Select SMB_EN output from charge pump logic */
		rc = regmap_update_bits(chip->regmap, chip->base + MISC_SMB_CFG,
					SMB_EN_SEL_BIT, SMB_EN_SEL_BIT);
		if (rc)
			return rc;

		/* Enable SMB_EN: clear override so CP can auto-control it.
		 * Also send EN_CP_CMD to start the charge pump.
		 */
		rc = regmap_update_bits(chip->regmap,
					chip->base + MISC_SMB_EN_CMD,
					SMB_EN_OVERRIDE_BIT | EN_CP_CMD_BIT,
					EN_CP_CMD_BIT);
		if (rc)
			return rc;

		dev_info(chip->dev, "SMB_EN enabled (SMB_EN_SEL + EN_CP_CMD)\n");
		return 0;
	}

	/* Disable: set override to force SMB_EN low, then clear EN_CP_CMD */
	rc = regmap_update_bits(chip->regmap, chip->base + MISC_SMB_EN_CMD,
				SMB_EN_OVERRIDE_BIT | EN_CP_CMD_BIT,
				SMB_EN_OVERRIDE_BIT);
	if (rc)
		return rc;

	rc = regmap_update_bits(chip->regmap, chip->base + MISC_SMB_CFG,
				  SMB_EN_SEL_BIT, 0);
	if (rc)
		return rc;

	dev_info(chip->dev, "SMB_EN disabled\n");
	return 0;
}

static void smb_resolve_cp_psy(struct smb_chip *chip)
{
	if (chip->cp_psy)
		return;

	chip->cp_psy = power_supply_get_by_reference(dev_fwnode(chip->dev),
						     "qcom,charge-pump");
	if (IS_ERR(chip->cp_psy))
		chip->cp_psy = NULL;

	if (!chip->cp_psy)
		chip->cp_psy = power_supply_get_by_name("smb1390-charger");
}

static void smb_notify_cp(struct smb_chip *chip, bool enable, unsigned int icl_ua)
{
	union power_supply_propval val;
	int rc;

	smb_resolve_cp_psy(chip);
	if (!chip->cp_psy)
		return;

	if (enable && icl_ua) {
		val.intval = icl_ua;
		rc = power_supply_set_property(chip->cp_psy,
					       POWER_SUPPLY_PROP_CURRENT_MAX,
					       &val);
		if (rc)
			dev_dbg(chip->dev, "CP CURRENT_MAX set failed: %d\n",
				rc);
	}

	val.intval = enable;
	rc = power_supply_set_property(chip->cp_psy, POWER_SUPPLY_PROP_ONLINE,
				       &val);
	if (rc)
		dev_dbg(chip->dev, "CP ONLINE set failed: %d\n", rc);

	power_supply_changed(chip->cp_psy);
}

static void smb_update_charge_pump(struct smb_chip *chip, unsigned int icl_ua)
{
	bool want_cp;
	unsigned int fcc_ua, vbus_uv;
	int rc;

	/* Read VBUS via IIO channel. The IIO channel returns values
	 * in µV but with a ~2x underestimate (hardware ADC scaling).
	 * Use VOLTAGE_NOW property (which applies *16 correction) as
	 * the reliable source instead.
	 */
	{
		union power_supply_propval pval;

		if (chip->chg_psy &&
		    power_supply_get_property(chip->chg_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) >= 0 &&
		    pval.intval > 0)
			vbus_uv = pval.intval;
		else
			vbus_uv = 0;
	}

	want_cp = chip->gen == SMB5 && vbus_uv >= CP_MIN_VBUS_UV &&
		  icl_ua >= 900000 && chip->qc_negotiated;

	dev_info(chip->dev,
		 "CP check: vbus=%u pd_vbus=%u icl=%u want_cp=%d cp_enabled=%d gen=%d\n",
		 vbus_uv, chip->pd_vbus_uv, icl_ua, want_cp, chip->cp_enabled,
		 chip->gen);

	if (want_cp == chip->cp_enabled && !want_cp)
		return;

	if (want_cp) {
		/* Ramp up CP ILIM in steps to avoid VBUS collapse:
		 * Step 0: 1.5A (initial enable, safe for QC3.0)
		 * Step 1: 2.0A (after 2s stable)
		 * Step 2: 2.8A (full speed after 4s stable)
		 */
		unsigned int cp_ilim;
		unsigned int cp_ramp_steps[] = {1500000, 2000000};
		unsigned int target_ilim = min_t(unsigned int, icl_ua, 2800000);

		if (!chip->cp_enabled) {
			/* First enable: start at low ILIM */
			cp_ilim = cp_ramp_steps[0];
			chip->cp_ramp_step = 0;
		} else if (chip->cp_ramp_step < ARRAY_SIZE(cp_ramp_steps)) {
			cp_ilim = cp_ramp_steps[chip->cp_ramp_step];
		} else {
			cp_ilim = target_ilim;
		}

		rc = smb_cp_hw_enable(chip, true);
		if (rc) {
			dev_err(chip->dev, "Failed to enable SMB_EN: %d\n", rc);
			return;
		}

		fcc_ua = min_t(unsigned int, cp_ilim * 2, FCC_CP_MAX_UA);
		smb_set_fcc(chip, fcc_ua);
		smb_notify_cp(chip, true, cp_ilim);
		chip->cp_enabled = true;

		/* Schedule ramp-up if not at target yet */
		if (cp_ilim < target_ilim) {
			chip->cp_ramp_step++;
			schedule_delayed_work(&chip->status_change_work,
					      msecs_to_jiffies(2000));
		}

		dev_info(chip->dev,
			 "Charge pump requested (Vbus=%u uV ICL=%u uA CP_ILIM=%u uA FCC=%u uA ramp=%d)\n",
			 vbus_uv, icl_ua, cp_ilim, fcc_ua, chip->cp_ramp_step);
	} else if (chip->cp_enabled) {
		smb_notify_cp(chip, false, 0);
		smb_cp_hw_enable(chip, false);
		smb_set_fcc(chip, smb_default_fcc_ua(chip));
		chip->cp_enabled = false;
		chip->cp_ramp_step = 0;
		dev_info(chip->dev, "Charge pump disabled\n");
	}
}

static unsigned int smb_apsd_icl_ua(struct smb_chip *chip, unsigned int type)
{
	switch (type) {
	case POWER_SUPPLY_USB_TYPE_CDP:
		return CDP_CURRENT_UA;
	case POWER_SUPPLY_USB_TYPE_DCP:
		return min_t(unsigned int,
			     chip->batt_info->constant_charge_current_max_ua,
			     USB_ICL_MAX_UA);
	case POWER_SUPPLY_USB_TYPE_SDP:
	default:
		return SDP_CURRENT_UA;
	}
}

static void smb_apply_input_limits(struct smb_chip *chip)
{
	unsigned int charger_type, current_ua;
	int usb_online = 0;
	int count, rc;

	mutex_lock(&chip->lock);

	pr_info("smb: apply_input_limits, pd_icl_ua=%u\n", chip->pd_icl_ua);
	smb_get_prop_usb_online(chip, &usb_online);
	if (!usb_online) {
		chip->pd_icl_ua = 0;
		chip->pd_vbus_uv = 0;
		smb_update_charge_pump(chip, 0);
		/* Reset to safe defaults on disconnect */
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_ADAPTER_ALLOW_CFG,
				   0x0f, 0);
		regmap_write(chip->regmap,
			  chip->base + USBIN_ADAPTER_ALLOW_OVERRIDE,
			  ADAPTER_ALLOW_FORCE_5V);
		regmap_write(chip->regmap,
			  chip->base + CMD_ICL_OVERRIDE, 0);
		/* Disable HVDCP when disconnected */
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_OPTIONS_1_CFG,
				   HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT | HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT, 0);
		chip->hvdcp_detected = false;
		chip->is_qc3 = false;
		chip->qc_negotiated = false;
		chip->apsd_rerun_done = false;
		chip->apsd_retry_count = 0;
		chip->qc3_wait_count = 0;
		mutex_unlock(&chip->lock);
		return;
	}

	if (chip->pd_icl_ua) {
		pr_info("smb: PD path, pd_icl_ua=%u\n", chip->pd_icl_ua);
		current_ua = min_t(unsigned int, chip->pd_icl_ua, USB_ICL_MAX_UA);
		/* PD contract active: allow up to 9V input and force software ICL */
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_ADAPTER_ALLOW_CFG,
				   0x0f, 8);
		regmap_write(chip->regmap,
			  chip->base + CMD_ICL_OVERRIDE,
			  ICL_OVERRIDE_BIT);
	} else {
		pr_info("smb: non-PD path, enabling HVDCP\n");
		/* Allow all voltages for QC negotiation */
		regmap_write(chip->regmap,
			  chip->base + USBIN_ADAPTER_ALLOW_OVERRIDE,
			  ADAPTER_ALLOW_FORCE_NULL);

		/* Quick path: if QC was already detected by a previous
		 * APSD run but voltage negotiation hasn't happened yet,
		 * do it now instead of re-running APSD.
		 */
		if (chip->hvdcp_detected && !chip->qc_negotiated) {
			chip->qc_negotiated = true;
			current_ua = 3000000;
			regmap_write(chip->regmap,
			  chip->base + CMD_ICL_OVERRIDE,
			  ICL_OVERRIDE_BIT);
			regmap_update_bits(chip->regmap,
					   chip->base + USBIN_ADAPTER_ALLOW_CFG,
					   0x0f, 8);
			pr_info("smb: QC already detected (qc3=%d), negotiating voltage\n",
				chip->is_qc3);
			/* Disable autonomous mode for software D+/D- control */
			regmap_update_bits(chip->regmap,
				chip->base + USBIN_OPTIONS_1_CFG,
				HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT, 0);
			if (chip->is_qc3) {
				int i;
				unsigned int qc_stat;
				for (i = 0; i < 20; i++) {
					regmap_update_bits(chip->regmap,
						chip->base + CMD_HVDCP_2_REG,
						SINGLE_INCREMENT_BIT,
						SINGLE_INCREMENT_BIT);
					msleep(2);
				}
				pr_info("smb: QC3 %d increments (quick)\n", i);
				regmap_read(chip->regmap,
					chip->base + QC_CHANGE_STATUS_REG,
					&qc_stat);
				pr_info("smb: QC_STATUS=0x%x after QC3\n", qc_stat);
			} else {
				int rc;
				unsigned int qc_stat;
				regmap_update_bits(chip->regmap,
					chip->base + CMD_HVDCP_2_REG,
					FORCE_9V_BIT, FORCE_9V_BIT);
				pr_info("smb: QC2 FORCE_9V\n");
				msleep(50);
				regmap_read(chip->regmap,
					chip->base + QC_CHANGE_STATUS_REG,
					&qc_stat);
				pr_info("smb: QC_STATUS=0x%x after QC2\n", qc_stat);
			}
			/* Read VBUS after QC negotiation */
			{
				union power_supply_propval pval;
				int vbus_uv = 0;
				if (chip->chg_psy &&
				    power_supply_get_property(chip->chg_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) == 0)
					vbus_uv = pval.intval;
				pr_info("smb: VBUS after QC=%d\n", vbus_uv);
			}
			goto set_current;
		}

		/* Enable DPDM regulator to route D+/D- to PMIC for QC negotiation */
		if (chip->dpdm_reg && !chip->dpdm_enabled) {
			rc = regulator_enable(chip->dpdm_reg);
			if (rc < 0)
				dev_warn(chip->dev, "Couldn't enable dpdm regulator rc=%d\n", rc);
			else {
				chip->dpdm_enabled = true;
				pr_info("smb: DPDM regulator enabled\n");
			}
		}
		/* Allow all voltages for QC negotiation */
		regmap_write(chip->regmap,
			  chip->base + USBIN_ADAPTER_ALLOW_OVERRIDE,
			  ADAPTER_ALLOW_FORCE_NULL);
		/* Force ICL override and allow 9V */
		regmap_write(chip->regmap,
			  chip->base + CMD_ICL_OVERRIDE,
			  ICL_OVERRIDE_BIT);
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_ADAPTER_ALLOW_CFG,
				   0x0f, 8);
		/* Enable HVDCP auth, QC3.0 handshake, and autonomous mode.
		 * In autonomous mode, PMIC hardware will automatically
		 * negotiate QC voltage via D+/D- without software
		 * intervention. We just need to wait and then check the
		 * result.
		 */
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_OPTIONS_1_CFG,
				   HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT,
				   HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT);
		/* Allow adapter voltage up to 9V — without this the PMIC
		 * will refuse to negotiate above 5V even in autonomous mode.
		 */
		regmap_update_bits(chip->regmap,
				   chip->base + USBIN_ADAPTER_ALLOW_CFG,
				   0x0f, 8);
		regmap_write(chip->regmap,
			  chip->base + USBIN_ADAPTER_ALLOW_OVERRIDE,
			  ADAPTER_ALLOW_FORCE_NULL);
		{
			unsigned int opt1 = 0, qc_chg = 0, adapter_cfg = 0, adapter_ovr = 0;
			regmap_read(chip->regmap,
				    chip->base + USBIN_OPTIONS_1_CFG, &opt1);
			regmap_read(chip->regmap,
				    chip->base + QC_CHANGE_STATUS_REG, &qc_chg);
			regmap_read(chip->regmap,
				    chip->base + USBIN_ADAPTER_ALLOW_CFG, &adapter_cfg);
			regmap_read(chip->regmap,
				    chip->base + USBIN_ADAPTER_ALLOW_OVERRIDE, &adapter_ovr);
			pr_info("smb: USBIN_OPTIONS_1=0x%x QC_STATUS=0x%x ADAPTER_CFG=0x%x ADAPTER_OVR=0x%x\n",
				opt1, qc_chg, adapter_cfg, adapter_ovr);
		}

		/* Read APSD result. DO NOT rerun APSD — that interrupts
		 * PMIC autonomous mode QC negotiation. Just read the
		 * current result and wait for QC bits to appear.
		 */
		rc = smb_apsd_get_charger_type(chip, &charger_type);
		pr_info("smb: APSD rc=%d charger_type=%d hvdcp=%d retry=%d\n",
			rc, charger_type, chip->hvdcp_detected,
			chip->apsd_retry_count);

		if (chip->hvdcp_detected) {
			/* QC charger detected. If this is the first time
			 * seeing QC2.0, wait additional cycles to see if
			 * it upgrades to QC3.0 (PMIC autonomous mode
			 * detects QC2.0 first, then QC3.0).
			 */
			if (!chip->is_qc3 && chip->qc3_wait_count < 3) {
				chip->qc3_wait_count++;
				current_ua = smb_apsd_icl_ua(chip, charger_type);
				pr_info("smb: QC2.0 detected, waiting to see if QC3.0 (wait=%d)\n",
					chip->qc3_wait_count);
				schedule_delayed_work(&chip->status_change_work,
						      msecs_to_jiffies(2000));
				goto set_current;
			}
			/* QC type stable now, negotiate voltage */
			current_ua = 3000000;
			if (!chip->qc_negotiated) {
				unsigned int qc_stat;
				int i;

				chip->qc_negotiated = true;
				pr_info("smb: QC detected (qc3=%d), negotiating voltage\n",
					chip->is_qc3);

				/* Software-controlled QC negotiation.
				 * Autonomous mode is NOT enabled — vendor
				 * driver uses software control for QC3.0
				 * via SINGLE_INCREMENT commands.
				 */
				if (chip->is_qc3) {
					unsigned int cmd_val, opt1, regval;
					/* Dump all QC-relevant registers */
					regmap_read(chip->regmap, chip->base + 0x362, &opt1);
					pr_info("smb: USBIN_OPTIONS_1=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x360, &opt1);
					pr_info("smb: ADAPTER_ALLOW_CFG=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x344, &opt1);
					pr_info("smb: ADAPTER_ALLOW_OVERRIDE=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x342, &opt1);
					pr_info("smb: CMD_ICL_OVERRIDE=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x309, &opt1);
					pr_info("smb: QC_CHANGE_STATUS=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x308, &opt1);
					pr_info("smb: QC_2P0_STATUS=0x%x\n", opt1);
					regmap_read(chip->regmap, chip->base + 0x343, &cmd_val);
					pr_info("smb: CMD_HVDCP_2 before: 0x%x\n", cmd_val);

					/* Test: try regmap_write directly */
					rc = regmap_write(chip->regmap,
						chip->base + CMD_HVDCP_2_REG,
						FORCE_9V_BIT);
					pr_info("smb: regmap_write FORCE_9V rc=%d\n", rc);
					msleep(100);
					regmap_read(chip->regmap, chip->base + 0x309, &regval);
					pr_info("smb: QC_STATUS after FORCE_9V=0x%x\n", regval);
					{
						union power_supply_propval pval;
						if (chip->chg_psy &&
						    power_supply_get_property(chip->chg_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) == 0)
							pr_info("smb: VBUS after FORCE_9V=%d\n", pval.intval);
					}
					/* If FORCE_9V didn't work, try SINGLE_INCREMENT */
					if (!(regval & QC_9V_BIT)) {
						pr_info("smb: FORCE_9V failed, trying SINGLE_INCREMENT\n");
						for (i = 0; i < 20; i++) {
							rc = regmap_write(chip->regmap,
								chip->base + CMD_HVDCP_2_REG,
								SINGLE_INCREMENT_BIT);
							if (rc)
								pr_info("smb: SINGLE_INCREMENT rc=%d i=%d\n", rc, i);
							msleep(50);
						}
						pr_info("smb: QC3 %d increments sent\n", i);
						msleep(500);
						regmap_read(chip->regmap, chip->base + 0x309, &regval);
						pr_info("smb: QC_STATUS after increments=0x%x\n", regval);
						{
							union power_supply_propval pval;
							if (chip->chg_psy &&
							    power_supply_get_property(chip->chg_psy,
								POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) == 0)
								pr_info("smb: VBUS after increments=%d\n", pval.intval);
						}
					}
				} else {
					/* QC2.0: try FORCE_9V */
					regmap_read(chip->regmap,
						chip->base + QC_CHANGE_STATUS_REG,
						&qc_stat);
					pr_info("smb: QC2 QC_STATUS=0x%x before force\n", qc_stat);
					if (qc_stat & QC_9V_BIT) {
						pr_info("smb: QC2 already at 9V\n");
					} else {
						regmap_update_bits(chip->regmap,
							chip->base + CMD_HVDCP_2_REG,
							FORCE_9V_BIT, FORCE_9V_BIT);
						pr_info("smb: QC2 FORCE_9V\n");
						msleep(500);
						regmap_read(chip->regmap,
							chip->base + QC_CHANGE_STATUS_REG,
							&qc_stat);
						pr_info("smb: QC_STATUS=0x%x after QC2\n", qc_stat);
						/* If FORCE_9V failed, try
						 * SINGLE_INCREMENT (works with
						 * QC3.0 chargers in QC2.0 mode)
						 */
						if (!(qc_stat & QC_9V_BIT)) {
							pr_info("smb: QC2 FORCE_9V failed, trying SINGLE_INCREMENT\n");
							for (i = 0; i < 20; i++) {
								regmap_update_bits(chip->regmap,
									chip->base + CMD_HVDCP_2_REG,
									SINGLE_INCREMENT_BIT,
									SINGLE_INCREMENT_BIT);
								msleep(50);
							}
							pr_info("smb: QC2 %d increments sent\n", i);
							msleep(500);
							regmap_read(chip->regmap,
								chip->base + QC_CHANGE_STATUS_REG,
								&qc_stat);
							pr_info("smb: QC_STATUS=0x%x after QC2 increments\n", qc_stat);
						}
					}
				}
				/* Read VBUS after QC negotiation */
				{
					unsigned int vbus_raw;
					int vbus_uv = 0;
					/* Try IIO for VBUS reading */
					if (chip->chg_psy) {
						union power_supply_propval pval;
						if (power_supply_get_property(chip->chg_psy,
							POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) == 0)
							pr_info("smb: VBUS psy=%d\n", pval.intval);
					}
					/* Also read directly from IIO */
					regmap_read(chip->regmap,
						chip->base + 0x47,
						&vbus_raw);
					pr_info("smb: VBUS raw=0x%x\n", vbus_raw);
				}
				/* QC negotiation done — schedule CP check */
				schedule_delayed_work(&chip->status_change_work,
						      msecs_to_jiffies(500));
			}
		} else if (chip->apsd_retry_count < 10) {
			/* QC not yet detected. PMIC autonomous mode is
			 * negotiating. Wait and check again.
			 */
			chip->apsd_retry_count++;
			current_ua = smb_apsd_icl_ua(chip, charger_type);
			pr_info("smb: QC not detected yet, waiting (attempt %d, icl=%u)\n",
				chip->apsd_retry_count, current_ua);
			schedule_delayed_work(&chip->status_change_work,
					      msecs_to_jiffies(2000));
		} else {
			/* QC not detected after 10 waits (~20s).
			 * Try FORCE_9V directly as last resort.
			 */
			pr_info("smb: QC not detected after %d waits, trying FORCE_9V\n",
				chip->apsd_retry_count);
			chip->hvdcp_detected = true;
			chip->is_qc3 = false;
			chip->qc_negotiated = true;
			current_ua = 3000000;
			regmap_update_bits(chip->regmap,
				chip->base + CMD_HVDCP_2_REG,
				FORCE_9V_BIT, FORCE_9V_BIT);
			pr_info("smb: Forced FORCE_9V\n");
			msleep(50);
			{
				unsigned int qc_stat;
				regmap_read(chip->regmap,
					chip->base + QC_CHANGE_STATUS_REG,
					&qc_stat);
				pr_info("smb: QC_STATUS=0x%x after forced FORCE_9V\n",
					qc_stat);
			}
			{
				union power_supply_propval pval;
				int vbus_uv = 0;
				if (chip->chg_psy &&
				    power_supply_get_property(chip->chg_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval) == 0)
					vbus_uv = pval.intval;
				pr_info("smb: VBUS after force_9V=%d\n", vbus_uv);
			}
		}
	}

set_current:
	smb_set_current_limit(chip, current_ua);

	if (!chip->cp_enabled)
		smb_set_fcc(chip, smb_default_fcc_ua(chip));

	smb_update_charge_pump(chip, current_ua);
	mutex_unlock(&chip->lock);

	power_supply_changed(chip->chg_psy);
}

static void smb_status_change_work(struct work_struct *work)
{
	struct smb_chip *chip;

	chip = container_of(work, struct smb_chip, status_change_work.work);
	smb_apply_input_limits(chip);
}

static bool smb_psy_is_tcpm(struct power_supply *psy)
{
	const char *name;

	if (!psy || !psy->desc || !psy->desc->name)
		return false;

	name = psy->desc->name;
	return !strncmp(name, "tcpm-source-psy-", 16);
}

static int smb_read_tcpm_contract(struct smb_chip *chip, struct power_supply *psy)
{
	union power_supply_propval volt = { 0 }, curr = { 0 };
	int rc;

	rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &volt);
	if (rc || !volt.intval) {
		chip->pd_icl_ua = 0;
		chip->pd_vbus_uv = 0;
		return 0;
	}

	rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
				       &volt);
	if (rc)
		return rc;

	rc = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX,
				       &curr);
	if (rc)
		return rc;

	chip->pd_vbus_uv = volt.intval;
	chip->pd_icl_ua = curr.intval;
	return 0;
}

static int smb_notifier_call(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	struct smb_chip *chip = container_of(nb, struct smb_chip, nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_DONE;

	if (smb_psy_is_tcpm(psy)) {
		mutex_lock(&chip->lock);
		smb_read_tcpm_contract(chip, psy);
		mutex_unlock(&chip->lock);
		schedule_delayed_work(&chip->status_change_work,
				      msecs_to_jiffies(100));
		return NOTIFY_OK;
	}

	if (chip->cp_psy && psy == chip->cp_psy) {
		schedule_delayed_work(&chip->status_change_work, 0);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static void smb_external_power_changed(struct power_supply *psy)
{
	struct smb_chip *chip = power_supply_get_drvdata(psy);

	schedule_delayed_work(&chip->status_change_work,
			      msecs_to_jiffies(100));
}

static int smb_get_iio_chan(struct smb_chip *chip, struct iio_channel *chan,
			     int *val)
{
	if (IS_ERR(chan)) {
		*val = 0;
		return 0;
	}

	return iio_read_channel_processed(chan, val);
}

static int smb5_get_prop_health(struct smb_chip *chip, int *val)
{
	int rc;
	unsigned int stat;

	rc = smbx_ov_status(chip);

	/* Treat any error as if we are in the overvoltage state */
	if (rc < 0)
		dev_err(chip->dev, "Couldn't determine overvoltage status!");
	if (rc) {
		dev_err(chip->dev, "battery over-voltage");
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		return 0;
	}

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_7,
			 &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read charger status 7 rc=%d\n", rc);
		return rc;
	}

	if (stat & SMB5_BAT_TEMP_STATUS_TOO_COLD_BIT)
		*val = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & SMB5_BAT_TEMP_STATUS_TOO_HOT_BIT)
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & SMB5_BAT_TEMP_STATUS_COLD_SOFT_BIT)
		*val = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & SMB5_BAT_TEMP_STATUS_HOT_SOFT_BIT)
		*val = POWER_SUPPLY_HEALTH_WARM;
	else
		*val = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static int smb2_get_prop_health(struct smb_chip *chip, int *val)
{
	int rc;
	unsigned int stat;

	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_2,
			 &stat);
	if (rc < 0) {
		dev_err(chip->dev, "Couldn't read charger status rc=%d\n", rc);
		return rc;
	}

	switch (stat) {
	case SMB2_CHARGER_ERROR_STATUS_BAT_OV_BIT:
		*val = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		return 0;
	case SMB2_BAT_TEMP_STATUS_TOO_COLD_BIT:
		*val = POWER_SUPPLY_HEALTH_COLD;
		return 0;
	case SMB2_BAT_TEMP_STATUS_TOO_HOT_BIT:
		*val = POWER_SUPPLY_HEALTH_OVERHEAT;
		return 0;
	case SMB2_BAT_TEMP_STATUS_COLD_SOFT_LIMIT_BIT:
		*val = POWER_SUPPLY_HEALTH_COOL;
		return 0;
	case SMB2_BAT_TEMP_STATUS_HOT_SOFT_LIMIT_BIT:
		*val = POWER_SUPPLY_HEALTH_WARM;
		return 0;
	default:
		*val = POWER_SUPPLY_HEALTH_GOOD;
		return 0;
	}
}

static int smb_get_prop_health(struct smb_chip *chip, int *val)
{
	switch (chip->gen) {
	case SMB2:
		return smb2_get_prop_health(chip, val);
	case SMB5:
		return smb5_get_prop_health(chip, val);
	default:
		dev_err(chip->dev, "Unsupported SMB chip generation\n");
		return -EINVAL;
	}
}

static int smb_get_property(struct power_supply *psy,
			     enum power_supply_property psp,
			     union power_supply_propval *val)
{
	struct smb_chip *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Qualcomm";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = chip->name;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return smb_get_current_limit(chip, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return smb_get_iio_chan(chip, chip->usb_in_i_chan,
					 &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = smb_get_iio_chan(chip, chip->usb_in_v_chan,
					 &val->intval);
		if (!ret) {
			if (chip->gen == SMB5)
				val->intval *= 16;
		}
		return ret;
	case POWER_SUPPLY_PROP_ONLINE:
		return smb_get_prop_usb_online(chip, &val->intval);
	case POWER_SUPPLY_PROP_STATUS:
		return smb_get_prop_status(chip, &val->intval);
	case POWER_SUPPLY_PROP_HEALTH:
		return smb_get_prop_health(chip, &val->intval);
	case POWER_SUPPLY_PROP_USB_TYPE:
		return smb_apsd_get_charger_type(chip, &val->intval);
	default:
		dev_err(chip->dev, "invalid property: %d\n", psp);
		return -EINVAL;
	}
}

static int smb_set_property(struct power_supply *psy,
			     enum power_supply_property psp,
			     const union power_supply_propval *val)
{
	struct smb_chip *chip = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return regmap_update_bits(chip->regmap, chip->base + USBIN_CMD_IL,
					  USBIN_SUSPEND_BIT, !val->intval);
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return smb_set_current_limit(chip, val->intval);
	default:
		dev_err(chip->dev, "No setter for property: %d\n", psp);
		return -EINVAL;
	}
}

static int smb_property_is_writable(struct power_supply *psy,
				     enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return 1;
	default:
		return 0;
	}
}

static irqreturn_t smb_handle_batt_overvoltage(int irq, void *data)
{
	struct smb_chip *chip = data;

	if (smbx_ov_status(chip) == 1) {
		/* The hardware stops charging automatically */
		dev_err(chip->dev, "battery overvoltage detected\n");
		power_supply_changed(chip->chg_psy);
	}

	return IRQ_HANDLED;
}

static irqreturn_t smb_handle_usb_plugin(int irq, void *data)
{
	struct smb_chip *chip = data;

	power_supply_changed(chip->chg_psy);

	schedule_delayed_work(&chip->status_change_work,
			      msecs_to_jiffies(1500));

	return IRQ_HANDLED;
}

static irqreturn_t smb_handle_usb_icl_change(int irq, void *data)
{
	struct smb_chip *chip = data;

	power_supply_changed(chip->chg_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smb_handle_wdog_bark(int irq, void *data)
{
	struct smb_chip *chip = data;
	int rc;

	power_supply_changed(chip->chg_psy);

	rc = regmap_write(chip->regmap, BARK_BITE_WDOG_PET,
			  BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		dev_err(chip->dev, "Couldn't pet the dog rc=%d\n", rc);

	return IRQ_HANDLED;
}

static const struct power_supply_desc smb_psy_desc = {
	.name = "SMB2_charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN),
	.properties = smb_properties,
	.num_properties = ARRAY_SIZE(smb_properties),
	.get_property = smb_get_property,
	.set_property = smb_set_property,
	.property_is_writeable = smb_property_is_writable,
	.external_power_changed = smb_external_power_changed,
};

/* Init sequence derived from vendor downstream driver */
static const struct smb_init_register smb5_init_seq[] = {
{ .addr = USBIN_CMD_IL, .mask = USBIN_SUSPEND_BIT, .val = 0 },
/*
 * By default configure us as an upstream facing port
 * FIXME: This will be handled by the type-c driver
 */
{ .addr = SMB5_TYPE_C_MODE_CFG,
  .mask = SMB5_EN_TRY_SNK_BIT | SMB5_EN_SNK_ONLY_BIT,
  .val = SMB5_EN_TRY_SNK_BIT },
{ .addr = SMB5_TYPEC_TYPE_C_VCONN_CONTROL,
  .mask = SMB5_VCONN_EN_ORIENTATION_BIT | SMB5_VCONN_EN_SRC_BIT |
  SMB5_VCONN_EN_VALUE_BIT,
  .val = SMB2_VCONN_EN_SRC_BIT },
{ .addr = SMB5_DEBUG_ACCESS_SRC_CFG,
  .mask = SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT,
  .val = SMB5_EN_UNORIENTED_DEBUG_ACCESS_SRC_BIT },
{ .addr = SMB5_TYPE_C_EXIT_STATE_CFG,
  .mask = SMB5_SEL_SRC_UPPER_REF_BIT,
  .val = SMB5_SEL_SRC_UPPER_REF_BIT },
/*
 * Disable Type-C factory mode and stay in Attached.SRC state when VCONN
 * over-current happens
 */
{ .addr = TYPE_C_CFG,
  .mask = APSD_START_ON_CC_BIT,
  .val = 0 },
{ .addr = SMB5_TYPE_C_DEBUG_ACCESS_SINK,
  .mask = SMB5_TYPEC_DEBUG_ACCESS_SINK_MASK,
  .val = 0x17 },
/* Configure VBUS for software control */
{ .addr = OTG_CFG, .mask = OTG_EN_SRC_CFG_BIT, .val = 0 },
/*
 * Recharge when State Of Charge drops below 98%.
 */
{ .addr = SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_REG,
  .mask = SMB5_CHARGE_RCHG_SOC_THRESHOLD_CFG_MASK,
  .val = 250 },
/* Enable charging */
{ .addr = CHARGING_ENABLE_CMD,
  .mask = CHARGING_ENABLE_CMD_BIT,
  .val = CHARGING_ENABLE_CMD_BIT },
/* Enable factory mode detection + don't wait for CC before APSD */
{ .addr = TYPE_C_CFG,
  .mask = APSD_START_ON_CC_BIT | FACTORY_MODE_DETECTION_EN_BIT,
  .val = FACTORY_MODE_DETECTION_EN_BIT },
/* Force uUSB factory mode so D+/D- get routed to charger for APSD/QC */
{ .addr = TYPEC_U_USB_CFG_REG,
  .mask = EN_MICRO_USB_FACTORY_MODE_BIT | EN_MICRO_USB_MODE_BIT,
  .val = EN_MICRO_USB_FACTORY_MODE_BIT | EN_MICRO_USB_MODE_BIT },
/* Disable Type-C DRP state machine — it interferes with D+/D- QC
 * negotiation when no CC signal is present (USB-A-to-C cable).
 * Set EN_SNK_ONLY_BIT to prevent Type-C from trying source role. */
{ .addr = 0x544,	/* TYPE_C_MODE_CFG_REG */
  .mask = 0x1f,	/* EN_TRY_SNK|EN_SRC_ONLY|EN_SNK_ONLY etc */
  .val = BIT(1) },	/* EN_SNK_ONLY_BIT - sink only, no DRP */
/* Enable HVDCP auth + QC enable (no autonomous mode - vendor driver
 * uses software SINGLE_INCREMENT for QC3.0 voltage control) */
{ .addr = USBIN_OPTIONS_1_CFG,
  .mask = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT | HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT,
  .val = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT },
/* Allow all input voltages (5V-12V) for QC negotiation.
 * Without this, ADAPTER_ALLOW_OVERRIDE defaults to FORCE_5V
 * which blocks QC voltage increase. */
{ .addr = USBIN_ADAPTER_ALLOW_OVERRIDE,
  .mask = 0x0f,
  .val = ADAPTER_ALLOW_FORCE_NULL },
/* Set the default SDP charger type to a 500ma USB 2.0 port */
{ .addr = USBIN_ICL_OPTIONS,
  .mask = USBIN_MODE_CHG_BIT,
  .val = USBIN_MODE_CHG_BIT },
{ .addr = CMD_ICL_OVERRIDE,
  .mask = ICL_OVERRIDE_BIT,
  .val = ICL_OVERRIDE_BIT },
{ .addr = USBIN_CURRENT_LIMIT_CFG,
  .mask = 0xff,
  .val = 3000000 / CURRENT_SCALE_FACTOR },
{ .addr = USBIN_LOAD_CFG,
  .mask = ICL_OVERRIDE_AFTER_APSD_BIT,
  .val = ICL_OVERRIDE_AFTER_APSD_BIT },
/* Disable watchdog */
{ .addr = SNARL_BARK_BITE_WD_CFG, .mask = 0xff, .val = 0 },
{ .addr = WD_CFG,
  .mask = WATCHDOG_TRIGGER_AFP_EN_BIT | WDOG_TIMER_EN_ON_PLUGIN_BIT |
  BARK_WDOG_INT_EN_BIT,
  .val = 0 },
/*
 * This overrides all of the other current limit configs and is
 * expected to be used for setting limits based on temperature.
 * We set some relatively safe default value while still allowing
 * a comfortably fast charging rate.
 */
{ .addr = FAST_CHARGE_CURRENT_CFG,
  .mask = FAST_CHARGE_CURRENT_SETTING_MASK,
  .val = 1950000 / CURRENT_SCALE_FACTOR },
/*
 * Enable Automatic Input Current Limit, this will slowly ramp up the current
 * When connected to a wall charger, and automatically stop when it detects
 * the charger current limit (voltage drop?) or it reaches the programmed limit.
 */
{ .addr = USBIN_AICL_OPTIONS_CFG,
  .mask = USBIN_AICL_PERIODIC_RERUN_EN_BIT | USBIN_AICL_ADC_EN_BIT |
	  USBIN_AICL_EN_BIT | SUSPEND_ON_COLLAPSE_USBIN_BIT |
	  USBIN_AICL_START_AT_MAX_BIT,
  .val = USBIN_AICL_ADC_EN_BIT },
};

/* Init sequence derived from vendor downstream driver */
static const struct smb_init_register smb2_init_seq[] = {
	/*
	 * By default configure us as an upstream facing port
	 * FIXME: This will be handled by the type-c driver
	 */
	{ .addr = TYPE_C_INTRPT_ENB_SOFTWARE_CTRL,
	  .mask = TYPEC_POWER_ROLE_CMD_MASK | SMB2_VCONN_EN_SRC_BIT |
		  VCONN_EN_VALUE_BIT,
	  .val = SMB2_VCONN_EN_SRC_BIT },
	/*
	 * Disable Type-C factory mode and stay in Attached.SRC state when VCONN
	 * over-current happens
	 */
	{ .addr = TYPE_C_CFG,
	  .mask = FACTORY_MODE_DETECTION_EN_BIT | VCONN_OC_CFG_BIT,
	  .val = 0 },
	/* Configure VBUS for software control */
	{ .addr = OTG_CFG, .mask = OTG_EN_SRC_CFG_BIT, .val = 0 },
	/*
	 * Use VBAT to determine the recharge threshold when battery is full
	 * rather than the state of charge.
	 */
	{ .addr = SMB2_FG_UPDATE_CFG_2_SEL,
	  .mask = SMB2_SOC_LT_CHG_RECHARGE_THRESH_SEL_BIT |
		  SMB2_VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT,
	  .val = SMB2_VBT_LT_CHG_RECHARGE_THRESH_SEL_BIT },
	/* Enable charging */
	{ .addr = USBIN_OPTIONS_1_CFG, .mask = HVDCP_EN_BIT, .val = 0 },
	{ .addr = CHARGING_ENABLE_CMD,
	  .mask = CHARGING_ENABLE_CMD_BIT,
	  .val = CHARGING_ENABLE_CMD_BIT },
	/*
	 * Match downstream defaults
	 * CHG_EN_SRC_BIT - charger enable is controlled by software
	 * CHG_EN_POLARITY_BIT - polarity of charge enable pin when in HW control
	 *                       pulled low on OnePlus 6 and SHIFT6mq
	 * PRETOFAST_TRANSITION_CFG_BIT -
	 * BAT_OV_ECC_BIT -
	 * I_TERM_BIT - Current termination ?? 0 = enabled
	 * AUTO_RECHG_BIT - Enable automatic recharge when battery is full
	 *                  0 = enabled
	 * EN_ANALOG_DROP_IN_VBATT_BIT
	 * CHARGER_INHIBIT_BIT - Inhibit charging based on battery voltage
	 *                       instead of ??
	 */
	{ .addr = CHGR_CFG2,
	  .mask = CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT |
		  PRETOFAST_TRANSITION_CFG_BIT | BAT_OV_ECC_BIT | I_TERM_BIT |
		  AUTO_RECHG_BIT | EN_ANALOG_DROP_IN_VBATT_BIT |
		  CHARGER_INHIBIT_BIT,
	  .val = CHARGER_INHIBIT_BIT },
	/* STAT pin software override, match downstream. Parallel charging? */
	{ .addr = STAT_CFG,
	  .mask = STAT_SW_OVERRIDE_CFG_BIT,
	  .val = STAT_SW_OVERRIDE_CFG_BIT },
	/* Set the default SDP charger type to a 500ma USB 2.0 port */
	{ .addr = USBIN_ICL_OPTIONS,
	  .mask = USB51_MODE_BIT | USBIN_MODE_CHG_BIT,
	  .val = USB51_MODE_BIT },
	/* Disable watchdog */
	{ .addr = SNARL_BARK_BITE_WD_CFG, .mask = 0xff, .val = 0 },
	{ .addr = WD_CFG,
	  .mask = WATCHDOG_TRIGGER_AFP_EN_BIT | WDOG_TIMER_EN_ON_PLUGIN_BIT |
		  BARK_WDOG_INT_EN_BIT,
	  .val = 0 },
	/* These bits aren't documented anywhere */
	{ .addr = USBIN_5V_AICL_THRESHOLD_CFG,
	  .mask = USBIN_5V_AICL_THRESHOLD_CFG_MASK,
	  .val = 0x3 },
	{ .addr = USBIN_CONT_AICL_THRESHOLD_CFG,
	  .mask = USBIN_CONT_AICL_THRESHOLD_CFG_MASK,
	  .val = 0x3 },
	/*
	 * Enable Automatic Input Current Limit, this will slowly ramp up the current
	 * When connected to a wall charger, and automatically stop when it detects
	 * the charger current limit (voltage drop?) or it reaches the programmed limit.
	 */
	{ .addr = USBIN_AICL_OPTIONS_CFG,
	  .mask = USBIN_AICL_START_AT_MAX_BIT | USBIN_AICL_ADC_EN_BIT |
		  USBIN_AICL_EN_BIT | SUSPEND_ON_COLLAPSE_USBIN_BIT |
		  USBIN_HV_COLLAPSE_RESPONSE_BIT |
		  USBIN_LV_COLLAPSE_RESPONSE_BIT,
	  .val = USBIN_HV_COLLAPSE_RESPONSE_BIT |
		 USBIN_LV_COLLAPSE_RESPONSE_BIT | USBIN_AICL_EN_BIT },
	/*
	 * Set pre charge current to default, the OnePlus 6 bootloader
	 * sets this very conservatively.
	 */
	{ .addr = PRE_CHARGE_CURRENT_CFG,
	  .mask = PRE_CHARGE_CURRENT_SETTING_MASK,
	  .val = 500000 / CURRENT_SCALE_FACTOR },
};

struct smb_match_data pmi8998_match_data = {
	.init_seq = smb2_init_seq,
	.init_seq_len = ARRAY_SIZE(smb2_init_seq),
	.name = "pmi8998",
	.gen = SMB2,
};

struct smb_match_data pm660_match_data = {
	.init_seq = smb2_init_seq,
	.init_seq_len = ARRAY_SIZE(smb2_init_seq),
	.name = "pm660",
	.gen = SMB2,
};

struct smb_match_data pm8150b_match_data = {
	.init_seq = smb5_init_seq,
	.init_seq_len = ARRAY_SIZE(smb5_init_seq),
	.name = "pm8150b",
	.gen = SMB5,
};

struct smb_match_data pm7250b_match_data = {
	.init_seq = smb5_init_seq,
	.init_seq_len = ARRAY_SIZE(smb5_init_seq),
	.name = "pm7250b",
	.gen = SMB5,
};


static int smb_init_hw(struct smb_chip *chip, const struct smb_init_register *init_seq, size_t len)
{
	int rc, i;

	for (i = 0; i < len; i++) {
		rc = regmap_update_bits(chip->regmap,
					chip->base + init_seq[i].addr,
					init_seq[i].mask,
					init_seq[i].val);
		if (rc < 0)
			return dev_err_probe(chip->dev, rc,
					     "%s: init command %d failed\n",
					     __func__, i);
	}

	return 0;
}

static int smb_init_irq(struct smb_chip *chip, int *irq, const char *name,
			 irqreturn_t (*handler)(int irq, void *data))
{
	int irqnum;
	int rc;

	irqnum = platform_get_irq_byname(to_platform_device(chip->dev), name);
	if (irqnum < 0)
		return irqnum;

	rc = devm_request_threaded_irq(chip->dev, irqnum, NULL, handler,
				       IRQF_ONESHOT, name, chip);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc, "Couldn't request irq %s\n",
				     name);

	if (irq)
		*irq = irqnum;

	return 0;
}

static int smb_probe(struct platform_device *pdev)
{
	struct power_supply_config supply_config = {};
	struct power_supply_desc *desc;
	struct smb_chip *chip;
	const struct smb_match_data *match_data;
	int rc, irq;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &pdev->dev;
	chip->name = pdev->name;

	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV,
				     "failed to locate the regmap\n");

	rc = device_property_read_u32(chip->dev, "reg", &chip->base);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Couldn't read base address\n");

	chip->usb_in_v_chan = devm_iio_channel_get(chip->dev, "usbin_v");
	if (IS_ERR(chip->usb_in_v_chan))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_v_chan),
				     "Couldn't get usbin_v IIO channel\n");

	chip->usb_in_i_chan = devm_iio_channel_get(chip->dev, "usbin_i");
	if (IS_ERR(chip->usb_in_i_chan)) {
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_i_chan),
				     "Couldn't get usbin_i IIO channel\n");
	}

	match_data = (const struct smb_match_data *)device_get_match_data(chip->dev);

	chip->gen = match_data->gen;

	dev_info(chip->dev, "Generation %s\n", chip->gen == SMB2 ? "SMB2" : "SMB5");

	/* Get DPDM regulator for D+/D- signal routing.
	 * Try "dpdm" supply name (DTS dpdm-supply), then fall
	 * back to global lookup by regulator name.
	 */
	chip->dpdm_reg = devm_regulator_get(chip->dev, "dpdm");
	if (IS_ERR(chip->dpdm_reg)) {
		dev_err(chip->dev, "dpdm regulator DTS lookup failed: %ld, trying global\n",
			PTR_ERR(chip->dpdm_reg));
		chip->dpdm_reg = regulator_get(NULL, "dpdm");
		if (IS_ERR(chip->dpdm_reg)) {
			chip->dpdm_reg = NULL;
			dev_info(chip->dev, "No dpdm regulator (D+/D- routed via uUSB mode)\n");
		} else {
			dev_info(chip->dev, "Found dpdm regulator via global lookup\n");
		}
	} else {
		dev_info(chip->dev, "Found dpdm regulator via DTS\n");
	}

	/* Enable dpdm regulator immediately at probe so that:
	 * 1) PHY releases D+/D- lines to PMIC for APSD/QC
	 * 2) regulator_init_complete doesn't auto-disable it
	 */
	if (chip->dpdm_reg) {
		rc = regulator_enable(chip->dpdm_reg);
		if (rc < 0) {
			dev_err(chip->dev, "Couldn't enable dpdm regulator rc=%d\n", rc);
		} else {
			chip->dpdm_enabled = true;
			dev_info(chip->dev, "DPDM regulator enabled at probe\n");
		}
	}

	rc = smb_init_hw(chip, match_data->init_seq, match_data->init_seq_len);
	if (rc < 0)
		return rc;

	supply_config.drv_data = chip;
	supply_config.fwnode = dev_fwnode(&pdev->dev);

	desc = devm_kzalloc(chip->dev, sizeof(smb_psy_desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;
	memcpy(desc, &smb_psy_desc, sizeof(smb_psy_desc));
	desc->name =
		devm_kasprintf(chip->dev, GFP_KERNEL, "%s-charger",
			       match_data->name);
	if (!desc->name)
		return -ENOMEM;

	chip->chg_psy =
		devm_power_supply_register(chip->dev, desc, &supply_config);
	if (IS_ERR(chip->chg_psy))
		return dev_err_probe(chip->dev, PTR_ERR(chip->chg_psy),
				     "failed to register power supply\n");

	rc = power_supply_get_battery_info(chip->chg_psy, &chip->batt_info);
	if (rc)
		return dev_err_probe(chip->dev, rc,
				     "Failed to get battery info\n");
	if (chip->batt_info->constant_charge_current_max_ua == -EINVAL)
		chip->batt_info->constant_charge_current_max_ua = DCP_CURRENT_UA;

	mutex_init(&chip->lock);

	rc = devm_delayed_work_autocancel(chip->dev, &chip->status_change_work,
					  smb_status_change_work);
	if (rc)
		return dev_err_probe(chip->dev, rc,
				     "Failed to init status change work\n");

	rc = (chip->batt_info->voltage_max_design_uv - 3487500) / 7500 + 1;
	rc = regmap_update_bits(chip->regmap, chip->base + FLOAT_VOLTAGE_CFG,
				FLOAT_VOLTAGE_SETTING_MASK, rc);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc, "Couldn't set vbat max\n");

	rc = smb_init_irq(chip, &irq, "bat-ov", smb_handle_batt_overvoltage);
	if (rc < 0)
		return rc;

	rc = smb_init_irq(chip, &chip->cable_irq, "usb-plugin",
			   smb_handle_usb_plugin);
	if (rc < 0)
		return rc;

	rc = smb_init_irq(chip, &irq, "usbin-icl-change",
			   smb_handle_usb_icl_change);
	if (rc < 0)
		return rc;
	rc = smb_init_irq(chip, &irq, "wdog-bark", smb_handle_wdog_bark);
	if (rc < 0)
		return rc;

	devm_device_init_wakeup(chip->dev);

	rc = devm_pm_set_wake_irq(chip->dev, chip->cable_irq);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc, "Couldn't set wake irq\n");

	platform_set_drvdata(pdev, chip);

	/*
	 * Program FCC from DT constant-charge-current-max. Charge-pump mode
	 * may raise this further to ~2×ICL (capped at 5.1A).
	 */
	rc = smb_set_fcc(chip, smb_default_fcc_ua(chip));
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Couldn't write fast charge current cfg");

	rc = regmap_write_bits(chip->regmap, chip->base + AICL_RERUN_TIME_CFG,
			       AICL_RERUN_TIME_MASK, AIC_RERUN_TIME_3_SECS);
	if (rc < 0)
		return dev_err_probe(chip->dev, rc,
				     "Couldn't write fast AICL rerun time");

	chip->nb.notifier_call = smb_notifier_call;
	rc = power_supply_reg_notifier(&chip->nb);
	if (rc)
		return dev_err_probe(chip->dev, rc,
				     "Failed to register psy notifier\n");

	/* Initialise charger state */
	schedule_delayed_work(&chip->status_change_work, 0);

	return 0;
}

static void smb_remove(struct platform_device *pdev)
{
	struct smb_chip *chip = platform_get_drvdata(pdev);

	power_supply_unreg_notifier(&chip->nb);
	if (chip->cp_psy) {
		power_supply_put(chip->cp_psy);
		chip->cp_psy = NULL;
	}
	smb_cp_hw_enable(chip, false);
}

static const struct of_device_id smb_match_id_table[] = {
	{ .compatible = "qcom,pmi8998-charger", .data = &pmi8998_match_data },
	{ .compatible = "qcom,pm660-charger", .data = &pm660_match_data },
	{ .compatible = "qcom,pm7250b-charger", .data = &pm7250b_match_data },
	{ .compatible = "qcom,pm8150b-charger", .data = &pm8150b_match_data },
	{ /* sentinal */ }
};
MODULE_DEVICE_TABLE(of, smb_match_id_table);

static struct platform_driver qcom_spmi_smb = {
	.probe = smb_probe,
	.remove = smb_remove,
	.driver = {
		.name = "qcom-smbx-charger",
		.of_match_table = smb_match_id_table,
		},
};

module_platform_driver(qcom_spmi_smb);

MODULE_AUTHOR("Casey Connolly <casey.connolly@linaro.org>");
MODULE_DESCRIPTION("Qualcomm SMB2 Charger Driver");
MODULE_LICENSE("GPL");
