// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm SMB1390 charge-pump charger (mainline-friendly port).
 *
 * Register sequence and ILIM formula follow the Android smb1390-charger driver
 * used on Xiaomi Raphael (SM8150 + PM8150B), without pmic-voter / i2c-pmic MFD.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CORE_STATUS1_REG		0x1006
#define WIN_OV_BIT			BIT(0)
#define WIN_UV_BIT			BIT(1)
#define EN_PIN_OUT_BIT			BIT(2)
#define ILIM_BIT			BIT(5)
#define TEMP_ALARM_BIT			BIT(6)

#define CORE_STATUS2_REG		0x1007
#define SWITCHER_HOLD_OFF_BIT		BIT(0)
#define VPH_OV_HARD_BIT			BIT(1)
#define TSD_BIT				BIT(2)
#define IREV_BIT			BIT(3)
#define IOC_BIT				BIT(4)
#define VIN_UV_BIT			BIT(5)
#define VIN_OV_BIT			BIT(6)
#define EN_PIN_OUT2_BIT			BIT(7)

#define CORE_INT_RT_STS_REG		0x1010

#define CORE_CONTROL1_REG		0x1020
#define CMD_EN_SWITCHER_BIT		BIT(0)

#define CORE_FTRIM_ILIM_REG		0x1030
#define CFG_ILIM_MASK			GENMASK(4, 0)

#define CORE_FTRIM_MISC_XIAOMI		0x1032

#define CORE_FTRIM_LVL_REG		0x1033
#define CFG_WIN_HI_MASK			GENMASK(3, 2)
#define WIN_OV_LVL_1000MV		0x08

#define CORE_FTRIM_MISC_REG		0x1034
#define TR_WIN_1P5X_BIT			BIT(0)

#define SMB1390_ILIM_MIN_UA		900000
#define SMB1390_CP_MIN_VBUS_UV		8000000

struct smb1390_chip {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *primary_psy;
	struct mutex lock;
	struct delayed_work update_work;
	struct work_struct late_init_work;
	struct notifier_block nb;

	bool online;
	bool switcher_on;
	unsigned int ilim_ua;
	int irq;
};

static const struct regmap_config smb1390_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

static int smb1390_masked_write(struct smb1390_chip *chip, unsigned int reg,
				unsigned int mask, unsigned int val)
{
	return regmap_update_bits(chip->regmap, reg, mask, val);
}

static int smb1390_init_hw(struct smb1390_chip *chip)
{
	int rc;

	rc = smb1390_masked_write(chip, CORE_FTRIM_LVL_REG, CFG_WIN_HI_MASK,
				  WIN_OV_LVL_1000MV);
	if (rc)
		return rc;

	rc = smb1390_masked_write(chip, CORE_FTRIM_MISC_REG, TR_WIN_1P5X_BIT, 0);
	if (rc)
		return rc;

	/* Xiaomi Raphael trim */
	return smb1390_masked_write(chip, CORE_FTRIM_MISC_XIAOMI, 0x0f, 0x07);
}

static int smb1390_set_ilim(struct smb1390_chip *chip, unsigned int ilim_ua)
{
	unsigned int code;
	int rc;

	if (ilim_ua < SMB1390_ILIM_MIN_UA)
		return -EINVAL;

	code = DIV_ROUND_CLOSEST(ilim_ua - 500000, 100000);
	rc = smb1390_masked_write(chip, CORE_FTRIM_ILIM_REG, CFG_ILIM_MASK,
				  code & CFG_ILIM_MASK);
	dev_info(chip->dev, "set_ilim=%u code=%u rc=%d\n", ilim_ua, code, rc);
	return rc;
}

static int smb1390_set_switcher(struct smb1390_chip *chip, bool enable)
{
	int rc;

	rc = smb1390_masked_write(chip, CORE_CONTROL1_REG, CMD_EN_SWITCHER_BIT,
				  enable ? CMD_EN_SWITCHER_BIT : 0);
	dev_info(chip->dev, "set_switcher=%d rc=%d\n", enable, rc);
	if (!rc)
		chip->switcher_on = enable;

	return rc;
}

static int smb1390_enable(struct smb1390_chip *chip, bool enable)
{
	int rc = 0;

	lockdep_assert_held(&chip->lock);

	dev_info(chip->dev, "enable=%d online=%d switcher_on=%d ilim=%u\n",
		 enable, chip->online, chip->switcher_on, chip->ilim_ua);

	if (enable == chip->online && (!enable || chip->switcher_on))
		return 0;

	if (!enable) {
		rc = smb1390_set_switcher(chip, false);
		chip->online = false;
		return rc;
	}

	if (chip->ilim_ua < SMB1390_ILIM_MIN_UA) {
		dev_dbg(chip->dev, "ILIM %u uA too low; not enabling\n",
			chip->ilim_ua);
		return -EINVAL;
	}

	rc = smb1390_set_ilim(chip, chip->ilim_ua);
	if (rc)
		return rc;

	/* Allow SMB_EN from primary charger to settle */
	usleep_range(1000, 2000);

	rc = smb1390_set_switcher(chip, true);
	if (rc)
		return rc;

	chip->online = true;
	dev_info(chip->dev, "switcher enabled (ILIM=%u uA)\n", chip->ilim_ua);
	return 0;
}

static int smb1390_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct smb1390_chip *chip = power_supply_get_drvdata(psy);
	unsigned int status1 = 0, status2 = 0;
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = "Qualcomm";
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "SMB1390";
		return 0;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = chip->online && chip->switcher_on;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = chip->ilim_ua;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		if (!chip->online) {
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
			return 0;
		}
		rc = regmap_read(chip->regmap, CORE_STATUS1_REG, &status1);
		if (rc)
			return rc;
		rc = regmap_read(chip->regmap, CORE_STATUS2_REG, &status2);
		if (rc)
			return rc;
		dev_info(chip->dev, "status1=0x%02x status2=0x%02x\n", status1, status2);
		if ((status1 & (WIN_OV_BIT | WIN_UV_BIT | TEMP_ALARM_BIT)) ||
		    (status2 & (TSD_BIT | IREV_BIT | VPH_OV_HARD_BIT)))
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (status1 & EN_PIN_OUT_BIT)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		return 0;
	default:
		return -EINVAL;
	}
}

static int smb1390_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	struct smb1390_chip *chip = power_supply_get_drvdata(psy);
	int rc = 0;

	mutex_lock(&chip->lock);

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		dev_info(chip->dev, "set CURRENT_MAX=%d\n", val->intval);
		chip->ilim_ua = val->intval;
		if (chip->online && chip->ilim_ua >= SMB1390_ILIM_MIN_UA)
			rc = smb1390_set_ilim(chip, chip->ilim_ua);
		else if (chip->online)
			rc = smb1390_enable(chip, false);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		dev_info(chip->dev, "set ONLINE=%d\n", val->intval);
		rc = smb1390_enable(chip, !!val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&chip->lock);

	if (!rc)
		power_supply_changed(chip->psy);

	return rc;
}

static int smb1390_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property smb1390_props[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_STATUS,
};

static const struct power_supply_desc smb1390_psy_desc = {
	.name = "smb1390-charger",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.properties = smb1390_props,
	.num_properties = ARRAY_SIZE(smb1390_props),
	.get_property = smb1390_get_property,
	.set_property = smb1390_set_property,
	.property_is_writeable = smb1390_property_is_writeable,
};

static void smb1390_update_from_primary(struct smb1390_chip *chip)
{
	union power_supply_propval online = { 0 }, icl = { 0 }, vbus = { 0 };
	int rc;

	if (!chip->primary_psy)
		chip->primary_psy = power_supply_get_by_reference(
			dev_fwnode(chip->dev), "qcom,primary-charger");

	if (IS_ERR_OR_NULL(chip->primary_psy)) {
		chip->primary_psy = NULL;
		return;
	}

	rc = power_supply_get_property(chip->primary_psy,
				       POWER_SUPPLY_PROP_ONLINE, &online);
	if (rc || !online.intval) {
		mutex_lock(&chip->lock);
		smb1390_enable(chip, false);
		mutex_unlock(&chip->lock);
		return;
	}

	/* Read VBUS voltage from primary charger to confirm QC3.0
	 * has negotiated 9V before enabling the 2:1 charge pump.
	 */
	rc = power_supply_get_property(chip->primary_psy,
				       POWER_SUPPLY_PROP_VOLTAGE_NOW, &vbus);
	dev_info(chip->dev, "VOLTAGE_NOW rc=%d val=%d\n", rc, vbus.intval);
	if (rc < 0)
		vbus.intval = 0;

	/* Try I2C ADC directly if power_supply returns 0 */
	if (vbus.intval == 0) {
		struct iio_channel *usbin_v;
		usbin_v = iio_channel_get(chip->dev, "usbin_v");
		if (!IS_ERR(usbin_v)) {
			int iio_rc = iio_read_channel_processed(usbin_v, &vbus.intval);
			dev_info(chip->dev, "IIO direct rc=%d val=%d\n", iio_rc, vbus.intval);
			iio_channel_release(usbin_v);
		} else {
			dev_info(chip->dev, "IIO channel get failed: %ld\n", PTR_ERR(usbin_v));
		}
	}

	/* Need VBUS >= 8V for charge pump 2:1 mode (9V→4.5V/6A=27W).
	 * If VBUS is 0 (ADC not ready yet), skip — don't enable CP
	 * until we have a valid high-voltage reading.
	 */
	if (vbus.intval <= 0 || vbus.intval < SMB1390_CP_MIN_VBUS_UV) {
		dev_info(chip->dev, "VBUS=%d uV < %d, CP not enabled\n",
			vbus.intval, SMB1390_CP_MIN_VBUS_UV);
		if (chip->online) {
			mutex_lock(&chip->lock);
			smb1390_enable(chip, false);
			mutex_unlock(&chip->lock);
		}
		return;
	}

	rc = power_supply_get_property(chip->primary_psy,
				       POWER_SUPPLY_PROP_CURRENT_MAX, &icl);
	if (rc)
		icl.intval = 3000000; /* default 3A for QC3.0 */

	mutex_lock(&chip->lock);
	/*
	 * Enable only when VBUS >= 8V (QC3.0 negotiated 9V).
	 * ICL alone is not sufficient — CP needs high voltage.
	 */
	if (vbus.intval >= SMB1390_CP_MIN_VBUS_UV && icl.intval >= SMB1390_ILIM_MIN_UA) {
		chip->ilim_ua = max(icl.intval, (int)SMB1390_ILIM_MIN_UA);
		if (!chip->online)
			smb1390_enable(chip, true);
		else
			smb1390_set_ilim(chip, chip->ilim_ua);
	} else if (chip->online) {
		smb1390_enable(chip, false);
	}
	mutex_unlock(&chip->lock);
}

static void smb1390_update_work(struct work_struct *work)
{
	struct smb1390_chip *chip =
		container_of(work, struct smb1390_chip, update_work.work);

	smb1390_update_from_primary(chip);
}

static int smb1390_notifier_call(struct notifier_block *nb, unsigned long event,
				 void *data)
{
	struct smb1390_chip *chip = container_of(nb, struct smb1390_chip, nb);
	struct power_supply *psy = data;

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_DONE;

	if (chip->primary_psy && psy == chip->primary_psy) {
		/* Cancel any pending update and reschedule with a
		 * longer delay to debounce rapid property changes
		 * from the primary charger during QC negotiation.
		 */
		cancel_delayed_work_sync(&chip->update_work);
		schedule_delayed_work(&chip->update_work,
				      msecs_to_jiffies(500));
	}

	return NOTIFY_OK;
}

static irqreturn_t smb1390_stat_irq(int irq, void *data)
{
	struct smb1390_chip *chip = data;
	unsigned int status1 = 0, status2 = 0, rt_sts = 0;

	regmap_read(chip->regmap, CORE_STATUS1_REG, &status1);
	regmap_read(chip->regmap, CORE_STATUS2_REG, &status2);
	regmap_read(chip->regmap, CORE_INT_RT_STS_REG, &rt_sts);

	/* Only log meaningful status changes, not spurious zeroes */
	if (status1 || status2 || rt_sts)
		dev_info(chip->dev, "STAT irq: status1=0x%02x status2=0x%02x rt=0x%02x\n",
			status1, status2, rt_sts);

	if ((status2 & (TSD_BIT | IREV_BIT | VPH_OV_HARD_BIT)) ||
	    (status1 & TEMP_ALARM_BIT)) {
		mutex_lock(&chip->lock);
		smb1390_enable(chip, false);
		mutex_unlock(&chip->lock);
		dev_err(chip->dev, "fault; switcher disabled\n");
		/* Only emit changed on fault to avoid udev storm */
		if (chip->psy)
			power_supply_changed(chip->psy);
	}

	return IRQ_HANDLED;
}

static void smb1390_late_init(struct work_struct *work)
{
	struct smb1390_chip *chip =
		container_of(work, struct smb1390_chip, late_init_work);
	struct power_supply_config cfg = {};
	int rc;

	cfg.drv_data = chip;
	cfg.fwnode = dev_fwnode(chip->dev);

	chip->psy = devm_power_supply_register(chip->dev, &smb1390_psy_desc,
					       &cfg);
	if (IS_ERR(chip->psy)) {
		dev_err(chip->dev, "power_supply_register failed: %ld\n",
			PTR_ERR(chip->psy));
		chip->psy = NULL;
		return;
	}

	chip->nb.notifier_call = smb1390_notifier_call;
	rc = power_supply_reg_notifier(&chip->nb);
	if (rc)
		dev_warn(chip->dev, "notifier reg failed: %d\n", rc);

	schedule_delayed_work(&chip->update_work, msecs_to_jiffies(1000));
	dev_info(chip->dev, "SMB1390 late init done\n");
}

static int smb1390_probe(struct i2c_client *client)
{
	struct smb1390_chip *chip;
	unsigned int status;
	int rc;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &smb1390_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(chip->dev, PTR_ERR(chip->regmap),
				     "regmap init failed\n");

	rc = regmap_read(chip->regmap, CORE_STATUS1_REG, &status);
	if (rc)
		return dev_err_probe(chip->dev, rc,
				     "SMB1390 not responding on I2C\n");

	rc = smb1390_init_hw(chip);
	if (rc)
		return dev_err_probe(chip->dev, rc, "trim init failed\n");

	INIT_DELAYED_WORK(&chip->update_work, smb1390_update_work);
	INIT_WORK(&chip->late_init_work, smb1390_late_init);

	chip->irq = client->irq;
	if (chip->irq > 0) {
		rc = devm_request_threaded_irq(chip->dev, chip->irq, NULL,
					       smb1390_stat_irq, IRQF_ONESHOT,
					       "smb1390-stat", chip);
		if (rc)
			dev_warn(chip->dev, "IRQ request failed: %d\n", rc);
	}

	/* Delay power_supply registration and notifier to avoid
	 * udev event storm during boot. The charge pump hardware
	 * is initialized; userspace and notifier will come later.
	 */
	schedule_work(&chip->late_init_work);

	dev_info(chip->dev, "SMB1390 charge pump probed (status1=0x%02x)\n",
		 status);
	return 0;
}

static void smb1390_remove(struct i2c_client *client)
{
	struct smb1390_chip *chip = i2c_get_clientdata(client);

	cancel_work_sync(&chip->late_init_work);
	power_supply_unreg_notifier(&chip->nb);
	cancel_delayed_work_sync(&chip->update_work);
	mutex_lock(&chip->lock);
	smb1390_enable(chip, false);
	mutex_unlock(&chip->lock);
	if (chip->primary_psy)
		power_supply_put(chip->primary_psy);
}

static const struct of_device_id smb1390_of_match[] = {
	{ .compatible = "qcom,smb1390-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, smb1390_of_match);

static struct i2c_driver smb1390_driver = {
	.driver = {
		.name = "qcom-smb1390-charger",
		.of_match_table = smb1390_of_match,
	},
	.probe = smb1390_probe,
	.remove = smb1390_remove,
};
module_i2c_driver(smb1390_driver);

MODULE_DESCRIPTION("Qualcomm SMB1390 charge pump charger");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Raphael mainline bring-up");
