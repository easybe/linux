// SPDX-License-Identifier: GPL-2.0+
/*
 * 7-segment display driver
 *
 * Copyright (C) 2020 Ezra Buehler
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include <uapi/linux/map_to_7segment.h>

#include "fd6551.h"

#define SEG7DISP_GLYPHS_SIZE 7 + 1
#define SEG7DISP_ATTRS_SIZE SEG7DISP_GLYPHS_SIZE + 2

static DEFINE_MUTEX(seg7disp_mutex);

struct seg7disp_glyph {
	const char* name;
	u32 bit;
	struct device_attribute devattr;
};

static struct seg7disp_data {
	struct i2c_client *controller;
	bool enable;
	u8 ndigits;
	u32 *digit_addrs;
	u32 glyph_addr;
	u32 enable_addr;
	bool flipped;
	struct seg7disp_glyph *glyphs[SEG7DISP_GLYPHS_SIZE];
	u8 glyphs_state;
} seg7disp_data;

static SEG7_DEFAULT_MAP(map_seg7);

unsigned char flip_seg7(unsigned char val)
{
	unsigned char new = 0;

	new |= val & 1 << BIT_SEG7_A ? 1 << BIT_SEG7_D : 0;
	new |= val & 1 << BIT_SEG7_B ? 1 << BIT_SEG7_E : 0;
	new |= val & 1 << BIT_SEG7_C ? 1 << BIT_SEG7_F : 0;
	new |= val & 1 << BIT_SEG7_D ? 1 << BIT_SEG7_A : 0;
	new |= val & 1 << BIT_SEG7_E ? 1 << BIT_SEG7_B : 0;
	new |= val & 1 << BIT_SEG7_F ? 1 << BIT_SEG7_C : 0;
	new |= val & 1 << BIT_SEG7_G;

	return new;
}

static ssize_t digits_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "tbd\n");
}

static ssize_t digits_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	int i;
	int rc;
	u8 val, addr;

	for (i = 0; i < seg7disp_data.ndigits; i++) {
		val = map_to_seg7(&map_seg7, buf[i]);
		if (seg7disp_data.flipped)
			val = flip_seg7(val);
		addr = seg7disp_data.digit_addrs[i];
		rc = fd6551_write(seg7disp_data.controller, addr, val);
		if (rc < 0) {
			pr_err("Failed to write to display controller\n");
			return rc;
		}
	}

	return len;
}

static DEVICE_ATTR_RW(digits);

static ssize_t enable_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", seg7disp_data.enable);
}

static ssize_t enable_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	if (buf[0] == '1')
		seg7disp_data.enable = true;
	else if (buf[0] == '0')
		seg7disp_data.enable = false;
	else
		return -EINVAL;

	fd6551_write(seg7disp_data.controller, seg7disp_data.enable_addr,
		     seg7disp_data.enable ? 1 : 0);

	return len;
}

static DEVICE_ATTR_RW(enable);

static ssize_t glyph_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%02x\n", seg7disp_data.glyphs_state);
}

static ssize_t glyph_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	int i;
	int rc;
	const struct seg7disp_glyph *glyph;
	const char *name = attr->attr.name;
	bool enable;

	if (buf[0] == '1')
		enable = true;
	else if (buf[0] == '0')
		enable = false;
	else
		return -EINVAL;

	for (i = 0; i < SEG7DISP_GLYPHS_SIZE - 1; i++) {
		if (!strcmp(seg7disp_data.glyphs[i]->name, name)) {
			glyph = seg7disp_data.glyphs[i];

			if (enable)
				seg7disp_data.glyphs_state |= 1 << glyph->bit;
			else
				seg7disp_data.glyphs_state &= ~(1 << glyph->bit);

			rc = fd6551_write(seg7disp_data.controller,
					  seg7disp_data.glyph_addr,
					  seg7disp_data.glyphs_state);
			if (rc < 0) {
				pr_err("Failed to write to display controller\n");
				return rc;
			}
			break;
		}
	}

	return len;
}

static struct attribute *seg7disp_attrs[SEG7DISP_ATTRS_SIZE] = {
	&dev_attr_enable.attr,
	NULL,
};

static const struct attribute_group seg7disp_group = {
	.name = "seg7disp",
	.attrs = seg7disp_attrs,
};

static int seg7disp_register_digits(struct device *dev)
{
	int i;
	int ret;

	ret = ENOBUFS;
	for (i = 0; i < SEG7DISP_ATTRS_SIZE - 1; i++) {
		if (seg7disp_attrs[i] == NULL) {
			seg7disp_attrs[i] = &dev_attr_digits.attr;
			seg7disp_attrs[i + 1] = NULL;
			ret = 0;
			break;
		}
	}

	if (!ret)
		ret = sysfs_update_group(&dev->kobj, &seg7disp_group);

	return ret;
}

static int seg7disp_register_glyph(struct device *dev, const char *name,
				   u32 bit)
{
	int i;
	int ret;
	struct seg7disp_glyph *glyph;

	ret = -ENOBUFS;
	for (i = 0; i < SEG7DISP_GLYPHS_SIZE - 1; i++) {
		if (seg7disp_data.glyphs[i] == NULL) {
			glyph = devm_kzalloc(dev, sizeof(struct seg7disp_glyph),
					     GFP_KERNEL);
			glyph->name = name;
			glyph->bit = bit;
			glyph->devattr.attr.name = name;
			glyph->devattr.attr.mode = 0644;
			glyph->devattr.show = &glyph_show;
			glyph->devattr.store = &glyph_store;
			seg7disp_data.glyphs[i] = glyph;
			seg7disp_data.glyphs[i + 1] = NULL;
			ret = 0;
			break;
		}
	}

	if (ret)
		return ret;

	ret = -ENOBUFS;
	for (i = 0; i < SEG7DISP_ATTRS_SIZE - 1; i++) {
		if (seg7disp_attrs[i] == NULL) {
			seg7disp_attrs[i] = &glyph->devattr.attr;
			seg7disp_attrs[i + 1] = NULL;
			ret = 0;
			break;
		}
	}

	if (!ret)
		ret = sysfs_update_group(&dev->kobj, &seg7disp_group);

	return ret;
}

static void seg7disp_init(void)
{
	seg7disp_data.controller = NULL;
	seg7disp_data.enable = false;
	seg7disp_data.ndigits = 0;
	seg7disp_data.digit_addrs = NULL;
	seg7disp_data.enable_addr = 0;
	seg7disp_data.glyph_addr = 0;
	seg7disp_data.flipped = false;
	seg7disp_data.glyphs[0] = NULL;
	seg7disp_data.glyphs_state = 0;
}

static int seg7disp_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int err;
	size_t i;
	struct device_node *node;
	const char *names[SEG7DISP_GLYPHS_SIZE];
	size_t nglyphs;

	pr_info("hello\n");

	seg7disp_init();

	node = of_parse_phandle(np, "display-controller", 0);
	if (!node) {
		pr_err_ratelimited("Display controller property not found\n");
		err = -EINVAL;
		goto error;
	}
	seg7disp_data.controller = of_find_i2c_device_by_node(node);
	if (!seg7disp_data.controller) {
		err = -EPROBE_DEFER;
		goto error;
	}

	err = sysfs_create_group(&pdev->dev.kobj, &seg7disp_group);
	if (err < 0) {
		dev_err(&pdev->dev, "couldn't register sysfs group\n");
		goto error;
	}

	for_each_child_of_node(np, node) {
		if (of_node_name_eq(node, "digits")) {

			seg7disp_data.ndigits = of_property_count_elems_of_size(
				node, "reg", sizeof(u32));
			if (seg7disp_data.ndigits <= 0) {
				pr_err_ratelimited("No digits configured\n");
				continue;
			}
			seg7disp_data.digit_addrs = kcalloc(
				seg7disp_data.ndigits, sizeof(u32), GFP_KERNEL);
			if (!seg7disp_data.digit_addrs) {
				pr_err_ratelimited(
					"Could not allocate memory for digits\n");
				continue;
			}
			err = of_property_read_u32_array(
				node, "reg", seg7disp_data.digit_addrs,
				seg7disp_data.ndigits);
			if (err < 0) {
				pr_err_ratelimited(
					"Could not read digit addresses\n");
				continue;
			}
			err = seg7disp_register_digits(&pdev->dev);
			if (err < 0) {
				pr_err_ratelimited(
					"Could not register digits\n");
				continue;
			}
			seg7disp_data.flipped = of_property_read_bool(
				node, "digits-flipped");

		} else if (of_node_name_eq(node, "glyphs")) {
			err = of_property_read_u32(
				node, "reg", &seg7disp_data.glyph_addr);
			if (err < 0) {
				pr_err_ratelimited(
					"Could not read glyph reg property\n");
				continue;
			}
			nglyphs = of_property_count_strings(node,
							    "glyph-names");
			err = of_property_read_string_array(node, "glyph-names",
							    names, nglyphs);
			if (err < 0) {
				pr_err_ratelimited(
					"Could not read glyph name\n");
				continue;
			}
			for (i = 0; i < nglyphs; i++) {
				err = seg7disp_register_glyph(&pdev->dev,
							      names[i], i);
				if (err < 0) {
					pr_err_ratelimited(
						"Could not register glyph '%s'\n",
						names[i]);
					continue;
				}
			}
		} else if (of_node_name_eq(node, "enable")) {
			err = of_property_read_u32(
				node, "reg", &seg7disp_data.enable_addr);
			if (err < 0) {
				pr_err_ratelimited(
					"Could not read enable reg property\n");
				continue;
			}
		}
	}

	if (!seg7disp_data.enable_addr) {
		pr_err_ratelimited("No 'enable' node found in DT\n");
		err = -EINVAL;
		goto free_digits;
	}

	return 0;

free_digits:
	if (seg7disp_data.digit_addrs)
		kfree(seg7disp_data.digit_addrs);
error:
	return err;
}

static int seg7disp_remove(struct platform_device *pdev)
{
	pr_info("remove\n");
	sysfs_remove_group(&pdev->dev.kobj, &seg7disp_group);
	kfree(seg7disp_data.digit_addrs);
	return 0;
}

static const struct of_device_id seg7disp_match[] = {
	{ .compatible = "seven-segment-display" },
	{ },
};
MODULE_DEVICE_TABLE(of, seg7disp_match);

static struct platform_driver seg7disp_driver = {
	.driver = {
		.name		= "seg7disp",
		.of_match_table = seg7disp_match,
	},
	.probe	= seg7disp_probe,
	.remove = seg7disp_remove,
};

module_platform_driver(seg7disp_driver);

MODULE_DESCRIPTION("7-segment display driver");
MODULE_AUTHOR("Ezra Buehler <ezra@easyb.ch>");
MODULE_LICENSE("GPL v2");
