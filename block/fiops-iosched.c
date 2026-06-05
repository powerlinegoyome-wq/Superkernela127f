/*
 * FIOPS: Fair IO Pending Scheduler
 *
 * Copyright (C) 2012 Shaohua Li <shli@kernel.org>
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

struct fiops_data {
	struct list_head fifo_list[2];
	struct request *next_rq[2];
	unsigned int batched;
	unsigned int batching;
};

static void fiops_merged_requests(struct request_queue *q, struct request *rq,
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

static void fiops_add_request(struct request_queue *q, struct request *rq)
{
	struct fiops_data *fdata = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);

	if (fdata->next_rq[sync] == NULL)
		fdata->next_rq[sync] = rq;

	rq_set_fifo_time(rq, jiffies + (sync ? HZ/2 : 5*HZ));
	list_add_tail(&rq->queuelist, &fdata->fifo_list[sync]);
}

static struct request *
fiops_expired_request(struct fiops_data *fdata, int sync)
{
	struct request *rq;

	if (list_empty(&fdata->fifo_list[sync]))
		return NULL;

	rq = list_entry(fdata->fifo_list[sync].next, struct request, queuelist);
	if (time_after_eq(jiffies, rq_fifo_time(rq)))
		return rq;

	return NULL;
}

static struct request *
fiops_choose_request(struct fiops_data *fdata, int data_dir)
{
	struct request *rq = fdata->next_rq[data_dir];

	if (rq) {
		fdata->next_rq[data_dir] = list_entry_rq(rq->queuelist.next);
		if (list_empty(&rq->queuelist))
			fdata->next_rq[data_dir] = NULL;
	}

	return rq;
}

static struct request *
fiops_dispatch_request(struct fiops_data *fdata)
{
	struct request *rq = NULL;

	if (fdata->batched < fdata->batching) {
		rq = fiops_choose_request(fdata, 1);
		if (!rq)
			rq = fiops_choose_request(fdata, 0);
	} else {
		fdata->batched = 0;
		rq = fiops_expired_request(fdata, 1);
		if (!rq)
			rq = fiops_expired_request(fdata, 0);

		if (!rq) {
			rq = fiops_choose_request(fdata, 1);
			if (!rq)
				rq = fiops_choose_request(fdata, 0);
		}
	}

	if (rq) {
		fdata->batched++;
		list_del_init(&rq->queuelist);
	}

	return rq;
}

static int fiops_dispatch(struct request_queue *q, int force)
{
	struct fiops_data *fdata = q->elevator->elevator_data;
	struct request *rq;

	rq = fiops_dispatch_request(fdata);
	if (rq) {
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static int fiops_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct fiops_data *fdata;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	fdata = kmalloc_node(sizeof(*fdata), GFP_KERNEL, q->node);
	if (!fdata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = fdata;

	INIT_LIST_HEAD(&fdata->fifo_list[0]);
	INIT_LIST_HEAD(&fdata->fifo_list[1]);
	fdata->next_rq[0] = NULL;
	fdata->next_rq[1] = NULL;
	fdata->batched = 0;
	fdata->batching = 16;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void fiops_exit_queue(struct elevator_queue *e)
{
	struct fiops_data *fdata = e->elevator_data;

	BUG_ON(!list_empty(&fdata->fifo_list[0]));
	BUG_ON(!list_empty(&fdata->fifo_list[1]));
	kfree(fdata);
}

static struct elevator_type elevator_fiops = {
	.ops.sq = {
		.elevator_merge_req_fn		= fiops_merged_requests,
		.elevator_dispatch_fn		= fiops_dispatch,
		.elevator_add_req_fn		= fiops_add_request,
		.elevator_init_fn		= fiops_init_queue,
		.elevator_exit_fn		= fiops_exit_queue,
	},
	.elevator_name = "fiops",
	.elevator_owner = THIS_MODULE,
};

static int __init fiops_init(void)
{
	return elv_register(&elevator_fiops);
}

static void __exit fiops_exit(void)
{
	elv_unregister(&elevator_fiops);
}

module_init(fiops_init);
module_exit(fiops_exit);

MODULE_AUTHOR("Shaohua Li");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FIOPS IO scheduler");
