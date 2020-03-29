/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _FD6551_H
#define _FD6551_H

#include <linux/i2c.h>

int fd6551_write(struct i2c_client *client, u8 addr, u8 val);

#endif /* FD6551_H */
