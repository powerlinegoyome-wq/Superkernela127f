/*
 * Boeffla Wakelock Blocker
 *
 * Copyright (C) 2013-2015 Lord Boeffla
 * Adapted for 4.19 kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/boeffla_wl_blocker.h>

#define MAX_BLOCKER_LENGTH 1024
#define DELIM ";"

static char wakelock_blocker_list[MAX_BLOCKER_LENGTH];
static DEFINE_MUTEX(wakelock_blocker_mutex);

bool boeffla_wl_blocker_active(const char *name)
{
	bool is_blocked = false;
	char *list_copy;
	char *token, *rest;

	if (!name || strlen(name) == 0 || strlen(wakelock_blocker_list) == 0)
		return false;

	mutex_lock(&wakelock_blocker_mutex);

	list_copy = kstrdup(wakelock_blocker_list, GFP_KERNEL);
	if (!list_copy) {
		mutex_unlock(&wakelock_blocker_mutex);
		return false;
	}

	rest = list_copy;
	while ((token = strsep(&rest, DELIM)) != NULL) {
		if (strlen(token) > 0 && strcmp(name, token) == 0) {
			is_blocked = true;
			break;
		}
	}

	kfree(list_copy);
	mutex_unlock(&wakelock_blocker_mutex);

	return is_blocked;
}
EXPORT_SYMBOL(boeffla_wl_blocker_active);

static ssize_t wakelock_blocker_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	ssize_t ret;
	
	mutex_lock(&wakelock_blocker_mutex);
	ret = sprintf(buf, "%s\n", wakelock_blocker_list);
	mutex_unlock(&wakelock_blocker_mutex);
	
	return ret;
}

static ssize_t wakelock_blocker_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	if (count >= MAX_BLOCKER_LENGTH)
		return -EINVAL;

	mutex_lock(&wakelock_blocker_mutex);
	
	memset(wakelock_blocker_list, 0, MAX_BLOCKER_LENGTH);
	strncpy(wakelock_blocker_list, buf, count);
	
	/* Strip newline */
	if (count > 0 && wakelock_blocker_list[count-1] == '\n')
		wakelock_blocker_list[count-1] = '\0';

	pr_info("boeffla_wl_blocker: updated list to: %s\n", wakelock_blocker_list);
	
	mutex_unlock(&wakelock_blocker_mutex);

	return count;
}

static struct kobj_attribute wakelock_blocker_attr =
	__ATTR(wakelock_blocker, 0664,
		wakelock_blocker_show,
		wakelock_blocker_store);

static struct attribute *wakelock_blocker_attrs[] = {
	&wakelock_blocker_attr.attr,
	NULL,
};

static struct attribute_group wakelock_blocker_attr_group = {
	.attrs = wakelock_blocker_attrs,
};

static struct kobject *boeffla_wl_blocker_kobj;

static int __init boeffla_wl_blocker_init(void)
{
	int ret;

	/* Default blocklist */
	memset(wakelock_blocker_list, 0, MAX_BLOCKER_LENGTH);
	/* strcpy(wakelock_blocker_list, "qcom_rx_wakelock;wlan;wlan_extscan_wl"); */

	boeffla_wl_blocker_kobj = kobject_create_and_add("boeffla_wakelock_blocker", kernel_kobj);
	if (!boeffla_wl_blocker_kobj) {
		pr_err("boeffla_wl_blocker: kobject create failed!\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(boeffla_wl_blocker_kobj, &wakelock_blocker_attr_group);
	if (ret) {
		pr_err("boeffla_wl_blocker: sysfs create failed!\n");
		kobject_put(boeffla_wl_blocker_kobj);
		return ret;
	}

	pr_info("boeffla_wl_blocker: initialized\n");

	return 0;
}

static void __exit boeffla_wl_blocker_exit(void)
{
	if (boeffla_wl_blocker_kobj) {
		sysfs_remove_group(boeffla_wl_blocker_kobj,
			&wakelock_blocker_attr_group);
		kobject_put(boeffla_wl_blocker_kobj);
	}
}

module_init(boeffla_wl_blocker_init);
module_exit(boeffla_wl_blocker_exit);

MODULE_AUTHOR("Lord Boeffla");
MODULE_DESCRIPTION("Boeffla Wakelock Blocker");
MODULE_LICENSE("GPL v2");
