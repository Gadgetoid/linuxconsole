/*
 * $Id$
 *
 *  Force feedback support for hid devices.
 *  Not all hid devices use the same protocol. For example, some use PID,
 *  other use their own proprietary procotol.
 *
 *  Copyright (c) 2002 Johann Deneux
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by
 * e-mail - mail your message to <deneux@ifrance.com>
 */

#include <linux/input.h>
#include <linux/sched.h>

#undef DEBUG

#include <linux/usb.h>

#include "hid.h"

/* Effect status */
#define EFFECT_STARTED 0     /* Effect is going to play after some time
				(ff_replay.delay) */
#define EFFECT_PLAYING 1     /* Effect is being played */
#define EFFECT_USED    2

/* Check that the current process can access an effect */
#define CHECK_OWNERSHIP(effect) (current->pid == 0 \
        || effect.owner == current->pid)


/* Drivers' initializing functions */
static int hid_lgff_init(struct hid_device* hid);

struct hid_ff_initializer {
	__u16 idVendor;
	__u16 idProduct;
	int (*init)(struct hid_device*);
};

static struct hid_ff_initializer inits[] = {
#ifdef CONFIG_LOGITECH_RUMBLE
	{0x46d, 0xc211, hid_lgff_init},
#endif
	{0, 0, NULL} /* Terminating entry */
};

static struct hid_ff_initializer *hid_get_ff_init(__u16 idVendor,
						  __u16 idProduct)
{
	struct hid_ff_initializer *init;
	for (init = inits;
	     init->idVendor
		     && !(init->idVendor == idVendor
			  && init->idProduct == idProduct);
	     init++);

	return init->idVendor? init : NULL;
}

int hid_ff_init(struct hid_device* hid)
{
	struct hid_ff_initializer *init;

	init = hid_get_ff_init(hid->dev->descriptor.idVendor,
			       hid->dev->descriptor.idProduct);

	return init? init->init(hid) : -ENOSYS;
}




/* **************************************************************************/
/* Implements the protocol used by the Logitech WingMan Cordless rumble pad */
/* **************************************************************************/

#ifdef CONFIG_LOGITECH_RUMBLE

#define LGFF_CHECK_OWNERSHIP(i, l) \
        (i>=0 && i<LGFF_EFFECTS \
        && test_bit(EFFECT_USED, l->effects[i].flags) \
        && CHECK_OWNERSHIP(l->effects[i]))

#define LGFF_BUFFER_SIZE 8
#define LGFF_EFFECTS 8

struct lgff_effect {
	pid_t owner;
	unsigned char left;        /* Magnitude of vibration for left motor */
	unsigned char right;       /* Magnitude of vibration for right motor */
	struct ff_replay replay;
	unsigned long flags[1];
};

struct hid_ff_logitech {
	struct urb* urbffout;        /* Output URB used to send ff commands */
	struct usb_ctrlrequest ffcr; /* ff commands use control URBs */
	char buf[LGFF_BUFFER_SIZE];
	struct lgff_effect effects[LGFF_EFFECTS];
	spinlock_t lock;             /* device-level lock. Having locks on
					a per-effect basis could be nice, but
					isn't really necessary */
	wait_queue_head_t wait;
};


static void hid_lgff_ctrl_out(struct urb *urb);
static void hid_lgff_exit(struct hid_device* hid);
static int hid_lgff_event(struct hid_device *hid, struct input_dev *input,
			  unsigned int type, unsigned int code, int value);
static void hid_lgff_make_rumble(struct hid_device* hid);

static int hid_lgff_flush(struct input_dev *input, struct file *file);
static int hid_lgff_upload_effect(struct input_dev *input,
				  struct ff_effect *effect);
static int hid_lgff_erase(struct input_dev *input, int id);
static void hid_lgff_ctrl_playback(struct hid_device* hid, struct lgff_effect*,
				   int play);

static int hid_lgff_init(struct hid_device* hid)
{
	struct hid_ff_logitech *private;

	/* Private data */
	hid->ff_private = kmalloc(sizeof(struct hid_ff_logitech), GFP_KERNEL);
	private = hid->ff_private;
	if (!private) return -1;

	memset(private, 0, sizeof(struct hid_ff_logitech));

	hid->ff_private = private;

	spin_lock_init(&private->lock);
	init_waitqueue_head(&private->wait);

	/* Event and exit callbacks */
	hid->ff_exit = hid_lgff_exit;
	hid->ff_event = hid_lgff_event;

	/* USB init */
	if (!(private->urbffout = usb_alloc_urb(0, GFP_KERNEL))) {
		kfree(hid->ff_private);
		return -1;
	}

	usb_fill_control_urb(private->urbffout, hid->dev, 0,
			     (void*) &private->ffcr, private->buf, 8,
			     hid_lgff_ctrl_out, hid);
	dbg("Created ff output control urb");

	/* Input init */
	hid->input.upload_effect = hid_lgff_upload_effect;
	hid->input.flush = hid_lgff_flush;
	set_bit(FF_RUMBLE, hid->input.ffbit);
	set_bit(EV_FF, hid->input.evbit);
	hid->input.ff_effects_max = LGFF_EFFECTS;

	printk(KERN_INFO "Force feedback for Logitech rumble devices by Johann Deneux <deneux@ifrance.com>\n");

	return 0;
}

static void hid_lgff_exit(struct hid_device* hid)
{
	struct hid_ff_logitech *lgff = hid->ff_private;
	DECLARE_WAITQUEUE(wait, current);
	int timeout = 5*HZ; /* 5 seconds */

	if (lgff->urbffout) {
		usb_unlink_urb(lgff->urbffout);
		
		set_current_state(TASK_INTERRUPTIBLE);
		add_wait_queue(&lgff->wait, &wait);

		if (lgff->urbffout->status == -EINPROGRESS)
			timeout = schedule_timeout(timeout);

		if (!timeout)
			warn("ff control urb still in use. Unlinking anyway\n");

		set_current_state(TASK_RUNNING);
		remove_wait_queue(&lgff->wait, &wait);

		usb_free_urb(lgff->urbffout);
	}
}

static int hid_lgff_event(struct hid_device *hid, struct input_dev* input,
			  unsigned int type, unsigned int code, int value)
{
	struct hid_ff_logitech *lgff = hid->ff_private;
	struct lgff_effect *effect = lgff->effects + code;
	unsigned long flags;

	if (type != EV_FF) return -EINVAL;
	
	if (!LGFF_CHECK_OWNERSHIP(code, lgff)) return -EACCES;

	if (value < 0) return -EINVAL;

	spin_lock_irqsave(&lgff->lock, flags);
	hid_lgff_ctrl_playback(hid, effect, value);
	spin_unlock_irqrestore(&lgff->lock, flags);

	return 0;

}

/* Erase all effects this process owns */
static int hid_lgff_flush(struct input_dev *dev, struct file *file)
{
	struct hid_device *hid = dev->private;
	struct hid_ff_logitech *lgff = hid->ff_private;
	int i;

	for (i=0; i<dev->ff_effects_max; ++i) {

		/*NOTE: no need to lock here. The only times EFFECT_USED is
		  modified is when effects are uploaded or when an effect is
		  erased. But a process cannot close its dev/input/eventX fd
		  and perform ioctls on the same fd all at the same time */
		if ( current->pid == lgff->effects[i].owner
		     && test_bit(EFFECT_USED, lgff->effects[i].flags)) {
			
			if (hid_lgff_erase(dev, i))
				warn("erase effect %d failed\n", i);
		}

	}

	return 0;
}

static int hid_lgff_erase(struct input_dev *dev, int id)
{
	struct hid_device *hid = dev->private;
	struct hid_ff_logitech *lgff = hid->ff_private;
	unsigned long flags;

	if (!LGFF_CHECK_OWNERSHIP(id, lgff)) return -EACCES;

	spin_lock_irqsave(&lgff->lock, flags);
	hid_lgff_ctrl_playback(hid, lgff->effects + id, 0);
	lgff->effects[id].flags[0] = 0;
	spin_unlock_irqrestore(&lgff->lock, flags);

	return 0;
}

static int hid_lgff_upload_effect(struct input_dev* input,
				  struct ff_effect* effect)
{
	struct hid_device *hid = input->private;
	struct hid_ff_logitech *lgff = hid->ff_private;
	struct lgff_effect new;
	int id;
	unsigned long flags;
	
	dbg("ioctl rumble");

	if (!test_bit(effect->type, input->ffbit)) return -EINVAL;

	if (effect->type != FF_RUMBLE) return -EINVAL;

	spin_lock_irqsave(&lgff->lock, flags);

	if (effect->id == -1) {
		int i;

		for (i=0; i<LGFF_EFFECTS && test_bit(EFFECT_USED, lgff->effects[i].flags); ++i);
		if (i >= LGFF_EFFECTS) {
			spin_unlock_irqrestore(&lgff->lock, flags);
			return -ENOSPC;
		}

		effect->id = i;
		lgff->effects[i].owner = current->pid;
		lgff->effects[i].flags[0] = 0;
		set_bit(EFFECT_USED, lgff->effects[i].flags);
	}
	else if (!LGFF_CHECK_OWNERSHIP(effect->id, lgff)) {
		spin_unlock_irqrestore(&lgff->lock, flags);
		return -EACCES;
	}

	id = effect->id;
	new = lgff->effects[id];

	new.right = effect->u.rumble.strong_magnitude >> 9;
	new.left = effect->u.rumble.weak_magnitude >> 9;
	new.replay = effect->replay;

	/* If we updated an effect that was being played, we need to remake
	   the rumble effect */
	if (test_bit(EFFECT_STARTED, lgff->effects[id].flags)
	    || test_bit(EFFECT_STARTED, lgff->effects[id].flags)) {

		/* Changing replay parameters is not allowed (for the time
		   being) */
		if (new.replay.delay != lgff->effects[id].replay.delay
		    || new.replay.length != lgff->effects[id].replay.length) {
			spin_unlock_irqrestore(&lgff->lock, flags);
			return -ENOSYS;
		}

		lgff->effects[id] = new;
		hid_lgff_make_rumble(hid);

	} else {
		lgff->effects[id] = new;
	}

	spin_unlock_irqrestore(&lgff->lock, flags);
	return 0;
}

static void hid_lgff_make_rumble(struct hid_device* hid)
{
	struct hid_ff_logitech *lgff = hid->ff_private;
	char packet[] = {0x03, 0x42, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00};
	int err;
	int left = 0, right = 0;
	int i;

	dbg("in hid_make_rumble");
	memcpy(lgff->buf, packet, 8);


	for (i=0; i<LGFF_EFFECTS; ++i) {
		if (test_bit(EFFECT_USED, lgff->effects[i].flags)
		    && test_bit(EFFECT_PLAYING, lgff->effects[i].flags)) {
			left += lgff->effects[i].left;
			right += lgff->effects[i].right;
		}
	}

	lgff->buf[3] = left > 0x7f ? 0x7f : left;
	lgff->buf[4] = right > 0x7f ? 0x7f : right;

	/*FIXME: needs a queue. I should at least check if the urb is
	  available */
	lgff->urbffout->pipe = usb_sndctrlpipe(hid->dev, 0);
	lgff->ffcr.bRequestType = USB_TYPE_CLASS | USB_DIR_OUT | USB_RECIP_INTERFACE;
	lgff->urbffout->transfer_buffer_length = lgff->ffcr.wLength = 8;
	lgff->ffcr.bRequest = 9;
	lgff->ffcr.wValue = 0x0203;    /*NOTE: Potential problem with 
					 little/big endian */
	lgff->ffcr.wIndex = 0;
	
	lgff->urbffout->dev = hid->dev;
	
	if ((err=usb_submit_urb(lgff->urbffout, GFP_ATOMIC)))
		warn("usb_submit_urb returned %d", err);
	dbg("rumble urb submited");
}

static void hid_lgff_ctrl_out(struct urb *urb)
{
	struct hid_device *hid = urb->context;

	if (urb->status)
		warn("hid_irq_ffout status %d received", urb->status);

	wake_up(&((struct hid_ff_logitech *)(hid->ff_private))->wait);
}

/* Lock must be held by caller */
static void hid_lgff_ctrl_playback(struct hid_device *hid,
				   struct lgff_effect *effect, int play)
{
	if (play) {
		set_bit(EFFECT_PLAYING, effect->flags);
		hid_lgff_make_rumble(hid);

	} else {
		clear_bit(EFFECT_PLAYING, effect->flags);
		hid_lgff_make_rumble(hid);
	}
}

#endif /* CONFIG_LOGITECH_RUMBLE */