// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Ezra Buehler <spam@easyb.ch>
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

#include "fd6551.h"

#define TEST 1

struct fd6551_data {
	struct mutex update_lock;
	u8 i2c_addr;
};


#ifdef TEST

static int fd6551_test(struct i2c_client *client, const char *args, size_t len)
{
	char *arg, *buf;
	u8 addr, val;
	long res;
	int err;

	buf = kmalloc(len, GFP_KERNEL);
	strlcpy(buf, args, len);
	arg = strsep(&buf, " ");
	err = kstrtol(arg, 0, &res);
	if (err)
		goto error;
	addr = (u8)res;
	arg = strsep(&buf, " ");
	if (arg == NULL) {
		dev_err(&client->dev, "Second argument missing, aborting...");
		err = -1;
		goto error;
	}
	err = kstrtol(arg, 0, &res);
	if (err)
		goto error;
	val = (u8)res;

	dev_dbg(&client->dev, "Sending: 0x%02x, 0x%02x", addr, val);

	return fd6551_write(client, addr, val);

error:
	kfree(buf);
	return err;
}

static ssize_t show_test(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "Usage: echo <addr> <val> > test\n");
}

static ssize_t store_test(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	struct i2c_client *client;
	int ret;
	client = to_i2c_client(dev);

	ret = fd6551_test(client, buf, len);
	if (ret < 0)
		return ret;

	return len;
}

static DEVICE_ATTR(test, S_IRUGO | S_IWUSR, show_test, store_test);

static struct attribute *fd6551_attrs[] = {
	&dev_attr_test.attr,
	NULL,
};

static const struct attribute_group fd6551_group = {
	.name = "fd6551",
	.attrs = fd6551_attrs,
};

#endif

int fd6551_write(struct i2c_client *client, u8 addr, u8 val)
{
	struct fd6551_data *data = i2c_get_clientdata(client);
	u8 buf = val;

	if (mutex_lock_interruptible(&data->update_lock) < 0)
		return -EAGAIN;

	dev_dbg(&client->dev, "addr: %02x, val: %02x\n", addr, val);

	client->addr = addr;
	i2c_transfer_buffer_flags(client, &buf, 1, I2C_M_IGNORE_NAK);
	client->addr = data->i2c_addr;

	mutex_unlock(&data->update_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(fd6551_write);

static void fd6551_init_hw(struct i2c_client *client)
{
	dev_dbg(&client->dev, "Init");
}

static int fd6551_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct fd6551_data *data;
	int err;

	data = devm_kzalloc(&client->dev,
			sizeof(struct fd6551_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto exit;
	}

	data->i2c_addr = client->addr;

	i2c_set_clientdata(client, data);
	mutex_init(&data->update_lock);

    #ifdef TEST
	err = sysfs_create_group(&client->dev.kobj, &fd6551_group);
	if (err < 0) {
		dev_err(&client->dev, "couldn't register sysfs group\n");
		goto exit;
	}
    #endif

	fd6551_init_hw(client);

	return 0;

exit:
	return err;
}

static int fd6551_remove(struct i2c_client *client)
{
    #ifdef TEST
	sysfs_remove_group(&client->dev.kobj, &fd6551_group);
    #endif
	return 0;
}

static const struct i2c_device_id fd6551_id[] = {
	{"fd6551", 0},
	{}
};

static const struct of_device_id fd6551_i2c_dt_match[] = {
	{ .compatible = "fdhisi,fd6551" },
	{},
};
MODULE_DEVICE_TABLE(of, fd6551_i2c_dt_match);

static struct i2c_driver fd6551_i2c_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		   .name = "fd6551",
		   },
	.probe = fd6551_probe,
	.remove = fd6551_remove,
	.id_table = fd6551_id,
};

module_i2c_driver(fd6551_i2c_driver);

MODULE_DESCRIPTION("FD6551 driver");
MODULE_AUTHOR("Ezra Buehler <spam@easyb.ch");
MODULE_LICENSE("GPL v2");
