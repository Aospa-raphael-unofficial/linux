// SPDX-License-Identifier: GPL-2.0
/****************************************************************
 *
 *     idtp9418.c - idtp9418 Wireless Power Charger Monitor driver
 *
 *     (c) 2021 XiaoMi, Inc.
 *     (c) 2025 Viola Guerrera
 *
 *     Original author: bsp-open <bsp-open@xiaomi.com>
 *     Adapted and cleaned up by: Viola Guerrera <viooo@anche.no>
 *     Tested on xiaomi-nabu
 *
 ****************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/power_supply.h>
#include "idtp9418.h"

#define LIMIT_SOC 85
#define CHARGE_MONITOR_INTERVAL 2 * HZ

struct idtp9418_device_info;

struct idtp9418_access_func
{
	int (*read)(struct idtp9418_device_info *di, uint16_t reg, uint8_t *val);
	int (*write)(struct idtp9418_device_info *di, uint16_t reg, uint8_t val);
	int (*read_buf)(struct idtp9418_device_info *di, uint16_t reg, uint8_t *buf,
					uint32_t size);
	int (*write_buf)(struct idtp9418_device_info *di, uint16_t reg, uint8_t *buf,
					 uint32_t size);
};

struct idtp9418_device_info
{
	int chip_enable;
	char *name;
	struct power_supply *psy;
	int charge_limit;
	bool is_charging;
	struct device *dev;
	struct idtp9418_access_func bus;
	struct regmap *regmap;
	struct
	{
		struct gpio_desc *enable;
		struct gpio_desc *boost_enable;
		struct gpio_desc *idt_irq;
		struct gpio_desc *pen_det_active_high;
		struct gpio_desc *pen_det_active_low;
	} gpios;
	struct delayed_work irq_work;
	struct delayed_work hall_irq_work;
	struct delayed_work charge_monitor_work;
	struct mutex i2c_lock;
};

static enum power_supply_property idtp9418_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_ONLINE,
};

void reverse_clrInt(struct idtp9418_device_info *di);

static int idtp9418_read(struct idtp9418_device_info *di, uint16_t reg, uint8_t *val)
{
	unsigned int temp;
	int rc;
	msleep(100);

	mutex_lock(&di->i2c_lock);
	rc = regmap_read(di->regmap, reg, &temp);
	if (rc >= 0)
		*val = (u8)temp;

	mutex_unlock(&di->i2c_lock);
	if (rc < 0)
	{
		dev_err(di->dev, "read error: %d. while reading reg: 0x%04x val: 0x%02x\n",
				rc, reg, *val);
		return rc;
	}
	return rc;
}

static int idtp9418_write(struct idtp9418_device_info *di, uint16_t reg, uint8_t val)
{
	int rc = 0;

	mutex_lock(&di->i2c_lock);
	rc = regmap_write(di->regmap, reg, val);
	if (rc < 0)
	{
		dev_err(di->dev, "write error: %d at reg: 0x%04x val: 0x%02x\n",
				rc, reg, val);
	}
	mutex_unlock(&di->i2c_lock);
	return rc;
}

static int idtp9418_read_buffer(struct idtp9418_device_info *di, uint16_t reg, uint8_t *buf,
								uint32_t size)
{
	int rc = 0;

	while (size--)
	{
		rc = di->bus.read(di, reg++, buf++);
		if (rc < 0)
		{
			dev_err(di->dev, "[idt] read buf error: %d\n", rc);
			return rc;
		}
	}

	return rc;
}

static int idtp9418_write_buffer(struct idtp9418_device_info *di, uint16_t reg, uint8_t *buf,
								 uint32_t size)
{
	int rc = 0;

	while (size--)
	{
		rc = di->bus.write(di, reg++, *buf++);
		if (rc < 0)
		{
			dev_err(di->dev, "[idt] write error: %d\n", rc);
			return rc;
		}
	}

	return rc;
}

/*
	Turn on/off the power to the chip
	This turn on "reverse-enable" gpio which contorls the
	mosfet to power the TPS22916 boost converter
	By default, vout of the TPS22916 is the same as the input voltage
*/
static void idtp9418_set_chip_power(struct idtp9418_device_info *di,
									int enable)
{
	gpiod_set_value(di->gpios.enable, !!enable);
}

static void idt_set_reverse_fod(struct idtp9418_device_info *di, int mw)
{
	uint8_t mw_l, mw_h;
	if (!di)
		return;
	mw_l = mw & 0xff;
	mw_h = mw >> 8;
	di->bus.write(di, REG_FOD_LOW, mw_l);
	di->bus.write(di, REG_FOD_HIGH, mw_h);
	dev_info(di->dev, "set reverse fod: %d\n", mw);
}

/*
	Use the reverse-boost-enable-gpio gpio to turn on/off the voltage boosting.
	This needs to be turned on after the chip is powered.
*/
static void idtp9418_enable_boost_converter(struct idtp9418_device_info *di, int enable)
{
	gpiod_set_value(di->gpios.boost_enable, !!enable);
}

static ssize_t chip_version_show(struct device *dev,
								 struct device_attribute *attr, char *buf)
{
	uint8_t chip_id_l, chip_id_h, chip_rev, cust_id, status, vset;
	uint8_t fw_otp_ver[4], fw_app_ver[4];
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct idtp9418_device_info *di = i2c_get_clientdata(client);
	msleep(100);
	di->bus.read(di, REG_STATUS_L, &status);
	di->bus.read(di, REG_VOUT_SET, &vset);

	di->bus.read(di, REG_CHIP_ID_L, &chip_id_l);
	di->bus.read(di, REG_CHIP_ID_H, &chip_id_h);
	di->bus.read(di, REG_CHIP_REV, &chip_rev);
	chip_rev = chip_rev >> 4;
	di->bus.read(di, REG_CTM_ID, &cust_id);
	di->bus.read_buf(di, REG_OTPFWVER_ADDR, fw_otp_ver, 4);
	di->bus.read_buf(di, REG_EPRFWVER_ADDR, fw_app_ver, 4);

	return snprintf(
		buf, PAGE_SIZE,
		"chip_id_l:%02x\nchip_id_h:%02x\nchip_rev:%02x\ncust_id:%02x status:%02x vset:%02x\n otp_ver:%x.%x.%x.%x\n app_ver:%x.%x.%x.%x\n",
		chip_id_l, chip_id_h, chip_rev, cust_id, status, vset,
		fw_otp_ver[0], fw_otp_ver[1], fw_otp_ver[2],
		fw_otp_ver[3], fw_app_ver[0], fw_app_ver[1],
		fw_app_ver[2], fw_app_ver[3]);
}

static int idt_get_reverse_vout(struct idtp9418_device_info *di)
{
	u8 vout_l, vout_h;
	int vout = -1;

	if (!di)
		return vout;

	di->bus.read(di, REG_REVERSE_VIN_LOW, &vout_l);
	di->bus.read(di, REG_REVERSE_VIN_HIGH, &vout_h);
	vout = vout_l | (vout_h << 8);
	dev_info(di->dev, "[reverse] vout is %d\n", vout);
	return vout;
}

static int idt_get_reverse_soc(struct idtp9418_device_info *di)
{
	u8 soc = -1;

	if (!di)
		return soc;

	di->bus.read(di, REG_CHG_STATUS, &soc);
	if ((soc < 0) || (soc > 0x64))
	{
		if (soc == 0xFF)
		{
			dev_info(di->dev, "[reverse] soc is default 0xFF\n");
			return -1;
		}
		else
		{
			dev_info(di->dev, "[reverse] soc illegal: %d\n", soc);
			return -1;
		}
	}
	return soc;
}

static int idt_get_reverse_iin(struct idtp9418_device_info *di)
{
	u8 iin_l, iin_h;
	int iin = -1;

	if (!di)
		return iin;

	di->bus.read(di, REG_REVERSE_IIN_LOW, &iin_l);
	di->bus.read(di, REG_REVERSE_IIN_HIGH, &iin_h);
	iin = iin_l | (iin_h << 8);
	dev_info(di->dev, "[reverse] iin is %d\n", iin);
	return iin;
}

static int idt_get_reverse_temp(struct idtp9418_device_info *di)
{
	u8 temp = -1;

	if (!di)
		return temp;

	di->bus.read(di, REG_REVERSE_TEMP, &temp);
	dev_info(di->dev, "[reverse] temp is %d\n", temp);
	return temp;
}

static ssize_t chip_status_show(struct device *dev, struct device_attribute *attr,
								char *buf)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct idtp9418_device_info *di = i2c_get_clientdata(client);
	int vout, iin, temp, soc;

	vout = idt_get_reverse_vout(di);
	iin = idt_get_reverse_iin(di);
	temp = idt_get_reverse_temp(di);
	soc = idt_get_reverse_soc(di);

	return snprintf(buf, PAGE_SIZE, "vout:%d\niin:%d\ntemp:%d\nsoc:%d\n",
					vout, iin, temp, soc);
}

static ssize_t chip_powered_show(struct device *dev,
								 struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct idtp9418_device_info *di = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "%d\n", di->chip_enable);
}

static ssize_t chip_power_store(struct device *dev,
								struct device_attribute *attr, const char *buf, size_t count)
{
	bool enable;
	int ret;
	struct i2c_client *client = container_of(dev, struct i2c_client, dev);
	struct idtp9418_device_info *di = i2c_get_clientdata(client);

	ret = kstrtobool(buf, &enable);
	if (ret < 0)
		return ret;

	idtp9418_set_chip_power(di, enable);

	return count;
}

static DEVICE_ATTR(chip_version, S_IRUGO, chip_version_show, NULL);
static DEVICE_ATTR(chip_powered, S_IWUSR | S_IRUGO, chip_powered_show, chip_power_store);
static DEVICE_ATTR(chip_status, S_IRUGO, chip_status_show, NULL);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_chip_version.attr,
	&dev_attr_chip_powered.attr,
	&dev_attr_chip_status.attr,
	NULL,
};

static const struct attribute_group sysfs_group_attrs = {
	.attrs = sysfs_attrs,
};

static irqreturn_t idtp9418_irq_handler(int irq, void *dev_id)
{
	struct idtp9418_device_info *di = dev_id;
	pm_stay_awake(di->dev);
	schedule_delayed_work(&di->irq_work, msecs_to_jiffies(10));
	return IRQ_HANDLED;
}

static irqreturn_t idtp9418_hall_irq_handler(int irq, void *dev_id)
{
	struct idtp9418_device_info *di = dev_id;
	pm_stay_awake(di->dev);
	schedule_delayed_work(&di->hall_irq_work, msecs_to_jiffies(100));

	return IRQ_HANDLED;
}

static int idtp9418_gpio_init(struct idtp9418_device_info *di)
{
	struct
	{
		const char *name;
		struct gpio_desc **gpio;
		enum gpiod_flags flags;
	} gpio_map[] = {
		{"irq", &di->gpios.idt_irq, GPIOD_IN},
		{"enable", &di->gpios.enable, GPIOD_OUT_HIGH},
		{"boost-enable", &di->gpios.boost_enable, GPIOD_OUT_HIGH},
		{"pen-det-active-high", &di->gpios.pen_det_active_high, GPIOD_IN},
		{"pen-det-active-low", &di->gpios.pen_det_active_low, GPIOD_IN}};

	for (int i = 0; i < ARRAY_SIZE(gpio_map); i++)
	{
		*gpio_map[i].gpio = devm_gpiod_get(di->dev, gpio_map[i].name, gpio_map[i].flags);
		if (IS_ERR(*gpio_map[i].gpio))
		{
			dev_err(di->dev, "Unable to get GPIO: %s\n", gpio_map[i].name);
			return PTR_ERR(*gpio_map[i].gpio);
		}
	}

	struct
	{
		const char *name;
		struct gpio_desc *gpio;
		irq_handler_t handler;
		unsigned long irqflags;
	} irq_map[] = {
		{
			"idt-irq",
			di->gpios.idt_irq,
			idtp9418_irq_handler,
			IRQF_TRIGGER_FALLING,
		},
		{
			"hall-pen-l-irq",
			di->gpios.pen_det_active_low,
			idtp9418_hall_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
		},
		{
			"hall-pen-h-irq",
			di->gpios.pen_det_active_high,
			idtp9418_hall_irq_handler,
			IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
		}};
	for (int i = 0; i < ARRAY_SIZE(irq_map); i++)
	{
		int ret = devm_request_irq(di->dev, gpiod_to_irq(irq_map[i].gpio),
								   irq_map[i].handler,
								   irq_map[i].irqflags,
								   irq_map[i].name, di);
		if (ret)
		{
			dev_err(di->dev, "failed to request gpio \"%s\" IRQ\n", irq_map[i].name);
		}
	}

	return 0;
}

static bool reverse_need_irq_cleared(struct idtp9418_device_info *di, uint32_t val)
{
	uint8_t int_buf[4];
	uint32_t int_val;
	int rc = -1;

	rc = di->bus.read_buf(di, REG_SYS_INT, int_buf, 4);
	if (rc < 0)
	{
		dev_err(di->dev, "%s: read int state error\n", __func__);
		return false;
	}
	int_val = int_buf[0] | (int_buf[1] << 8) | (int_buf[2] << 16) |
			  (int_buf[3] << 24);
	if (int_val && (int_val == val))
	{
		dev_info(di->dev, "irq clear wrong, retry: 0x%08x\n", int_val);
		return true;
	}

	return false;
}

static void idtp9418_reverse_charge_enable(struct idtp9418_device_info *di)
{
	u8 mode = 0;
	int i = 0;
	int fod_set = REVERSE_FOD;

	/* set reverse fod to REVERSE_FOD */
	idt_set_reverse_fod(di, fod_set);
	for (i = 0; i < 3; i++)
	{
		di->bus.write(di, REG_TX_CMD, TX_EN | TX_FOD_EN);
		msleep(50);
		di->bus.read(di, REG_TX_DATA, &mode);
		dev_info(di->dev, "tx data(0078): 0x%x\n", mode);
		if (mode & BIT(0))
		{
			dev_info(di->dev, "start reverse charging\n");
			break;
		}
		else
		{
			dev_err(di->dev, "idt set reverse charge failed, retry: %d\n", i);
		}
	}

	if (i < 3)
	{
		dev_info(di->dev, "reverse charging start success\n");
		di->is_charging = true;
		// Start timer
		schedule_delayed_work(&di->charge_monitor_work,
							  msecs_to_jiffies(CHARGE_MONITOR_INTERVAL));
	}
	else
	{
		// Clean up
		dev_info(di->dev, "reverse charging failed start\n");
		idtp9418_enable_boost_converter(di, false);
		idtp9418_set_chip_power(di, false);
		di->is_charging = false;
		cancel_delayed_work_sync(&di->charge_monitor_work);
	}
}

static void idtp9418_charge_monitor_work(struct work_struct *work)
{
	struct idtp9418_device_info *di =
		container_of(work, struct idtp9418_device_info, charge_monitor_work.work);

	int soc;
	bool attached;

	soc = idt_get_reverse_soc(di);
	attached = (!gpiod_get_value(di->gpios.pen_det_active_low) &&
				gpiod_get_value(di->gpios.pen_det_active_high));

	dev_info(di->dev, "[charge-monitor] pen=%d soc=%d\n", attached, soc);
	power_supply_changed(di->psy);

	if (!attached || soc < 0 || soc >= di->charge_limit)
	{
		dev_info(di->dev, "[charge-monitor] stopping reverse charging. attached=%d soc=%d\n",
				 attached, soc);

		// Turn off charging and power
		idtp9418_enable_boost_converter(di, false);
		idtp9418_set_chip_power(di, false);
		di->is_charging = false;

		return;
	}

	schedule_delayed_work(&di->charge_monitor_work,
						  msecs_to_jiffies(CHARGE_MONITOR_INTERVAL));
}

static void idtp9418_irq_hall_work(struct work_struct *work)
{
	struct idtp9418_device_info *di = container_of(work, struct idtp9418_device_info, hall_irq_work.work);
	bool attached = false;

	if (!gpiod_get_value(di->gpios.pen_det_active_low) && gpiod_get_value(di->gpios.pen_det_active_high))
	{
		dev_info(di->dev, "[idtp] Pen attached! Turning on reverse charging... \n");
		attached = true;
	}
	else
	{
		dev_info(di->dev, "[idtp] Pen detached! Turning off reverse charging... \n");
		di->is_charging = false;
		cancel_delayed_work_sync(&di->charge_monitor_work);
	}

	idtp9418_set_chip_power(di, false);
	idtp9418_enable_boost_converter(di, false);
	msleep(100);
	if (attached)
	{
		idtp9418_enable_boost_converter(di, true);
		msleep(600);
		idtp9418_set_chip_power(di, true);
		msleep(3000);
		idtp9418_reverse_charge_enable(di);
	}
	pm_relax(di->dev);
	power_supply_changed(di->psy);
}

static void idtp9418_irq_work(struct work_struct *work)
{
	struct idtp9418_device_info *di = container_of(work, struct idtp9418_device_info, irq_work.work);

	dev_err(di->dev, "%s: Entered irq work\n", __func__);

	u8 int_buf[4] = {0};
	u32 int_val = 0;
	int rc = 0;

	if (gpiod_get_value(di->gpios.idt_irq))
	{
		pm_relax(di->dev);
		return;
	}

	rc = di->bus.read_buf(di, REG_SYS_INT, int_buf, 4);
	if (rc < 0)
	{
		dev_err(di->dev, "[idt]read int state error: %d\n", rc);
	}
	int_val =
		int_buf[0] | (int_buf[1] << 8) | (int_buf[2] << 16) | (int_buf[3] << 24);

	dev_info(di->dev, "Irq got: %d\n", int_val);

	msleep(5);
	if (reverse_need_irq_cleared(di, int_val))
	{
		reverse_clrInt(di);
		msleep(5);
	}
	pm_relax(di->dev);
}

static struct regmap_config i2c_idtp9418_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xFFFF,
};

static const struct i2c_device_id idtp9418_id[] = {
	{IDT_DRIVER_NAME, 0},
	{},
};

static const struct of_device_id idt_match_table[] = {{.compatible =
														   "idt,p9418"},
													  {}};

MODULE_DEVICE_TABLE(i2c, idtp9418_id);

void reverse_clrInt(struct idtp9418_device_info *di)
{
	uint8_t clr_buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
	di->bus.write_buf(di, REG_SYS_INT_CLR, clr_buf, sizeof(clr_buf));
	di->bus.write(di, REG_TX_CMD, TX_FOD_EN | TX_CLRINT);

	dev_err(di->dev, "Interrupt cleared!\n");
}

static int idtp9418_get_property(struct power_supply *psy,
								 enum power_supply_property psp,
								 union power_supply_propval *val)
{
	struct idtp9418_device_info *di = power_supply_get_drvdata(psy);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_CAPACITY:
		if (!di->is_charging)
			return 0;
		val->intval = idt_get_reverse_soc(di);
		if (val->intval < 0)
			val->intval = 0;
		return 0;

	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = di->charge_limit;
		return 0;

	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = di->is_charging ? 1 : 0;
		return 0;

	default:
		return -EINVAL;
	}
}

static int idtp9418_property_is_writeable(struct power_supply *psy,
										  enum power_supply_property psp)
{
	switch (psp)
	{
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return 1;
	default:
		return 0;
	}
}

static int idtp9418_set_property(struct power_supply *psy,
								 enum power_supply_property psp,
								 const union power_supply_propval *val)
{
	struct idtp9418_device_info *di = power_supply_get_drvdata(psy);

	switch (psp)
	{
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		if (val->intval < 0 || val->intval > 95)
			return -EINVAL;

		di->charge_limit = val->intval;
		dev_info(di->dev, "Charge limit set to %d%%\n", di->charge_limit);
		return 0;

	default:
		return -EINVAL;
	}
}

static int idtp9418_probe(struct i2c_client *client)
{
	int ret;
	struct idtp9418_device_info *di;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct power_supply_config idtp_cfg;
	struct power_supply_desc *psy_desc;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
	{
		dev_err(&client->dev, "i2c check functionality failed!\n");
		return -EIO;
	}

	di = devm_kzalloc(&client->dev, sizeof(*di), GFP_KERNEL);
	if (!di)
	{
		dev_err(&client->dev,
				"i2c allocated device info data failed!\n");
		return -ENOMEM;
	}

	di->name = IDT_DRIVER_NAME;
	di->dev = &client->dev;
	di->chip_enable = 0;

	di->is_charging = false;
	di->charge_limit = LIMIT_SOC;

	di->regmap = devm_regmap_init_i2c(client, &i2c_idtp9418_regmap_config);
	if (!di->regmap)
		return -ENODEV;
	di->bus.read = idtp9418_read;
	di->bus.write = idtp9418_write;
	di->bus.read_buf = idtp9418_read_buffer;
	di->bus.write_buf = idtp9418_write_buffer;

	mutex_init(&di->i2c_lock);
	device_init_wakeup(&client->dev, true);
	i2c_set_clientdata(client, di);

	ret = idtp9418_gpio_init(di);
	if (ret < 0)
	{
		dev_err(di->dev, "%s: gpio init error [%d]\n", __func__, ret);
		return ret;
	}

	if (sysfs_create_group(&client->dev.kobj, &sysfs_group_attrs))
	{
		dev_err(&client->dev, "create sysfs attrs failed!\n");
		return -EIO;
	}

	idtp_cfg.drv_data = di;

	INIT_DELAYED_WORK(&di->irq_work, idtp9418_irq_work);
	INIT_DELAYED_WORK(&di->hall_irq_work, idtp9418_irq_hall_work);
	INIT_DELAYED_WORK(&di->charge_monitor_work, idtp9418_charge_monitor_work);

	idtp9418_set_chip_power(di, false);

	// Initialize psy

	psy_desc = devm_kzalloc(&client->dev, sizeof(*psy_desc), GFP_KERNEL);
	if (!psy_desc)
		return -ENOMEM;

	psy_desc->name = "idtp9418";
	psy_desc->type = POWER_SUPPLY_TYPE_WIRELESS;
	psy_desc->properties = idtp9418_props;
	psy_desc->num_properties = ARRAY_SIZE(idtp9418_props);
	psy_desc->get_property = idtp9418_get_property;
	psy_desc->set_property = idtp9418_set_property;
	psy_desc->property_is_writeable = idtp9418_property_is_writeable;

	idtp_cfg.drv_data = di;

	di->psy = devm_power_supply_register(&client->dev, psy_desc, &idtp_cfg);
	if (IS_ERR(di->psy))
	{
		dev_err(&client->dev, "Failed to register power supply\n");
		return PTR_ERR(di->psy);
	}

	return 0;
}

static void idtp9418_remove(struct i2c_client *client)
{
	sysfs_remove_group(&client->dev.kobj, &sysfs_group_attrs);

	struct idtp9418_device_info *di = i2c_get_clientdata(client);
	i2c_set_clientdata(client, NULL);

	cancel_delayed_work_sync(&di->irq_work);
	cancel_delayed_work_sync(&di->hall_irq_work);
	cancel_delayed_work_sync(&di->charge_monitor_work);
}

static void idtp9418_shutdown(struct i2c_client *client)
{
}

static struct i2c_driver idtp9418_driver = {
	.driver = {
		.name = IDT_DRIVER_NAME,
		.owner = THIS_MODULE,
		.of_match_table = idt_match_table,
	},
	.probe = idtp9418_probe,
	.remove = idtp9418_remove,
	.shutdown = idtp9418_shutdown,
	.id_table = idtp9418_id,
};

static int __init idt_init(void)
{
	return i2c_add_driver(&idtp9418_driver);
}

static void __exit idt_exit(void)
{
	i2c_del_driver(&idtp9418_driver);
}

module_init(idt_init);
module_exit(idt_exit);

MODULE_AUTHOR("Viola Guerrera <viooo@anche.no>");
MODULE_DESCRIPTION("idtp9418 Wireless Power Charger Monitor driver");
MODULE_LICENSE("GPL v2");
