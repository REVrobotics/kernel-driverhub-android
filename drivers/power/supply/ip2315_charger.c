/*
 * ip2315 Charger (Charger Interface) driver
 *
 * Copyright (C) 2020 Ahren Li <lili@vamrs.com>
 *
 * based on ip2315_power.c by ip2315
 * Copyright (C) 2020 ip2315, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/errno.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/gpio.h>

#define  MAX_USB_PHY_LEN 16

static bool  vbs = 0;
static char* vbs_name = NULL;

void   set_vbus_status(struct device* dev, bool status){
    if(likely(dev && vbs_name)){
        if(strstr(dev_name(dev), vbs_name)){
            vbs = status;
        }
    }
}
EXPORT_SYMBOL_GPL(set_vbus_status);

struct ip2315_charger {
    struct device*       dev;
    struct power_supply* pd;
    struct power_supply* poe;
    struct power_supply* usb;
    bool			     pd_is_active;
    bool			     poe_is_active;
    bool			     usb_is_active;

    int                  pd_gpio;
    struct iio_channel*  poe_chan;
    int                  work_delay;
    struct delayed_work  delay_work;
    char                 phy_name[MAX_USB_PHY_LEN];
};

static enum power_supply_property ip2315_charger_poe_props[] = {
        POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property ip2315_charger_pd_props[] = {
        POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property ip2315_charger_usb_props[] = {
        POWER_SUPPLY_PROP_ONLINE,
};

static int ip2315_charger_get_property(struct power_supply *psy, enum power_supply_property psp,
                                      union power_supply_propval *val){
    struct ip2315_charger *charger = dev_get_drvdata(psy->dev.parent);
    if(charger == NULL) return 0;

    switch (psp) {
        case POWER_SUPPLY_PROP_ONLINE:
            if (psy->desc->type == POWER_SUPPLY_TYPE_USB){
                val->intval = charger->usb_is_active;
            }else if(psy->desc->type == POWER_SUPPLY_TYPE_MAINS){
                val->intval = charger->poe_is_active;
            } else if(psy->desc->type == POWER_SUPPLY_TYPE_USB_PD){
                val->intval = charger->pd_is_active;
            }
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static const struct power_supply_desc ip2315_charger_poe_desc = {
        .name		= "ip2315_ac",
        .type		= POWER_SUPPLY_TYPE_MAINS,
        .properties	= ip2315_charger_poe_props,
        .num_properties	= ARRAY_SIZE(ip2315_charger_poe_props),
        .get_property	= ip2315_charger_get_property,
};

static const struct power_supply_desc ip2315_charger_pd_desc = {
        .name		= "ip2315_pd",
        .type		= POWER_SUPPLY_TYPE_USB_PD,
        .properties	= ip2315_charger_pd_props,
        .num_properties	= ARRAY_SIZE(ip2315_charger_pd_props),
        .get_property	= ip2315_charger_get_property,
};

static const struct power_supply_desc ip2315_charger_usb_desc = {
        .name		= "ip2315_usb",
        .type		= POWER_SUPPLY_TYPE_USB,
        .properties	= ip2315_charger_usb_props,
        .num_properties	= ARRAY_SIZE(ip2315_charger_usb_props),
        .get_property	= ip2315_charger_get_property,
};

static void ip2315_charger_delay_work_func(struct work_struct *work){
    struct delayed_work*  delay_work = container_of(work, struct delayed_work, work);
    struct ip2315_charger* charger = container_of(delay_work, struct ip2315_charger, delay_work);

    int result, poe_value, poe_active, pd_value;
    result = iio_read_channel_processed(charger->poe_chan, &poe_value);
    if (unlikely(result < 0)) {
        poe_value = 0;
    }

    poe_active = poe_value >= 800;
    if(charger->poe_is_active != poe_active){
        dev_info(charger->dev, "poe active");
        charger->poe_is_active = poe_active;
        power_supply_changed(charger->poe);
    }

    if(vbs){
        if(likely(gpio_is_valid(charger->pd_gpio))){
            pd_value = gpio_get_value(charger->pd_gpio);
        } else{
            pd_value = 1;
        }
//        if(!pd_value){
//            dev_info(charger->dev, "fast charge active");
//        }

        poe_active = poe_value >= 400;
        if(poe_active){
            if(!charger->pd_is_active){
                charger->pd_is_active = 1;
                power_supply_changed(charger->pd);
            }
            if(charger->usb_is_active){
                charger->usb_is_active = 0;
                power_supply_changed(charger->usb);
            }
        }else{
            if(!charger->usb_is_active){
                charger->usb_is_active = 1;
                power_supply_changed(charger->usb);
            }
            if(charger->pd_is_active){
                charger->pd_is_active = 0;
                power_supply_changed(charger->pd);
            }
        }

        if(charger->usb_is_active == poe_active){

        }

        /*if(!pd_value){
            if(!charger->pd_is_active){
                charger->pd_is_active = 1;
                power_supply_changed(charger->pd);
            }
            if(charger->usb_is_active){
                charger->usb_is_active = 0;
                power_supply_changed(charger->usb);
            }
        }else{
            if(!charger->usb_is_active){
                charger->usb_is_active = 1;
                power_supply_changed(charger->usb);
            }
            if(charger->pd_is_active){
                charger->pd_is_active = 0;
                power_supply_changed(charger->pd);
            }
        }*/
    }else{
        if(charger->pd_is_active != vbs){
            charger->pd_is_active = 0;
            power_supply_changed(charger->pd);
        }
        if(charger->usb_is_active != vbs){
            charger->usb_is_active = 0;
            power_supply_changed(charger->usb);
        }
        if(charger->poe_is_active != vbs){
            charger->poe_is_active = 0;
            power_supply_changed(charger->poe);
        }
    }

    schedule_delayed_work(delay_work, msecs_to_jiffies(charger->work_delay));
}

static int ip2315_charger_probe(struct platform_device *pdev){
    struct ip2315_charger* charger;
    enum iio_chan_type type;
    int result = 0;
    const char* local_vbs_name = NULL;

    charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
    if (charger == NULL)
        return -ENOMEM;

    charger->pd_gpio  = of_get_named_gpio_flags(pdev->dev.of_node, "pd-gpio", 0, NULL);
    if (!gpio_is_valid(charger->pd_gpio)) {
        return -ENODATA;
    }/*else{
        result = gpio_request(charger->pd_gpio, "pd-det");
        if (result) {
            dev_err(&pdev->dev, "request pd-det error %d\n", result);
        }else{
            if(gpio_direction_input(charger->pd_gpio)){
                dev_err(&pdev->dev, "direction input pd-det error\n");
            }
        }
    }*/

    charger->poe_chan = devm_iio_channel_get(&pdev->dev, "poe");
    if (IS_ERR(charger->poe_chan))
        return PTR_ERR(charger->poe_chan);

    if (!charger->poe_chan->indio_dev)
        return -ENXIO;

    result = iio_get_channel_type(charger->poe_chan, &type);
    if (result < 0)
        return result;

    if (type != IIO_VOLTAGE) {
        dev_err(&pdev->dev, "Incompatible channel type %d\n", type);
        return -EINVAL;
    }

    if(of_property_read_u32(pdev->dev.of_node, "work-delay-ms", &charger->work_delay)){
        charger->work_delay = 100;
    }

    of_property_read_string(pdev->dev.of_node, "usb-phy", &local_vbs_name);
    if(!local_vbs_name){
        dev_err(&pdev->dev, "Not set usb-phy !\n");
    }else{
        int len = strlen(local_vbs_name);
        memcpy(charger->phy_name, local_vbs_name, len >= MAX_USB_PHY_LEN ? MAX_USB_PHY_LEN - 1 : len);
    }
    vbs_name = charger->phy_name;
    INIT_DELAYED_WORK(&charger->delay_work, ip2315_charger_delay_work_func);
    platform_set_drvdata(pdev, charger);

    charger->pd = devm_power_supply_register(&pdev->dev, &ip2315_charger_pd_desc, NULL);
    if (IS_ERR(charger->pd)) {
        result = PTR_ERR(charger->pd);
        dev_err(&pdev->dev, "failed to register pd: %d\n", result);
        return result;
    }

    charger->poe = devm_power_supply_register(&pdev->dev, &ip2315_charger_poe_desc, NULL);
    if (IS_ERR(charger->poe)) {
        result = PTR_ERR(charger->poe);
        dev_err(&pdev->dev, "failed to register poe: %d\n", result);
        return result;
    }

    charger->usb = devm_power_supply_register(&pdev->dev, &ip2315_charger_usb_desc, NULL);
    if (IS_ERR(charger->usb)) {
        result = PTR_ERR(charger->usb);
        dev_err(&pdev->dev, "failed to register usb: %d\n", result);
        return result;
    }

    schedule_delayed_work(&charger->delay_work, msecs_to_jiffies(charger->work_delay));
    return 0;
}

static int ip2315_charger_remove(struct platform_device *pdev){
    struct ip2315_charger* charger = platform_get_drvdata(pdev);
    if(charger != NULL){
        cancel_delayed_work_sync(&charger->delay_work);

        power_supply_unregister(charger->pd);
        power_supply_unregister(charger->poe);
        power_supply_unregister(charger->usb);

        if(gpio_is_valid(charger->pd_gpio)){
            gpio_free(charger->pd_gpio);
        }
    }

    return 0;
}

static const struct of_device_id ip2315_charger_of_match[] = {
        {.compatible = "ip2315,charger", },
        { }
};
MODULE_DEVICE_TABLE(of, ip2315_charger_of_match);

static struct platform_driver ip2315_charger_driver = {
        .probe = ip2315_charger_probe,
        .remove	= ip2315_charger_remove,
        .driver	= {
                .name	= "ip2315_charger",
                .of_match_table = of_match_ptr(ip2315_charger_of_match),
        },
};
module_platform_driver(ip2315_charger_driver);

MODULE_AUTHOR("Ahren Li");
MODULE_DESCRIPTION("ip2315 Charger Interface driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:ip2315");