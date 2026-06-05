/*
 * Dynamic Fsync for Linux
 *
 * Copyright (C) 2012-2014 faux123 <reioux@gmail.com>
 * Adapted for 4.19 kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>
#include <linux/writeback.h>
#include <linux/fb.h>

#define DYN_FSYNC_VERSION_MAJOR 1
#define DYN_FSYNC_VERSION_MINOR 5

/*
 * fsync_enabled: 1 = fsync active (normal), 0 = fsync deferred
 * dyn_fsync_active: 1 = dynamic fsync enabled
 */
bool dyn_fsync_active __read_mostly = true;
EXPORT_SYMBOL(dyn_fsync_active);

static bool suspend_active __read_mostly = false;
static struct mutex fsync_mutex;
static struct notifier_block notif;

static ssize_t dyn_fsync_active_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (dyn_fsync_active ? 1 : 0));
}

static ssize_t dyn_fsync_active_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		if (data == 1) {
			pr_info("%s: dynamic fsync enabled\n", __func__);
			dyn_fsync_active = true;
		} else if (data == 0) {
			pr_info("%s: dynamic fsync disabled\n", __func__);
			dyn_fsync_active = false;
		} else {
			pr_info("%s: bad value: %u\n", __func__, data);
		}
	} else {
		pr_info("%s: unknown input!\n", __func__);
	}

	return count;
}

static ssize_t dyn_fsync_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %u.%u\n",
		DYN_FSYNC_VERSION_MAJOR,
		DYN_FSYNC_VERSION_MINOR);
}

static struct kobj_attribute dyn_fsync_active_attr =
	__ATTR(Dyn_fsync_active, 0664,
		dyn_fsync_active_show,
		dyn_fsync_active_store);

static struct kobj_attribute dyn_fsync_version_attr =
	__ATTR(Dyn_fsync_version, 0444,
		dyn_fsync_version_show,
		NULL);

static struct attribute *dyn_fsync_active_attrs[] = {
	&dyn_fsync_active_attr.attr,
	&dyn_fsync_version_attr.attr,
	NULL,
};

static struct attribute_group dyn_fsync_active_attr_group = {
	.attrs = dyn_fsync_active_attrs,
};

static struct kobject *dyn_fsync_kobj;

static void dyn_fsync_force_flush(void)
{
	/* Force sync all filesystems */
	ksys_sync();
}

static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (!dyn_fsync_active)
		return NOTIFY_OK;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
		case FB_BLANK_UNBLANK:
			/* Screen on - fsync deferred */
			mutex_lock(&fsync_mutex);
			suspend_active = false;
			mutex_unlock(&fsync_mutex);
			break;
		case FB_BLANK_POWERDOWN:
			/* Screen off - flush pending writes */
			mutex_lock(&fsync_mutex);
			suspend_active = true;
			dyn_fsync_force_flush();
			mutex_unlock(&fsync_mutex);
			break;
		default:
			break;
		}
	}

	return NOTIFY_OK;
}

static int dyn_fsync_panic_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	/* On panic, force sync everything */
	suspend_active = false;
	dyn_fsync_force_flush();
	return NOTIFY_DONE;
}

static struct notifier_block dyn_fsync_panic_block = {
	.notifier_call  = dyn_fsync_panic_event,
	.priority       = INT_MAX,
};

static int dyn_fsync_reboot_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	/* On reboot, force sync everything */
	suspend_active = false;
	dyn_fsync_force_flush();
	return NOTIFY_DONE;
}

static struct notifier_block dyn_fsync_reboot_block = {
	.notifier_call  = dyn_fsync_reboot_event,
	.priority       = INT_MAX,
};

static int __init dynamic_fsync_init(void)
{
	int ret;

	mutex_init(&fsync_mutex);

	dyn_fsync_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);
	if (!dyn_fsync_kobj) {
		pr_err("%s: kobject create failed!\n", __func__);
		return -ENOMEM;
	}

	ret = sysfs_create_group(dyn_fsync_kobj, &dyn_fsync_active_attr_group);
	if (ret) {
		pr_err("%s: sysfs create failed!\n", __func__);
		kobject_put(dyn_fsync_kobj);
		return ret;
	}

	/* Register framebuffer notifier */
	notif.notifier_call = fb_notifier_callback;
	ret = fb_register_client(&notif);
	if (ret) {
		pr_err("%s: fb register failed!\n", __func__);
	}

	/* Register panic and reboot notifiers for safety */
	atomic_notifier_chain_register(&panic_notifier_list,
		&dyn_fsync_panic_block);
	register_reboot_notifier(&dyn_fsync_reboot_block);

	pr_info("%s: dynamic fsync v%u.%u initialized\n", __func__,
		DYN_FSYNC_VERSION_MAJOR, DYN_FSYNC_VERSION_MINOR);

	return 0;
}

static void __exit dynamic_fsync_exit(void)
{
	fb_unregister_client(&notif);
	atomic_notifier_chain_unregister(&panic_notifier_list,
		&dyn_fsync_panic_block);
	unregister_reboot_notifier(&dyn_fsync_reboot_block);

	if (dyn_fsync_kobj) {
		sysfs_remove_group(dyn_fsync_kobj,
			&dyn_fsync_active_attr_group);
		kobject_put(dyn_fsync_kobj);
	}
}

module_init(dynamic_fsync_init);
module_exit(dynamic_fsync_exit);

MODULE_AUTHOR("faux123 <reioux@gmail.com>");
MODULE_DESCRIPTION("Dynamic Fsync - screen-aware sync control");
MODULE_LICENSE("GPL v2");
