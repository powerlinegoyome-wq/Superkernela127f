/*
 * Maple IO scheduler
 * Based on Zen and FIOPS.
 *
 * Copyright (C) 2014-2016 Joe Maples
 * Adapted for 4.19+ kernels
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>

static inline unsigned long rq_fifo_time(const struct request *rq)
{
	return (unsigned long)rq->fifo_time;
}

static inline void rq_set_fifo_time(struct request *rq, unsigned long time)
{
	rq->fifo_time = (u64)time;
}

static const int sync_read_expire = HZ / 2;
static const int sync_write_expire = HZ;
static const int async_read_expire = 4 * HZ;
static const int async_write_expire = 5 * HZ;

struct maple_data {
	struct list_head fifo_list[4]; /* 0: SR, 1: SW, 2: AR, 3: AW */
	struct request *next_rq[4];
	unsigned int batched;
	unsigned int batching;
};

static inline int rq_dir_index(struct request *rq)
{
	int sync = rq_is_sync(rq);
	int write = rq_data_dir(rq);
	if (sync)
		return write ? 1 : 0;
	else
		return write ? 3 : 2;
}

static void maple_merged_requests(struct request_queue *q, struct request *rq,
				struct request *next)
{
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before(rq_fifo_time(next), rq_fifo_time(rq))) {
			list_move(&rq->queuelist, &next->queuelist);
			rq_set_fifo_time(rq, rq_fifo_time(next));
		}
	}
	list_del_init(&next->queuelist);
}

static void maple_add_request(struct request_queue *q, struct request *rq)
{
	struct maple_data *mdata = q->elevator->elevator_data;
	int dir = rq_dir_index(rq);
	unsigned long expire_time;

	if (mdata->next_rq[dir] == NULL)
		mdata->next_rq[dir] = rq;

	switch(dir) {
	case 0: expire_time = sync_read_expire; break;
	case 1: expire_time = sync_write_expire; break;
	case 2: expire_time = async_read_expire; break;
	case 3: expire_time = async_write_expire; break;
	default: expire_time = sync_read_expire; break;
	}

	rq_set_fifo_time(rq, jiffies + expire_time);
	list_add_tail(&rq->queuelist, &mdata->fifo_list[dir]);
}

static struct request *
maple_expired_request(struct maple_data *mdata, int dir)
{
	struct request *rq;

	if (list_empty(&mdata->fifo_list[dir]))
		return NULL;

	rq = list_entry(mdata->fifo_list[dir].next, struct request, queuelist);
	if (time_after_eq(jiffies, rq_fifo_time(rq)))
		return rq;

	return NULL;
}

static struct request *
maple_choose_request(struct maple_data *mdata, int dir)
{
	struct request *rq = mdata->next_rq[dir];

	if (rq) {
		mdata->next_rq[dir] = list_entry_rq(rq->queuelist.next);
		if (list_empty(&rq->queuelist))
			mdata->next_rq[dir] = NULL;
	}

	return rq;
}

static struct request *
maple_dispatch_request(struct maple_data *mdata)
{
	struct request *rq = NULL;
	int i;

	if (mdata->batched < mdata->batching) {
		for (i = 0; i < 4; i++) {
			rq = maple_choose_request(mdata, i);
			if (rq) break;
		}
	} else {
		mdata->batched = 0;
		for (i = 0; i < 4; i++) {
			rq = maple_expired_request(mdata, i);
			if (rq) break;
		}
		if (!rq) {
			for (i = 0; i < 4; i++) {
				rq = maple_choose_request(mdata, i);
				if (rq) break;
			}
		}
	}

	if (rq) {
		mdata->batched++;
		list_del_init(&rq->queuelist);
	}

	return rq;
}

static int maple_dispatch(struct request_queue *q, int force)
{
	struct maple_data *mdata = q->elevator->elevator_data;
	struct request *rq;

	rq = maple_dispatch_request(mdata);
	if (rq) {
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static int maple_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct maple_data *mdata;
	struct elevator_queue *eq;
	int i;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	mdata = kmalloc_node(sizeof(*mdata), GFP_KERNEL, q->node);
	if (!mdata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = mdata;

	for (i = 0; i < 4; i++) {
		INIT_LIST_HEAD(&mdata->fifo_list[i]);
		mdata->next_rq[i] = NULL;
	}
	mdata->batched = 0;
	mdata->batching = 64;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void maple_exit_queue(struct elevator_queue *e)
{
	struct maple_data *mdata = e->elevator_data;
	int i;

	for (i = 0; i < 4; i++)
		BUG_ON(!list_empty(&mdata->fifo_list[i]));
	kfree(mdata);
}

static struct elevator_type elevator_maple = {
	.ops.sq = {
		.elevator_merge_req_fn		= maple_merged_requests,
		.elevator_dispatch_fn		= maple_dispatch,
		.elevator_add_req_fn		= maple_add_request,
		.elevator_init_fn		= maple_init_queue,
		.elevator_exit_fn		= maple_exit_queue,
	},
	.elevator_name = "maple",
	.elevator_owner = THIS_MODULE,
};

static int __init maple_init(void)
{
	return elv_register(&elevator_maple);
}

static void __exit maple_exit(void)
{
	elv_unregister(&elevator_maple);
}

module_init(maple_init);
module_exit(maple_exit);

MODULE_AUTHOR("Joe Maples");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Maple IO scheduler");
