/*
 * Copyright (C) CNflysky. All rights reserved.
 * Updated by adrianchen91.
 * Fuel Gauge driver for MAX17048 chip found on HackberryPi CM5.
 *
 * Reworked to align with Acer Switch Battery Module standards.
 */

#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MAX17048_VCELL_REG 0x02
#define MAX17048_SOC_REG 0x04
#define MAX17048_CONFIG_REG 0x0C
#define MAX17048_VALRT_REG 0x14
#define MAX17048_CRATE_REG 0x16
#define MAX17048_STATUS_REG 0x1A

/* Constants for conversions and thresholds */
#define MAX17048_VCELL_LSB_NUM 625
#define MAX17048_VCELL_LSB_DEN 8
#define MAX17048_SOC_LSB_INV 256
#define MAX17048_CRATE_LSB_NUM 52
#define MAX17048_CRATE_LSB_DEN 25000
#define MAX17048_CRATE_NOISE_THR 4
#define MAX17048_FULL_SOC_THR 95
#define MAX17048_TTE_CONST_NUM 225000
#define MAX17048_TTE_CONST_DEN 13
#define MAX17048_TTE_RATE_THR 10
#define MAX17048_CAP_FULL_THR 99
#define MAX17048_CAP_CRIT_THR 5
#define MAX17048_CAP_LOW_THR 15
#define MAX17048_DEFAULT_CAP_UAH 5000000
#define MAX17048_MAX_CAP_UAH 10000000
#define MAX17048_MAX_ENERGY_UWH 18500000
#define MAX17048_TTE_TUNING_FACTOR 8

/**
 * The configuration of the regmap for MAX17048.
 * 8-bit registers, 16-bit values, Big Endian.
 */
static const struct regmap_config max17048_regmap_cfg = {
    .reg_bits = 8,
    .val_bits = 16,
    .val_format_endian = REGMAP_ENDIAN_BIG,
    .max_register = 0xFF,
    .disable_locking = false,
    .cache_type = REGCACHE_NONE,
};

/**
 * struct max17048 - Driver data for MAX17048 fuel gauge
 * @client:                 I2C client pointer
 * @regmap:                 Regmap for device access
 * @battery:                Battery power supply device
 * @ac_adapter:             AC adapter power supply device
 * @monitor_thread:         Thread for polling AC status
 * @charge_full_design_uah: Design capacity in uAh
 * @energy_full_design_uwh: Design energy in uWh
 * @ac_online:              Cached AC online status
 */
struct max17048 {
  struct i2c_client *client;
  struct regmap *regmap;
  struct power_supply *battery;
  u32 charge_full_design_uah;
  u32 energy_full_design_uwh;
  struct delayed_work work;
  struct power_supply *ac_adapter;
  unsigned int delay;
};

/**
 * max17048_read_reg - Read a 16-bit register
 * @battery: Driver data
 * @reg:     Register address
 *
 * Returns value on success, negative error code on failure.
 */
static int max17048_read_reg(struct max17048 *battery, u8 reg, u32 *val) {
  return regmap_read(battery->regmap, reg, val);
}

/**
 * max17048_get_vcell - Get battery voltage in microvolts
 * @battery: Driver data
 *
 * Returns voltage (uV) or error code.
 */
static int max17048_get_vcell(struct max17048 *battery) {
  u32 vcell = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_VCELL_REG, &vcell);
  if (ret)
    return ret;

  /* 78.125uV per LSB -> vcell * 78.125 = vcell * 625 / 8 */
  return (vcell * MAX17048_VCELL_LSB_NUM / MAX17048_VCELL_LSB_DEN);
}

/**
 * max17048_get_soc - Get State of Charge in percent (0-100)
 * @battery: Driver data
 *
 * Returns SOC (%) or error code.
 */
static int max17048_get_soc(struct max17048 *battery) {
  u32 soc = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_SOC_REG, &soc);
  if (ret)
    return ret;

  soc /= MAX17048_SOC_LSB_INV;
  if (soc > 100)
    soc = 100;

  return soc;
}

/**
 * max17048_get_crate - Get C-Rate raw value
 * @battery: Driver data
 * @crate:   Pointer to store sign-extended 16-bit C-Rate
 *
 * Returns 0 on success, error code on failure.
 */
static int max17048_get_crate(struct max17048 *battery, int16_t *crate) {
  u32 crate_raw = 0;
  int ret;

  ret = max17048_read_reg(battery, MAX17048_CRATE_REG, &crate_raw);
  if (ret)
    return ret;

  *crate = (int16_t)crate_raw;
  return 0;
}

/**
 * max17048_get_current - Get battery current in microamps
 * @battery: Driver data
 * @val:     Pointer to store current (uA)
 *
 * Positive = Charging, Negative = Discharging.
 */
static int max17048_get_current(struct max17048 *battery, int *val) {
  int16_t crate;
  int ret;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  /*
   * C-Rate LSB is 0.208%/hr.
   * Current = Capacity * C-Rate
   * Current (uA) = charge_design_uah * crate * 0.208 / 100
   *              = charge_design_uah * crate * 52 / 25000
   */
  *val = (int)div_s64((s64)battery->charge_full_design_uah * crate *
                          MAX17048_CRATE_LSB_NUM,
                      MAX17048_CRATE_LSB_DEN);
  return 0;
}

/**
 * max17048_get_status - Get battery charging status
 * @battery: Driver data
 */
static int max17048_get_status(struct max17048 *battery) {
  int16_t crate;
  int ret, soc;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return POWER_SUPPLY_STATUS_UNKNOWN;

  /* Threshold of 4 LSB (~0.8%/hr) for noise immunity */
  if (crate > MAX17048_CRATE_NOISE_THR)
    return POWER_SUPPLY_STATUS_CHARGING;
  if (crate < -MAX17048_CRATE_NOISE_THR)
    return POWER_SUPPLY_STATUS_DISCHARGING;

  soc = max17048_get_soc(battery);

  /* High SOC and low current -> Full */
  /* High SOC and low current -> Full */
  if (soc >= MAX17048_FULL_SOC_THR)
    return POWER_SUPPLY_STATUS_FULL;

  return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

/**
 * max17048_get_time_to_empty - Estimate time to empty
 * @battery: Driver data
 * @val:     Pointer to store TTE (seconds)
 */
static int max17048_get_time_to_empty(struct max17048 *battery, int *val) {
  int16_t crate;
  int ret, soc;
  int32_t discharge_rate;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  if (crate >= -MAX17048_TTE_RATE_THR)
    return -ENODATA;

  soc = max17048_get_soc(battery);
  if (soc < 0)
    return soc;

  discharge_rate = abs(crate);
  /* TTE (s) = 225000 * soc / (discharge_rate * 13) */
  /* Adjusted by tuning factor to match observed discharge profile */
  *val = (int)div_s64((s64)MAX17048_TTE_CONST_NUM * soc *
                          MAX17048_TTE_TUNING_FACTOR,
                      (s64)discharge_rate * MAX17048_TTE_CONST_DEN);
  return 0;
}

/**
 * max17048_get_time_to_full - Estimate time to full
 * @battery: Driver data
 * @val:     Pointer to store TTF (seconds)
 */
static int max17048_get_time_to_full(struct max17048 *battery, int *val) {
  int16_t crate;
  int ret, soc;

  ret = max17048_get_crate(battery, &crate);
  if (ret)
    return ret;

  if (crate <= MAX17048_TTE_RATE_THR)
    return -ENODATA;

  soc = max17048_get_soc(battery);
  if (soc < 0)
    return soc;

  *val = (int)div_s64((s64)MAX17048_TTE_CONST_NUM * (100 - soc),
                      (s64)crate * MAX17048_TTE_CONST_DEN);
  return 0;
}

/**
 * max17048_get_capacity_level - Get capacity level description
 * @battery: Driver data
 */
static int max17048_get_capacity_level(struct max17048 *battery) {
  int soc = max17048_get_soc(battery);
  int status = max17048_get_status(battery);

  if (soc < 0)
    return POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

  if (status == POWER_SUPPLY_STATUS_FULL || soc >= MAX17048_CAP_FULL_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_FULL;
  else if (soc <= MAX17048_CAP_CRIT_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
  else if (soc <= MAX17048_CAP_LOW_THR)
    return POWER_SUPPLY_CAPACITY_LEVEL_LOW;

  return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

/**
 * battery_get_property - Power Supply API get_property callback
 */
static int battery_get_property(struct power_supply *psy,
                                enum power_supply_property psp,
                                union power_supply_propval *val) {
  struct max17048 *battery = power_supply_get_drvdata(psy);
  int ret;

  switch (psp) {
  case POWER_SUPPLY_PROP_STATUS:
    val->intval = max17048_get_status(battery);
    break;
  case POWER_SUPPLY_PROP_VOLTAGE_NOW:
    ret = max17048_get_vcell(battery);
    if (ret < 0)
      return ret;
    val->intval = ret;
    break;
  case POWER_SUPPLY_PROP_CAPACITY:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    val->intval = ret;
    break;
  case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
    val->intval = max17048_get_capacity_level(battery);
    break;
  case POWER_SUPPLY_PROP_CHARGE_NOW:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    val->intval = (int)div_s64((s64)ret * battery->charge_full_design_uah, 100);
    break;
  case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
    val->intval = (int)battery->charge_full_design_uah;
    break;
  case POWER_SUPPLY_PROP_ENERGY_NOW:
    ret = max17048_get_soc(battery);
    if (ret < 0)
      return ret;
    /* Estimate Energy Now based on SOC and Energy Full */
    val->intval = (int)div_s64((s64)ret * battery->energy_full_design_uwh, 100);
    break;
  case POWER_SUPPLY_PROP_ENERGY_FULL:
  case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
    val->intval = (int)battery->energy_full_design_uwh;
    break;
  case POWER_SUPPLY_PROP_TECHNOLOGY:
    val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
    break;
  case POWER_SUPPLY_PROP_CURRENT_NOW:
    ret = max17048_get_current(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
    ret = max17048_get_time_to_empty(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
    ret = max17048_get_time_to_full(battery, &val->intval);
    if (ret < 0)
      return ret;
    break;
  case POWER_SUPPLY_PROP_MODEL_NAME:
    val->strval = "MAX17048";
    break;
  case POWER_SUPPLY_PROP_MANUFACTURER:
    val->strval = "Maxim Integrated";
    break;
  case POWER_SUPPLY_PROP_PRESENT:
    val->intval = 1;
    break;
  default:
    return -EINVAL;
  }
  return 0;
}

static enum power_supply_property max17048_battery_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_VOLTAGE_NOW,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_CAPACITY_LEVEL,
    POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
    POWER_SUPPLY_PROP_CHARGE_NOW,
    POWER_SUPPLY_PROP_ENERGY_NOW,
    POWER_SUPPLY_PROP_ENERGY_FULL,
    POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
    POWER_SUPPLY_PROP_TECHNOLOGY,
    POWER_SUPPLY_PROP_CURRENT_NOW,
    POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
    POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
    POWER_SUPPLY_PROP_MODEL_NAME,
    POWER_SUPPLY_PROP_MANUFACTURER,
    POWER_SUPPLY_PROP_PRESENT,
};

static const struct power_supply_desc max17048_battery_desc = {
    .name = "battery",
    .type = POWER_SUPPLY_TYPE_BATTERY,
    .get_property = battery_get_property,
    .properties = max17048_battery_props,
    .num_properties = ARRAY_SIZE(max17048_battery_props),
};

static int max17048_ac_get_property(struct power_supply *psy,
                                    enum power_supply_property psp,
                                    union power_supply_propval *val) {
  struct max17048 *drv = power_supply_get_drvdata(psy);
  int status;

  switch (psp) {
  case POWER_SUPPLY_PROP_ONLINE:
    status = max17048_get_status(drv);
    val->intval = (status == POWER_SUPPLY_STATUS_CHARGING ||
                   status == POWER_SUPPLY_STATUS_FULL);
    break;
  default:
    return -EINVAL;
  }
  return 0;
}

static enum power_supply_property max17048_ac_props[] = {
    POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc max17048_ac_desc = {
    .name = "max17048-mains",
    .type = POWER_SUPPLY_TYPE_MAINS,
    .get_property = max17048_ac_get_property,
    .properties = max17048_ac_props,
    .num_properties = ARRAY_SIZE(max17048_ac_props),
};

static void max17048_work(struct work_struct *work) {
  struct max17048 *drv = container_of(work, struct max17048, work.work);
  power_supply_changed(drv->battery);
  power_supply_changed(drv->ac_adapter);
  schedule_delayed_work(&drv->work, drv->delay);
}

static irqreturn_t max17048_irq_handler(int irq, void *dev_id) {
  struct max17048 *drv = dev_id;
  int ret;
  unsigned int status;

  /* Read Status to clear ALRT pin */
  ret = regmap_read(drv->regmap, MAX17048_STATUS_REG, &status);

  power_supply_changed(drv->battery);
  power_supply_changed(drv->ac_adapter);
  return IRQ_HANDLED;
}

static int max17048_probe(struct i2c_client *client) {
  struct device *dev = &client->dev;
  struct max17048 *drv;
  struct power_supply_config psycfg = {};
  int ret;

  if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE))
    return -EIO;

  drv = devm_kzalloc(dev, sizeof(struct max17048), GFP_KERNEL);
  if (!drv)
    return -ENOMEM;

  drv->client = client;
  drv->regmap = devm_regmap_init_i2c(client, &max17048_regmap_cfg);
  if (IS_ERR(drv->regmap))
    return PTR_ERR(drv->regmap);

  /* Read properties */
  ret = device_property_read_u32(dev, "charge-full-design-microamp-hours",
                                 &drv->charge_full_design_uah);

  if (drv->charge_full_design_uah > MAX17048_MAX_CAP_UAH)
    drv->charge_full_design_uah = MAX17048_MAX_CAP_UAH;

  if (ret) {
    /* Fallback to legacy battery-capacity (mAh) */
    u32 cap_mah = 0;
    if (device_property_read_u32(dev, "battery-capacity", &cap_mah) == 0) {
      if (cap_mah > 0 && cap_mah < 20000)
        drv->charge_full_design_uah = cap_mah * 1000;
    }
  }

  if (drv->charge_full_design_uah == 0) {
    dev_warn(dev, "Capacity not configured, default 5000mAh\n");
    drv->charge_full_design_uah = MAX17048_DEFAULT_CAP_UAH;
  }

  ret = device_property_read_u32(dev, "energy-full-design-microwatt-hours",
                                 &drv->energy_full_design_uwh);
  if (ret || drv->energy_full_design_uwh == 0) {
    drv->energy_full_design_uwh =
        (u32)div_u64((u64)drv->charge_full_design_uah * 37, 10);
  }

  if (drv->energy_full_design_uwh > MAX17048_MAX_ENERGY_UWH)
    drv->energy_full_design_uwh = MAX17048_MAX_ENERGY_UWH;

  dev_info(dev, "MAX17048: Design: %u uAh, %u uWh\n",
           drv->charge_full_design_uah, drv->energy_full_design_uwh);

  /* Register Battery */
  psycfg.drv_data = drv;
  psycfg.of_node = dev->of_node;

  drv->battery =
      devm_power_supply_register(dev, &max17048_battery_desc, &psycfg);
  if (IS_ERR(drv->battery)) {
    dev_err(dev, "Failed to register battery\n");
    return PTR_ERR(drv->battery);
  }

  /* Register AC Adapter */
  drv->ac_adapter = devm_power_supply_register(dev, &max17048_ac_desc, &psycfg);
  if (IS_ERR(drv->ac_adapter)) {
    dev_err(dev, "Failed to register AC adapter\n");
    return PTR_ERR(drv->ac_adapter);
  }

  i2c_set_clientdata(client, drv);

  INIT_DELAYED_WORK(&drv->work, max17048_work);

  if (client->irq) {
    ret = devm_request_threaded_irq(
        dev, client->irq, NULL, max17048_irq_handler,
        IRQF_TRIGGER_LOW | IRQF_ONESHOT, "max17048-battery", drv);
    if (ret) {
      dev_err(dev, "Failed to request IRQ %d: %d\n", client->irq, ret);
      return ret;
    }
    /* Heartbeat poll every 5 minutes if IRQ is present */
    drv->delay = msecs_to_jiffies(300000);
  } else {
    /* Poll every 30 seconds if no IRQ */
    drv->delay = msecs_to_jiffies(30000);
  }

  schedule_delayed_work(&drv->work, drv->delay);

  return 0;
}

static void max17048_remove(struct i2c_client *client) {
  struct max17048 *drv = i2c_get_clientdata(client);
  cancel_delayed_work_sync(&drv->work);
}

static struct of_device_id max17048_of_ids[] = {
    {.compatible = "hackberrypi,max17048-battery"}, {}};
MODULE_DEVICE_TABLE(of, max17048_of_ids);

static struct i2c_driver max17048_driver = {
    .driver = {.name = "max17048", .of_match_table = max17048_of_ids},
    .probe = max17048_probe,
    .remove = max17048_remove,
};

module_i2c_driver(max17048_driver);

MODULE_DESCRIPTION("MAX17048 fuel gauge driver for HackBerryPi CM5");
MODULE_AUTHOR("CNflysky <cnflysky@qq.com>");
MODULE_LICENSE("GPL");
