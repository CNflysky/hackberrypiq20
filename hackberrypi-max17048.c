/*
        Copyright (C) CNflysky. All rights reserved.
        Fuel Gauge driver for MAX17048 chip found on HackberryPi CM5.
*/

#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/property.h>
#include <linux/math64.h>

#define MAX17048_VCELL_REG 0x02
#define MAX17048_SOC_REG 0x04
#define MAX17048_CRATE_REG 0x16

static const struct regmap_config max17048_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xFF,
	.disable_locking = false,
	.cache_type = REGCACHE_NONE,
};

struct max17048 {
	struct i2c_client *client;
	struct regmap *regmap;
	struct power_supply *battery;
	u32 charge_full_design_uah;
	u32 energy_full_design_uwh;
};

static int max17048_get_vcell(struct max17048 *battery)
{
	uint32_t vcell = 0;
	int ret;

	ret = regmap_read(battery->regmap, MAX17048_VCELL_REG, &vcell);
	if (ret)
		return ret;

	/* 78.125uV per LSB -> vcell * 78.125 = vcell * 625 / 8 */
	return (vcell * 625 / 8);
}

static int max17048_get_soc(struct max17048 *battery)
{
	uint32_t soc = 0;
	int ret;

	ret = regmap_read(battery->regmap, MAX17048_SOC_REG, &soc);
	if (ret)
		return ret;

	soc /= 256;
	if (soc < 0)
		soc = 0;
	if (soc > 100)
		soc = 100;
	
	return soc;
}

static int max17048_get_crate(struct max17048 *battery, int16_t *crate)
{
	uint32_t crate_raw = 0;
	int ret;

	ret = regmap_read(battery->regmap, MAX17048_CRATE_REG, &crate_raw);
	if (ret)
		return ret;

	*crate = (int16_t)crate_raw;
	return 0;
}

static int max17048_get_status(struct max17048 *battery)
{
	int16_t crate;
	int ret, soc;

	ret = max17048_get_crate(battery, &crate);
	if (ret)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	/*
	 * CRATE LSB is 0.208%/hr.
	 * Threshold of 4 LSB (~0.8%/hr) for noise immunity.
	 */
	if (crate > 4)
		return POWER_SUPPLY_STATUS_CHARGING;
	if (crate < -4)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	soc = max17048_get_soc(battery);
	
	/*
	 * If we are in the noise threshold (neither charging/discharging significantly)
	 * and SOC is high (> 95%), assume we are fully charged and topped off.
	 * This prevents "Not Charging" or "Discharging" confused states at 100%.
	 */
	if (soc >= 95)
		return POWER_SUPPLY_STATUS_FULL;

	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int max17048_get_current(struct max17048 *battery, int *val)
{
	int16_t crate;
	int ret;

	ret = max17048_get_crate(battery, &crate);
	if (ret)
		return ret;

	/*
	 * C-Rate LSB is 0.208%/hr.
	 * Current = Capacity * C-Rate
	 * Current (uA) = Capacity (mAh) * 1000 * (crate * 0.208 / 100)
	 *              = (charge_design_uah / 1000) * 1000 * ...
	 *              = charge_design_uah * crate * 0.208 / 100
	 *              = charge_design_uah * crate * 52 / 25000
	 */
	*val = (int)div_s64((s64)battery->charge_full_design_uah * crate * 52, 25000);
	return 0;
}

static int max17048_get_time_to_empty(struct max17048 *battery, int *val)
{
	int16_t crate;
	int ret, soc;
	int32_t discharge_rate, min_discharge_rate;

	ret = max17048_get_crate(battery, &crate);
	if (ret)
		return ret;

	if (crate >= -10)
		return -ENODATA; // Not discharging enough

	soc = max17048_get_soc(battery);
	if (soc < 0)
		return soc;

	/*
	 * Conservative estimation:
	 * Ensure we assume at least a minimum system load (300mA) to avoid
	 * inflated time estimates during idle.
	 * 300mA in C-Rate LSBs = (300000 * 100) / (Capacity_uAh * 0.208)
	 *                      = 30,000,000 / (Cap_uAh * 0.208)
	 *                      = 144,230,769 / Cap_uAh
	 */
	discharge_rate = abs(crate);
	if (battery->charge_full_design_uah > 0) {
		min_discharge_rate = 144230769 / battery->charge_full_design_uah;
		if (discharge_rate < min_discharge_rate)
			discharge_rate = min_discharge_rate;
	} else {
		/* Fallback if capacity missing: prevent div/0 later? 
		   No, discharge_rate is used in denominator. 
		   If discharge_rate is 0 (abs(crate)=0), we have div/0 risk below:
		   (discharge_rate * 13) 
		   We should ensure discharge_rate > 0.
		*/
		if (discharge_rate == 0)
			return -ENODATA;
	}
	
	if (discharge_rate <= 0) // Should be caught above or by min_rate if cap > 0
		return -ENODATA;

	/*
	 * TTE (s) = 225000 * soc / (discharge_rate * 13)
	 * Use div_s64 for safety though inputs are unlikely to overflow 32-bit here.
	 */
	*val = (int)div_s64((s64)225000 * soc, (s64)discharge_rate * 13);
	return 0;
}

static int max17048_get_time_to_full(struct max17048 *battery, int *val)
{
	int16_t crate;
	int ret, soc;

	ret = max17048_get_crate(battery, &crate);
	if (ret)
		return ret;

	if (crate <= 10)
		return -ENODATA; // Not charging enough

	soc = max17048_get_soc(battery);
	if (soc < 0)
		return soc;
	
	if (crate == 0)
		return -ENODATA;

	*val = (int)div_s64((s64)225000 * (100 - soc), (s64)crate * 13);
	return 0;
}

static int max17048_get_property(struct power_supply *psy,
				 enum power_supply_property psp,
				 union power_supply_propval *val)
{
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
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = max17048_get_soc(battery);
		if (ret < 0)
			return ret;
		/* uAh = (soc * charge_design_uah) / 100 */
		val->intval = (int)div_s64((s64)ret * battery->charge_full_design_uah, 100); 
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = (int)battery->charge_full_design_uah;
		break;
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
	default:
		return -EINVAL;
	}
	return 0;
}

static enum power_supply_property max17048_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
};

static const struct power_supply_desc max17048_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.get_property = max17048_get_property,
	.properties = max17048_battery_props,
	.num_properties = ARRAY_SIZE(max17048_battery_props),
};

static int max17048_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max17048 *max17048_desc;
	struct power_supply_config psycfg = {};
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	max17048_desc = devm_kzalloc(dev, sizeof(struct max17048), GFP_KERNEL);
	if (!max17048_desc)
		return -ENOMEM;

	max17048_desc->client = client;
	max17048_desc->regmap =
		devm_regmap_init_i2c(client, &max17048_regmap_cfg);

	ret = device_property_read_u32(dev, "charge-full-design-microamp-hours",
				       &max17048_desc->charge_full_design_uah);
	
	if (max17048_desc->charge_full_design_uah > 100000000) {
		dev_warn(dev, "Capacity too high (%u), clamping to 100Ah\n", max17048_desc->charge_full_design_uah);
		max17048_desc->charge_full_design_uah = 100000000;
	}

	if (ret) {
		/* Fallback to legacy battery-capacity (mAh) */
		u32 cap_mah = 0;
		if (device_property_read_u32(dev, "battery-capacity", &cap_mah) == 0) {
			if (cap_mah > 0 && cap_mah < 20000) {
				max17048_desc->charge_full_design_uah = cap_mah * 1000;
			}
		}
	}
	
	/* Final Default if still 0 */
	if (max17048_desc->charge_full_design_uah == 0) {
		dev_warn(dev, "Capacity not configured, default 5000mAh\n");
		max17048_desc->charge_full_design_uah = 5000000;
	}

	/* Read Energy Design or estimate from Charge Design (assuming 3.7V nominal) */
	ret = device_property_read_u32(dev, "energy-full-design-microwatt-hours",
				       &max17048_desc->energy_full_design_uwh);
	if (ret || max17048_desc->energy_full_design_uwh == 0) {
	   	max17048_desc->energy_full_design_uwh = 
	   		(u32)div_u64((u64)max17048_desc->charge_full_design_uah * 37, 10);
	}
	
	if (max17048_desc->energy_full_design_uwh > 370000000) {
		max17048_desc->energy_full_design_uwh = 370000000; // Cap at ~370Wh
	}

	dev_info(dev, "MAX17048: Design: %u uAh, %u uWh\n", 
		 max17048_desc->charge_full_design_uah,
		 max17048_desc->energy_full_design_uwh);

	if (IS_ERR(max17048_desc->regmap))
		return PTR_ERR(max17048_desc->regmap);

	psycfg.drv_data = max17048_desc;
	psycfg.of_node = dev->of_node;

	max17048_desc->battery = devm_power_supply_register(
		dev, &max17048_battery_desc, &psycfg);
	if (IS_ERR(max17048_desc->battery)) {
		dev_err(&client->dev,
			"Failed to register power supply device\n");
		return PTR_ERR(max17048_desc->battery);
	}

	return 0;
}

static struct of_device_id max17048_of_ids[] = {
	{ .compatible = "hackberrypi,max17048-battery" },
	{}
};

MODULE_DEVICE_TABLE(of, max17048_of_ids);

static struct i2c_device_id max17048_i2c_ids[] = { };

static struct i2c_driver max17048_driver = {
	.driver = { .name = "max17048", .of_match_table = max17048_of_ids },
	.probe = max17048_probe,
	.id_table = max17048_i2c_ids
};

module_i2c_driver(max17048_driver);

MODULE_DESCRIPTION("MAX17048 fuel gauge driver for HackBerryPi CM5");
MODULE_AUTHOR("CNflysky <cnflysky@qq.com>");
MODULE_LICENSE("GPL");
