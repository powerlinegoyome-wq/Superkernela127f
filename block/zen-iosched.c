/*
 * Zen IO scheduler
 * Based on Noop, Deadline and FIOPS.
 *
 * Copyright (C) 2013-2015 Brandon Hurley
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

static const int sync_expire = HZ / 2;
static const int async_expire = 5 * HZ;

struct zen_data {
	struct list_head fifo_list[2];
	struct request *next_rq[2];
	unsigned int batched;
	unsigned int batching;
};

static void zen_merged_requests(struct request_queue *q, struct request *rq,
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

static void zen_add_request(struct request_queue *q, struct request *rq)
{
	struct zen_data *zdata = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);

	if (zdata->next_rq[sync] == NULL)
		zdata->next_rq[sync] = rq;

	rq_set_fifo_time(rq, jiffies + (sync ? sync_expire : async_expire));
	list_add_tail(&rq->queuelist, &zdata->fifo_list[sync]);
}

static struct request *
zen_expired_request(struct zen_data *zdata, int sync)
{
	struct request *rq;

	if (list_empty(&zdata->fifo_list[sync]))
		return NULL;

	rq = list_entry(zdata->fifo_list[sync].next, struct request, queuelist);
	if (time_after_eq(jiffies, rq_fifo_time(rq)))
		return rq;

	return NULL;
}

static struct request *
zen_choose_request(struct zen_data *zdata, int data_dir)
{
	struct request *rq = zdata->next_rq[data_dir];

	if (rq) {
		zdata->next_rq[data_dir] = list_entry_rq(rq->queuelist.next);
		if (list_empty(&rq->queuelist))
			zdata->next_rq[data_dir] = NULL;
	}

	return rq;
}

static struct request *
zen_dispatch_request(struct zen_data *zdata)
{
	struct request *rq = NULL;

	if (zdata->batched < zdata->batching) {
		rq = zen_choose_request(zdata, 1); /* SYNC first */
		if (!rq)
			rq = zen_choose_request(zdata, 0); /* then ASYNC */
	} else {
		zdata->batched = 0;
		rq = zen_expired_request(zdata, 1);
		if (!rq)
			rq = zen_expired_request(zdata, 0);

		if (!rq) {
			rq = zen_choose_request(zdata, 1);
			if (!rq)
				rq = zen_choose_request(zdata, 0);
		}
	}

	if (rq) {
		zdata->batched++;
		list_del_init(&rq->queuelist);
	}

	return rq;
}

static int zen_dispatch(struct request_queue *q, int force)
{
	struct zen_data *zdata = q->elevator->elevator_data;
	struct request *rq;
	
	rq = zen_dispatch_request(zdata);
	if (rq) {
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static int zen_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct zen_data *zdata;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	zdata = kmalloc_node(sizeof(*zdata), GFP_KERNEL, q->node);
	if (!zdata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = zdata;

	INIT_LIST_HEAD(&zdata->fifo_list[0]);
	INIT_LIST_HEAD(&zdata->fifo_list[1]);
	zdata->next_rq[0] = NULL;
	zdata->next_rq[1] = NULL;
	zdata->batched = 0;
	zdata->batching = 32;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void zen_exit_queue(struct elevator_queue *e)
{
	struct zen_data *zdata = e->elevator_data;

	BUG_ON(!list_empty(&zdata->fifo_list[0]));
	BUG_ON(!list_empty(&zdata->fifo_list[1]));
	kfree(zdata);
}

static struct elevator_type elevator_zen = {
	.ops.sq = {
		.elevator_merge_req_fn		= zen_merged_requests,
		.elevator_dispatch_fn		= zen_dispatch,
		.elevator_add_req_fn		= zen_add_request,
		.elevator_init_fn		= zen_init_queue,
		.elevator_exit_fn		= zen_exit_queue,
	},
	.elevator_name = "zen",
	.elevator_owner = THIS_MODULE,
};

static int __init zen_init(void)
{
	return elv_register(&elevator_zen);
}

static void __exit zen_exit(void)
{
	elv_unregister(&elevator_zen);
}

module_init(zen_init);
module_exit(zen_exit);

MODULE_AUTHOR("Brandon Hurley");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zen IO scheduler");
