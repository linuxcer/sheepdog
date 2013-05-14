/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * This code is based on bs.c from Linux target framework (tgt):
 *   Copyright (C) 2007 FUJITA Tomonori <tomof@acm.org>
 *   Copyright (C) 2007 Mike Christie <michaelc@cs.wisc.edu>
 */
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syscall.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/time.h>
#include <linux/types.h>

#include "list.h"
#include "util.h"
#include "work.h"
#include "logger.h"
#include "event.h"

/*
 * The protection period from shrinking work queue.  This is necessary
 * to avoid many calls of pthread_create.  Without it, threads are
 * frequently created and deleted and it leads poor performance.
 */
#define WQ_PROTECTION_PERIOD 1000 /* ms */

struct worker_info {
	const char *name;

	struct list_head finished_list;
	struct list_head worker_info_siblings;

	pthread_mutex_t finished_lock;
	pthread_mutex_t startup_lock;

	/* wokers sleep on this and signaled by tgtd */
	pthread_cond_t pending_cond;
	/* locked by tgtd and workers */
	pthread_mutex_t pending_lock;
	/* protected by pending_lock */
	struct work_queue q;
	size_t nr_pending;
	size_t nr_running;
	size_t nr_threads;
	/* we cannot shrink work queue till this time */
	uint64_t tm_end_of_protection;
	enum wq_thread_control tc;
};

static int efd;
static LIST_HEAD(worker_info_list);
static size_t nr_nodes = 1;
static size_t (*wq_get_nr_nodes)(void);
static void (*wq_create_cb)(pthread_t);
static void (*wq_destroy_cb)(pthread_t);

static void *worker_routine(void *arg);

static uint64_t get_msec_time(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static inline uint64_t wq_get_roof(struct worker_info *wi)
{
	uint64_t nr = 1;

	switch (wi->tc) {
	case WQ_ORDERED:
		break;
	case WQ_DYNAMIC:
		/* FIXME: 2 * nr_nodes threads. No rationale yet. */
		nr = nr_nodes * 2;
		break;
	case WQ_UNLIMITED:
		nr = SIZE_MAX;
		break;
	default:
		panic("Invalid threads control %d", wi->tc);
	}
	return nr;
}

static bool wq_need_grow(struct worker_info *wi)
{
	if (wi->nr_threads < wi->nr_pending + wi->nr_running &&
	    wi->nr_threads * 2 <= wq_get_roof(wi)) {
		wi->tm_end_of_protection = get_msec_time() +
			WQ_PROTECTION_PERIOD;
		return true;
	}

	return false;
}

/*
 * Return true if more than half of threads are not used more than
 * WQ_PROTECTION_PERIOD seconds
 */
static bool wq_need_shrink(struct worker_info *wi)
{
	if (wi->nr_pending + wi->nr_running <= wi->nr_threads / 2)
		/* we cannot shrink work queue during protection period. */
		return wi->tm_end_of_protection <= get_msec_time();

	/* update the end of protection time */
	wi->tm_end_of_protection = get_msec_time() + WQ_PROTECTION_PERIOD;
	return false;
}

static int create_worker_threads(struct worker_info *wi, size_t nr_threads)
{
	pthread_t thread;
	int ret;

	pthread_mutex_lock(&wi->startup_lock);
	while (wi->nr_threads < nr_threads) {
		ret = pthread_create(&thread, NULL, worker_routine, wi);
		if (ret != 0) {
			sd_eprintf("failed to create worker thread: %m");
			pthread_mutex_unlock(&wi->startup_lock);
			return -1;
		}
		if (wq_create_cb)
			wq_create_cb(thread);
		wi->nr_threads++;
		sd_dprintf("create thread %s %zu", wi->name, wi->nr_threads);
	}
	pthread_mutex_unlock(&wi->startup_lock);

	return 0;
}

void queue_work(struct work_queue *q, struct work *work)
{
	struct worker_info *wi = container_of(q, struct worker_info, q);

	pthread_mutex_lock(&wi->pending_lock);
	wi->nr_pending++;

	if (wq_need_grow(wi))
		/* double the thread pool size */
		create_worker_threads(wi, wi->nr_threads * 2);

	list_add_tail(&work->w_list, &wi->q.pending_list);
	pthread_mutex_unlock(&wi->pending_lock);

	pthread_cond_signal(&wi->pending_cond);
}

static void worker_thread_request_done(int fd, int events, void *data)
{
	int ret;
	struct worker_info *wi;
	struct work *work;
	eventfd_t value;
	LIST_HEAD(list);

	if (wq_get_nr_nodes)
		nr_nodes = wq_get_nr_nodes();

	ret = eventfd_read(fd, &value);
	if (ret < 0)
		return;

	list_for_each_entry(wi, &worker_info_list, worker_info_siblings) {
		pthread_mutex_lock(&wi->finished_lock);
		list_splice_init(&wi->finished_list, &list);
		pthread_mutex_unlock(&wi->finished_lock);

		while (!list_empty(&list)) {
			work = list_first_entry(&list, struct work, w_list);
			list_del(&work->w_list);

			work->done(work);
		}
	}
}

static void *worker_routine(void *arg)
{
	struct worker_info *wi = arg;
	struct work *work;
	eventfd_t value = 1;

	set_thread_name(wi->name, (wi->tc != WQ_ORDERED));

	pthread_mutex_lock(&wi->startup_lock);
	/* started this thread */
	pthread_mutex_unlock(&wi->startup_lock);

	pthread_mutex_lock(&wi->pending_lock);
	wi->nr_running++;
	pthread_mutex_unlock(&wi->pending_lock);

	while (true) {

		pthread_mutex_lock(&wi->pending_lock);
		if (wq_need_shrink(wi)) {
			wi->nr_running--;
			wi->nr_threads--;
			if (wq_destroy_cb)
				wq_destroy_cb(pthread_self());
			pthread_mutex_unlock(&wi->pending_lock);
			pthread_detach(pthread_self());
			sd_dprintf("destroy thread %s %d, %zu", wi->name,
				   gettid(), wi->nr_threads);
			break;
		}
retest:
		if (list_empty(&wi->q.pending_list)) {
			wi->nr_running--;
			pthread_cond_wait(&wi->pending_cond, &wi->pending_lock);
			wi->nr_running++;
			goto retest;
		}

		wi->nr_pending--;
		work = list_first_entry(&wi->q.pending_list,
				       struct work, w_list);

		list_del(&work->w_list);
		pthread_mutex_unlock(&wi->pending_lock);

		if (work->fn)
			work->fn(work);

		pthread_mutex_lock(&wi->finished_lock);
		list_add_tail(&work->w_list, &wi->finished_list);
		pthread_mutex_unlock(&wi->finished_lock);

		eventfd_write(efd, value);
	}

	pthread_exit(NULL);
}

int init_work_queue(size_t (*get_nr_nodes)(void), void (*create_cb)(pthread_t),
		    void (*destroy_cb)(pthread_t))
{
	int ret;

	wq_get_nr_nodes = get_nr_nodes;
	wq_create_cb = create_cb;
	wq_destroy_cb = destroy_cb;

	if (wq_get_nr_nodes)
		nr_nodes = wq_get_nr_nodes();

	efd = eventfd(0, EFD_NONBLOCK);
	if (efd < 0) {
		sd_eprintf("failed to create an event fd: %m");
		return 1;
	}

	ret = register_event(efd, worker_thread_request_done, NULL);
	if (ret) {
		sd_eprintf("failed to register event fd %m");
		close(efd);
		return 1;
	}

	return 0;
}

/*
 * Allowing unlimited threads to be created is necessary to solve the following
 * problems:
 *
 *  1. timeout of IO requests from guests. With on-demand short threads, we
 *     guarantee that there is always one thread available to execute the
 *     request as soon as possible.
 *  2. sheep halt for corner case that all gateway and io threads are executing
 *     local requests that ask for creation of another thread to execute the
 *     requests and sleep-wait for responses.
 */
struct work_queue *create_work_queue(const char *name,
				     enum wq_thread_control tc)
{
	int ret;
	struct worker_info *wi;

	wi = xzalloc(sizeof(*wi));
	wi->name = name;
	wi->tc = tc;

	INIT_LIST_HEAD(&wi->q.pending_list);
	INIT_LIST_HEAD(&wi->finished_list);

	pthread_cond_init(&wi->pending_cond, NULL);

	pthread_mutex_init(&wi->finished_lock, NULL);
	pthread_mutex_init(&wi->pending_lock, NULL);
	pthread_mutex_init(&wi->startup_lock, NULL);

	ret = create_worker_threads(wi, 1);
	if (ret < 0)
		goto destroy_threads;

	list_add(&wi->worker_info_siblings, &worker_info_list);

	return &wi->q;
destroy_threads:
	pthread_mutex_unlock(&wi->startup_lock);
	pthread_cond_destroy(&wi->pending_cond);
	pthread_mutex_destroy(&wi->pending_lock);
	pthread_mutex_destroy(&wi->startup_lock);
	pthread_mutex_destroy(&wi->finished_lock);

	return NULL;
}

struct work_queue *create_ordered_work_queue(const char *name)
{
	return create_work_queue(name, WQ_ORDERED);
}

bool work_queue_empty(struct work_queue *q)
{
	struct worker_info *wi = container_of(q, struct worker_info, q);
	size_t nr_works;

	pthread_mutex_lock(&wi->pending_lock);
	nr_works = wi->nr_running + wi->nr_pending;
	pthread_mutex_unlock(&wi->pending_lock);

	return nr_works == 0;
}
