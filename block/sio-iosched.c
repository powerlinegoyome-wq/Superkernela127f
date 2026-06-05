/*
 * Simple IO scheduler
 * Based on Noop, Deadline and V(R) IO schedulers.
 *
 * Copyright (C) 2012 Miguel Boton <mboton@gmail.com>
 * Adapted for newer kernels.
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

enum sio_data_dir {
	ASYNC,
	SYNC,
};

static inline unsigned long rq_fifo_time(const struct request *rq)
{
	return (unsigned long)rq->fifo_time;
}

static inline void rq_set_fifo_time(struct request *rq, unsigned long time)
{
	rq->fifo_time = (u64)time;
}

static const int sync_expire  = HZ / 2;    /* max time before a sync is submitted. */
static const int async_expire = 5 * HZ;    /* ditto for async, these limits are SOFT! */

struct sio_data {
	struct list_head fifo_list[2];
	struct request *next_rq[2];
	unsigned int batched;
	unsigned int batching;
};

static void sio_merged_requests(struct request_queue *q, struct request *rq,
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

static void sio_add_request(struct request_queue *q, struct request *rq)
{
	struct sio_data *sd = q->elevator->elevator_data;
	const int sync = rq_is_sync(rq);

	if (sd->next_rq[sync] == NULL)
		sd->next_rq[sync] = rq;

	if (sync)
		rq_set_fifo_time(rq, jiffies + sync_expire);
	else
		rq_set_fifo_time(rq, jiffies + async_expire);

	list_add_tail(&rq->queuelist, &sd->fifo_list[sync]);
}

static struct request *
sio_expired_request(struct sio_data *sd, int sync)
{
	struct request *rq;

	if (list_empty(&sd->fifo_list[sync]))
		return NULL;

	rq = list_entry(sd->fifo_list[sync].next, struct request, queuelist);
	if (time_after(jiffies, rq_fifo_time(rq)))
		return rq;

	return NULL;
}

static struct request *
sio_choose_request(struct sio_data *sd, int data_dir)
{
	struct request *rq = sd->next_rq[data_dir];

	if (rq) {
		sd->next_rq[data_dir] = list_entry_rq(rq->queuelist.next);
		if (list_empty(&rq->queuelist))
			sd->next_rq[data_dir] = NULL;
	}

	return rq;
}

static struct request *
sio_dispatch_request(struct sio_data *sd)
{
	struct request *rq = NULL;
	int data_dir = SYNC;

	if (sd->batched < sd->batching) {
		rq = sio_choose_request(sd, data_dir);
		if (!rq) {
			data_dir = ASYNC;
			rq = sio_choose_request(sd, data_dir);
		}
	} else {
		sd->batched = 0;

		rq = sio_expired_request(sd, SYNC);
		if (!rq) {
			rq = sio_expired_request(sd, ASYNC);
			if (rq)
				data_dir = ASYNC;
		}

		if (!rq) {
			rq = sio_choose_request(sd, data_dir);
			if (!rq) {
				data_dir = ASYNC;
				rq = sio_choose_request(sd, data_dir);
			}
		}
	}

	if (rq) {
		sd->batched++;
		list_del_init(&rq->queuelist);
		return rq;
	}

	return NULL;
}

static int sio_dispatch(struct request_queue *q, int force)
{
	struct sio_data *sd = q->elevator->elevator_data;
	struct request *rq;

	rq = sio_dispatch_request(sd);
	if (rq) {
		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

static int sio_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct sio_data *sd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	sd = kmalloc_node(sizeof(*sd), GFP_KERNEL, q->node);
	if (!sd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = sd;

	INIT_LIST_HEAD(&sd->fifo_list[SYNC]);
	INIT_LIST_HEAD(&sd->fifo_list[ASYNC]);
	sd->next_rq[SYNC] = NULL;
	sd->next_rq[ASYNC] = NULL;
	sd->batched = 0;
	sd->batching = 16;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void sio_exit_queue(struct elevator_queue *e)
{
	struct sio_data *sd = e->elevator_data;

	BUG_ON(!list_empty(&sd->fifo_list[SYNC]));
	BUG_ON(!list_empty(&sd->fifo_list[ASYNC]));
	kfree(sd);
}

static struct elevator_type elevator_sio = {
	.ops.sq = {
		.elevator_merge_req_fn		= sio_merged_requests,
		.elevator_dispatch_fn		= sio_dispatch,
		.elevator_add_req_fn		= sio_add_request,
		.elevator_init_fn		= sio_init_queue,
		.elevator_exit_fn		= sio_exit_queue,
	},
	.elevator_name = "sio",
	.elevator_owner = THIS_MODULE,
};

static int __init sio_init(void)
{
	return elv_register(&elevator_sio);
}

static void __exit sio_exit(void)
{
	elv_unregister(&elevator_sio);
}

module_init(sio_init);
module_exit(sio_exit);

MODULE_AUTHOR("Miguel Boton");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple IO scheduler");
