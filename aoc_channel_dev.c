// SPDX-License-Identifier: GPL-2.0-only
/* Copyright 2019 Google LLC. All Rights Reserved.
 *
 * Channelized character IPC device interface for AoC services
 * AOCC - AoC Channelized Comms
 */

#define pr_fmt(fmt) "aoc_chan: " fmt

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <linux/rwlock_types.h>
#include <linux/kthread.h>

#include "aoc.h"

#define AOCC_CHARDEV_NAME "aoc_chan"

static int aocc_major = -1;
static int aocc_major_dev;
static struct class *aocc_class;

#define AOCC_MAX_DEVICES 1

/* TODO(b/141396548): Update AOC channel driver to support multiple services. */
static struct device *aocc_devices[AOCC_MAX_DEVICES];
static unsigned long opened_devices;

#define AOCC_MAX_MSG_SIZE 1024
#define AOCC_MAX_PENDING_MSGS 32
static atomic_t channel_index_counter = ATOMIC_INIT(1);

/* Driver methods */
static int aocc_probe(struct aoc_service_dev *dev);
static int aocc_remove(struct aoc_service_dev *dev);

const char *channel_service_names[] = {
	"com.google.usf",
	NULL,
};

static struct aoc_driver aoc_chan_driver = {
	.drv = {
		.name = "aoc_chan",
	},
	.service_names = channel_service_names,
	.probe = aocc_probe,
	.remove = aocc_remove,
};

/* Message definitions. */
/* TODO: These should be synchronized with EFW source. b/140593553 */
struct aoc_message_node {
	struct list_head msg_list;
	size_t msg_size;
	union {
		char msg_buffer[AOCC_MAX_MSG_SIZE];
		struct {
			int channel_index;
			char payload[AOCC_MAX_MSG_SIZE - sizeof(int)];
		} __packed;
	};
};

enum aoc_cmd_code {
	AOCC_CMD_OPEN_CHANNEL = 0,
	AOCC_CMD_CLOSE_CHANNEL,
};

struct aocc_channel_control_msg {
	int channel_index; /* Should always be 0 for CMD messages */
	int command_code;
	int channel_to_modify;
} __packed;

struct file_prvdata {
	struct aoc_service_dev *service;
	int device_index;
	int channel_index;
	wait_queue_head_t read_queue;
	struct list_head open_files_list;
	struct list_head pending_aoc_messages;
	rwlock_t pending_msg_lock;
	atomic_t pending_msg_count;
};

/* Globals */
/* TODO(b/141396548): Move these to drv_data. */
LIST_HEAD(s_open_files);
rwlock_t s_open_files_lock;
static struct task_struct *s_demux_task;

/* Message related services */
static void aocc_send_cmd_msg(aoc_service *service_id, enum aoc_cmd_code code,
			      int channel_to_modify);

int aocc_demux_kthread(void *data)
{
	ssize_t retval = 0;
	struct aoc_service_dev *service = (struct aoc_service_dev *)data;

	pr_info("Demux handler started!");

	while (!kthread_should_stop()) {
		int handler_found = 0;
		int channel = 0;
		struct file_prvdata *entry;
		struct aoc_message_node *node =
			kmalloc(sizeof(struct aoc_message_node), GFP_KERNEL);

		INIT_LIST_HEAD(&node->msg_list);

		/* Attempt to read from the service, and block if we can't. */
		retval = aoc_service_read(service, node->msg_buffer,
					  AOCC_MAX_MSG_SIZE, true);

		if (retval < 0 || retval < sizeof(int)) {
			pr_err("Read failed with %ld", retval);
			kfree(node);

			if (retval == -ENODEV) {
				/*
				 * ENODEV indicates that the device is going
				 * away (most likely due to a firmware crash).
				 * At this point it is a race between the
				 * kthread and aocc_remove(), so we need to be
				 * careful to not miss the thread_stop event.
				 * By setting ourselves as INTERRUPTIBLE, that
				 * window is closed since a kthread_stop() will
				 * set this thread back to runnable before
				 * schedule is allowed to block.
				 */

				set_current_state(TASK_INTERRUPTIBLE);
				if (!kthread_should_stop())
					schedule();

				set_current_state(TASK_RUNNING);
			}

			continue;
		}

		node->msg_size = retval;
		channel = node->channel_index;

		/* Find the open file with the correct matching ID. */
		read_lock(&s_open_files_lock);
		list_for_each_entry(entry, &s_open_files, open_files_list) {
			if (channel == entry->channel_index) {
				handler_found = 1;

				if (atomic_read(&entry->pending_msg_count) >
				    AOCC_MAX_PENDING_MSGS) {
					pr_err("Too many pending messages on channel %d",
					       channel);
					kfree(node);
					break;
				}

				/* append message to the list of messages. */
				write_lock(&entry->pending_msg_lock);
				list_add(&node->msg_list,
					 &entry->pending_aoc_messages);
				atomic_inc(&entry->pending_msg_count);
				write_unlock(&entry->pending_msg_lock);

				/* wake up anyone blocked on reading */
				wake_up(&entry->read_queue);
				break;
			}
		}
		read_unlock(&s_open_files_lock);

		if (!handler_found) {
			pr_warn("Could not find handler for channel %d",
				channel);
			kfree(node);
			continue;
		}
	}

	return 0;
}

static void aocc_send_cmd_msg(aoc_service *service_id, enum aoc_cmd_code code,
			      int channel_to_modify)
{
	struct aocc_channel_control_msg msg;

	msg.channel_index = 0;
	msg.command_code = code;
	msg.channel_to_modify = channel_to_modify;

	aoc_service_write(service_id, (char *)&msg, sizeof(msg), true);
}

/* File methods */
static int aocc_open(struct inode *inode, struct file *file);
static int aocc_release(struct inode *inode, struct file *file);
static ssize_t aocc_read(struct file *file, char __user *buf, size_t count,
			 loff_t *off);
static ssize_t aocc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *off);

static const struct file_operations fops = {
	.open = aocc_open,
	.release = aocc_release,
	.read = aocc_read,
	.write = aocc_write,

	.owner = THIS_MODULE,
};

static struct aoc_service_dev *service_for_inode(struct inode *inode)
{
	int minor = MINOR(inode->i_rdev);

	if (minor < 0 || minor >= AOCC_MAX_DEVICES)
		return NULL;

	if (aocc_devices[minor] == NULL)
		return NULL;

	return dev_get_drvdata(aocc_devices[minor]);
}

static char *aocc_devnode(struct device *dev, umode_t *mode)
{
	if (!mode || !dev)
		return NULL;

	if (dev->devt == aocc_major_dev)
		*mode = 0666;

	return NULL;
}

static int create_character_device(struct aoc_service_dev *dev)
{
	int i;

	for (i = 0; i < AOCC_MAX_DEVICES; i++) {
		if (aocc_devices[i] == NULL) {
			aocc_devices[i] =
				device_create(aocc_class, &dev->dev,
					      MKDEV(aocc_major, i), NULL,
					      "acd-%s", dev_name(&(dev->dev)));
			if (aocc_devices[i] == NULL) {
				pr_err("failed to create aoc channel device\n");
				return -EINVAL;
			}

			dev_set_drvdata(aocc_devices[i], dev);

			return 0;
		}
	}

	return -ENODEV;
}

static int aocc_open(struct inode *inode, struct file *file)
{
	struct file_prvdata *prvdata;
	aoc_service *s;
	int minor = MINOR(inode->i_rdev);

	pr_debug("attempt to open major:%d minor:%d\n", MAJOR(inode->i_rdev),
		 MINOR(inode->i_rdev));

	s = service_for_inode(inode);
	if (!s)
		return -ENODEV;

	prvdata = kmalloc(sizeof(struct file_prvdata), GFP_KERNEL);
	if (!prvdata) {
		clear_bit(minor, &opened_devices);
		return -ENOMEM;
	}

	prvdata->service = s;
	prvdata->device_index = minor;
	file->private_data = prvdata;

	/* Check if our simple allocation scheme has overflowed */
	if (atomic_read(&channel_index_counter) == 0) {
		pr_err("Too many channels have been opened.");
		return -EMFILE;
	}

	/* Allocate a unique index to represent this open file. */
	prvdata->channel_index = atomic_inc_return(&channel_index_counter);
	pr_info("New client with channel ID %d", prvdata->channel_index);

	/* Start a new empty message list for this channel's message queue. */
	INIT_LIST_HEAD(&prvdata->pending_aoc_messages);
	rwlock_init(&prvdata->pending_msg_lock);
	atomic_set(&prvdata->pending_msg_count, 0);

	/* Initialize the waitqueues if this was opened as blocking. */
	if ((file->f_flags & O_NONBLOCK) == 0)
		init_waitqueue_head(&prvdata->read_queue);

	/* Add this item to the open files list for the dispatcher thread. */
	write_lock(&s_open_files_lock);
	INIT_LIST_HEAD(&prvdata->open_files_list);
	list_add(&prvdata->open_files_list, &s_open_files);
	write_unlock(&s_open_files_lock);

	/*Send a message to AOC to register a new channel. */
	aocc_send_cmd_msg(s, AOCC_CMD_OPEN_CHANNEL, prvdata->channel_index);

	return 0;
}

static int aocc_release(struct inode *inode, struct file *file)
{
	struct file_prvdata *private = file->private_data;
	int scrapped = 0;
	struct aoc_message_node *entry;
	struct aoc_message_node *temp;

	if (!private)
		return -ENODEV;

	/*Remove this file from from the list of active channels. */
	write_lock(&s_open_files_lock);
	list_del(&private->open_files_list);
	write_unlock(&s_open_files_lock);

	/*Clear all pending messages. */
	write_lock(&private->pending_msg_lock);
	list_for_each_entry_safe(entry, temp, &private->pending_aoc_messages,
				 msg_list) {
		kfree(entry);
		scrapped++;
		atomic_dec(&private->pending_msg_count);
	}
	write_unlock(&private->pending_msg_lock);

	/*Send a message to AOC to close the channel. */
	aocc_send_cmd_msg(private->service, AOCC_CMD_CLOSE_CHANNEL,
			  private->channel_index);

	pr_info("Destroyed channel %d with %d unread messages",
		private->channel_index, scrapped);

	clear_bit(private->device_index, &opened_devices);
	kfree(private);
	file->private_data = NULL;

	return 0;
}

static int aocc_are_messages_pending(struct file_prvdata *private)
{
	int retval = atomic_read(&private->pending_msg_count);
	return retval;
}

static ssize_t aocc_read(struct file *file, char __user *buf, size_t count,
			 loff_t *off)
{
	struct file_prvdata *private = file->private_data;
	struct aoc_message_node *node = NULL;
	ssize_t retval = 0;

	/*Block while there are no messages pending. */
	while (!aocc_are_messages_pending(private)) {
		if (file->f_flags & O_NONBLOCK) {
			retval = -EAGAIN;
			goto out;
		}

		retval = wait_event_interruptible(
			private->read_queue,
			aocc_are_messages_pending(private));

		if (retval == -ERESTARTSYS) {
			/* If the wait was interrupted, we are probably
			 * being killed. Quit.
			 */
			retval = -EINTR;
			goto out;
		}
	}

	/* pop pending message from the list. */
	read_lock(&private->pending_msg_lock);
	node = list_first_entry_or_null(&private->pending_aoc_messages,
					struct aoc_message_node, msg_list);
	read_unlock(&private->pending_msg_lock);

	if (!node) {
		pr_err("No messages available.");
		goto out;
	}

	/* is message too big to fit into read buffer? */
	if (count < (node->msg_size - sizeof(int))) {
		retval = -EFBIG;
		goto out;
	}

	/* copy message payload to userspace, minus the channel ID */
	retval = copy_to_user(buf, node->payload, node->msg_size - sizeof(int));

	/* copy_to_user returns bytes that couldn't be copied */
	retval = node->msg_size - retval;

	write_lock(&private->pending_msg_lock);
	list_del(&node->msg_list);
	atomic_dec(&private->pending_msg_count);
	write_unlock(&private->pending_msg_lock);

out:
	kfree(node);
	return retval;
}

static ssize_t aocc_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *off)
{
	struct file_prvdata *private = file->private_data;
	bool should_block = ((file->f_flags & O_NONBLOCK) == 0);
	char *buffer;
	size_t leftover;
	ssize_t retval = 0;

	if (!private)
		return -ENODEV;

	buffer = kmalloc(count + sizeof(int), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	/*Prepend the appropriate channel index to the message. */
	((int *)buffer)[0] = private->channel_index;

	leftover = copy_from_user(buffer + sizeof(int), buf, count);
	if (leftover == 0) {
		retval = aoc_service_write(private->service, buffer,
					   count + sizeof(int), should_block);
	} else {
		retval = -ENOMEM;
	}

	kfree(buffer);
	return retval;
}

static int aocc_probe(struct aoc_service_dev *dev)
{
	int ret;

	pr_notice("probe service with name %s\n", dev_name(&dev->dev));

	ret = create_character_device(dev);

	s_demux_task = kthread_run(&aocc_demux_kthread, dev, "aocc_demux");

	if (IS_ERR(s_demux_task))
		ret = PTR_ERR(s_demux_task);

	return ret;
}

static int aocc_remove(struct aoc_service_dev *dev)
{
	int i;

	for (i = 0; i < AOCC_MAX_DEVICES; i++) {
		if (aocc_devices[i] && aocc_devices[i]->parent == &dev->dev) {
			pr_notice("remove service with name %s\n",
				  dev_name(&dev->dev));

			device_destroy(aocc_class, aocc_devices[i]->devt);
			aocc_devices[i] = NULL;
		}
	}

	kthread_stop(s_demux_task);

	return 0;
}

static void cleanup_resources(void)
{
	aoc_driver_unregister(&aoc_chan_driver);

	if (aocc_class) {
		class_destroy(aocc_class);
		aocc_class = NULL;
	}

	if (aocc_major >= 0) {
		unregister_chrdev(aocc_major, AOCC_CHARDEV_NAME);
		aocc_major = -1;
	}
}

static int __init aocc_init(void)
{
	pr_debug("driver init\n");

	aocc_major = register_chrdev(0, AOCC_CHARDEV_NAME, &fops);
	if (aocc_major < 0) {
		pr_err("Failed to register character major number\n");
		goto fail;
	}

	aocc_major_dev = MKDEV(aocc_major, 0);

	aocc_class = class_create(THIS_MODULE, AOCC_CHARDEV_NAME);
	if (!aocc_class) {
		pr_err("Failed to create class\n");
		goto fail;
	}

	aocc_class->devnode = aocc_devnode;

	aoc_driver_register(&aoc_chan_driver);

	rwlock_init(&s_open_files_lock);
	return 0;

fail:
	cleanup_resources();
	return -1;
}

static void __exit aocc_exit(void)
{
	pr_debug("driver exit\n");

	cleanup_resources();
}

module_init(aocc_init);
module_exit(aocc_exit);

MODULE_LICENSE("GPL v2");
