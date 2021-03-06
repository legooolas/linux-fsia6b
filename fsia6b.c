/*
 *
 *  FS-iA6B iBus RC receiver kernel driver
 *  Copyright (C) 2018  Markus Koch <markus@notsyncing.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/slab.h>

#define SERIO_FSIA6B 100 /* TODO: This should be moved to the serio header */

#define DRIVER_DESC	"FS-iA6B iBus RC receiver"

MODULE_AUTHOR("Markus Koch <markus@notsyncing.net>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define IBUS_SERVO_COUNT 14
#define IBUS_SWITCH_OFFSET 6

static int fsia6b_axes[IBUS_SERVO_COUNT] = {
	ABS_X, ABS_Y,
	ABS_Z, ABS_RX,
	ABS_RY, ABS_RZ,
	ABS_HAT0X, ABS_HAT0Y,
	ABS_HAT1X, ABS_HAT1Y,
	ABS_HAT2X, ABS_HAT2Y,
	ABS_HAT3X, ABS_HAT3Y
};

enum ibus_state {SYNC, COLLECT, PROCESS};

struct ibus_packet {
	enum ibus_state state;

	int offset;
	uint16_t ibuf;
	uint16_t channel[IBUS_SERVO_COUNT];
};

struct fsia6b {
	struct input_dev *dev;
	struct ibus_packet packet;

	char phys[32];
};

static void fsia6b_serio_write_wakeup(struct serio *serio)
{
	struct fsia6b *fsia6b = serio_get_drvdata(serio);
	printk(KERN_INFO "fsia6b: write wakeup.\n");
}

static irqreturn_t fsia6b_serio_irq(struct serio *serio,
				    unsigned char data, unsigned int flags)
{
	struct fsia6b *fsia6b = serio_get_drvdata(serio);
	int i;

	fsia6b->packet.ibuf = (data << 8) | ((fsia6b->packet.ibuf >> 8) & 0xFF);

	switch (fsia6b->packet.state) {
	case SYNC:
		if (fsia6b->packet.ibuf == 0x4020) {
			fsia6b->packet.state = COLLECT;
		}
		break;

	case COLLECT:
		fsia6b->packet.state = PROCESS;
		break;

	case PROCESS:
		fsia6b->packet.channel[fsia6b->packet.offset] = fsia6b->packet.ibuf;
		fsia6b->packet.offset++;

		if (fsia6b->packet.offset == IBUS_SERVO_COUNT) {
			fsia6b->packet.offset = 0;
			fsia6b->packet.state = SYNC;
			for (i = 0; i < IBUS_SERVO_COUNT; ++i) {
				input_report_abs(fsia6b->dev, fsia6b_axes[i],
						 fsia6b->packet.channel[i]);
				if (i >= IBUS_SWITCH_OFFSET) {
					input_report_key(fsia6b->dev, BTN_1 + i*1,
							 (fsia6b->packet.channel[i] > 1900));
				}
			}
			input_sync(fsia6b->dev);
		} else {
			fsia6b->packet.state = COLLECT;
		}
		break;
	}

	return IRQ_HANDLED;
}

int fsia6b_init_device(struct fsia6b *fsia6b)
{
	printk(KERN_INFO "fsia6b: init.\n");
	return 0;
}

static int fsia6b_serio_connect(struct serio *serio, struct serio_driver *drv)
{
	struct fsia6b *fsia6b;
	struct input_dev *input_dev;
	int err;
	int i;

	printk(KERN_INFO "fsia6b: connect.\n");

	fsia6b = kzalloc(sizeof(struct fsia6b), GFP_KERNEL);
	if (!fsia6b)
		return -ENOMEM;

	fsia6b->packet.ibuf = 0;
	fsia6b->packet.offset = 0;
	fsia6b->packet.state = SYNC;

	serio_set_drvdata(serio, fsia6b);

	err = serio_open(serio, drv);
	if (err)
		goto fail1;


	input_dev = input_allocate_device();
	err = -ENODEV;
	if (!input_dev)
		goto fail2;
	fsia6b->dev = input_dev;

	snprintf(fsia6b->phys, sizeof(fsia6b->phys), "%s/input0", serio->phys);

	input_dev->name = DRIVER_DESC;
	input_dev->phys = fsia6b->phys;
	input_dev->id.bustype = BUS_RS232;
	input_dev->id.vendor = SERIO_FSIA6B;
	input_dev->id.product = serio->id.id;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serio->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	for (i = 0; i < IBUS_SERVO_COUNT; ++i) {
		input_set_abs_params(input_dev, fsia6b_axes[i], 1000, 2000, 2, 2);
		if (i >= IBUS_SWITCH_OFFSET) {
			input_dev->keybit[BIT_WORD(BTN_0)] |= BIT_MASK(BTN_1 + i);
		}
	}

	err = input_register_device(fsia6b->dev);
	if (err)
		goto fail2;

	err = fsia6b_init_device(fsia6b);
	if (err)
		goto fail3;

	return 0;

 fail3: input_free_device(input_dev);
 fail2:	serio_close(serio);
 fail1:	serio_set_drvdata(serio, NULL);
	kfree(fsia6b);
	return err;
}

static void fsia6b_serio_disconnect(struct serio *serio)
{
	struct fsia6b *fsia6b = serio_get_drvdata(serio);

	printk(KERN_INFO "fsia6b: disconnect.\n");

	serio_close(serio);
	serio_set_drvdata(serio, NULL);
	input_unregister_device(fsia6b->dev);
	kfree(fsia6b);
}

static const struct serio_device_id fsia6b_serio_ids[] = {
	{
		.type	= SERIO_RS232,
		.proto	= SERIO_FSIA6B,
		.id	= SERIO_ANY,
		.extra	= SERIO_ANY,
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(serio, fsia6b_serio_ids);

struct serio_driver fsia6b_serio_drv = {
	.driver		= {
		.name	= "fsia6b"
	},
	.description	= DRIVER_DESC,
	.id_table	= fsia6b_serio_ids,
	.write_wakeup	= fsia6b_serio_write_wakeup,
	.interrupt	= fsia6b_serio_irq,
	.connect	= fsia6b_serio_connect,
	.disconnect	= fsia6b_serio_disconnect
};

module_serio_driver(fsia6b_serio_drv)
