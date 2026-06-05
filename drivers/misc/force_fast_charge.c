/*
 * Force Fast Charge for Samsung devices
 *
 * Copyright (C) 2013-2015 faux123 <reioux@gmail.com>
 * Adapted for 4.19 kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

bool force_fast_charge = true;
EXPORT_SYMBOL(force_fast_charge);

static ssize_t force_fast_charge_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (force_fast_charge ? 1 : 0));
}

static ssize_t force_fast_charge_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		if (data == 1) {
			pr_info("%s: force fast charge enabled\n", __func__);
			force_fast_charge = true;
		} else if (data == 0) {
			pr_info("%s: force fast charge disabled\n", __func__);
			force_fast_charge = false;
		} else {
			pr_info("%s: bad value: %u\n", __func__, data);
		}
	} else {
		pr_info("%s: unknown input!\n", __func__);
	}

	return count;
}

static struct kobj_attribute force_fast_charge_attr =
	__ATTR(force_fast_charge, 0664,
		force_fast_charge_show,
		force_fast_charge_store);

static struct attribute *force_fast_charge_attrs[] = {
	&force_fast_charge_attr.attr,
	NULL,
};

static struct attribute_group force_fast_charge_attr_group = {
	.attrs = force_fast_charge_attrs,
};

static struct kobject *force_fast_charge_kobj;

static int __init force_fast_charge_init(void)
{
	int ret;

	force_fast_charge_kobj = kobject_create_and_add("fast_charge", kernel_kobj);
	if (!force_fast_charge_kobj) {
		pr_err("%s: kobject create failed!\n", __func__);
		return -ENOMEM;
	}

	ret = sysfs_create_group(force_fast_charge_kobj, &force_fast_charge_attr_group);
	if (ret) {
		pr_err("%s: sysfs create failed!\n", __func__);
		kobject_put(force_fast_charge_kobj);
		return ret;
	}

	pr_info("%s: initialized\n", __func__);

	return 0;
}

static void __exit force_fast_charge_exit(void)
{
	if (force_fast_charge_kobj) {
		sysfs_remove_group(force_fast_charge_kobj,
			&force_fast_charge_attr_group);
		kobject_put(force_fast_charge_kobj);
	}
}

module_init(force_fast_charge_init);
module_exit(force_fast_charge_exit);

MODULE_AUTHOR("faux123 <reioux@gmail.com>");
MODULE_DESCRIPTION("Force Fast Charge Driver");
MODULE_LICENSE("GPL v2");
