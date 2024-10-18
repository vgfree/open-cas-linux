/*
* Copyright(c) 2012-2022 Intel Corporation
* Copyright(c) 2024 Huawei Technologies
* SPDX-License-Identifier: BSD-3-Clause
*/

#include "threads.h"
#include "cas_cache.h"

#define MAX_THREAD_NAME_SIZE 48

struct cas_thread_info {
	char name[MAX_THREAD_NAME_SIZE];
	void *sync_data;
	atomic_t stop;
	atomic_t kicked;
	struct completion compl;
	struct completion sync_compl;
	wait_queue_head_t wq;
	struct task_struct *thread;
};

static int queue_thread_run(void *data)
{
	ocf_queue_t q = data;
	struct cas_thread_info *info;

	BUG_ON(!q);

	/* complete the creation of the thread */
	info = ocf_queue_get_priv(q);
	BUG_ON(!info);

	CAS_DAEMONIZE(info->thread->comm);

	complete(&info->compl);

	/* Continue working until signaled to exit. */
	do {
		/* Wait until there are completed read misses from the HDDs,
		 * or a stop.
		 */
		wait_event_interruptible(info->wq, ocf_queue_pending_io(q) ||
				atomic_read(&info->stop));

		ocf_queue_run(q);

	} while (!atomic_read(&info->stop) || ocf_queue_pending_io(q));

	WARN(ocf_queue_pending_io(q), "Still pending IO requests\n");

	/* If we get here, then thread was signalled to terminate.
	 * So, let's complete and exit.
	 */
	CAS_COMPLETE_AND_EXIT(&info->compl, 0);

	return 0;
}

ocf_queue_t cache_get_fastest_porter_queue(ocf_cache_t cache)
{
	uint32_t cpus_no = num_online_cpus();
	struct cache_priv *cache_priv = ocf_cache_get_priv(cache);
	ocf_queue_t queue;
	ocf_queue_t min_queue;
	uint32_t min_io, cmp_io;
	int i;

	ENV_BUG_ON(!cpus_no);
	ENV_BUG_ON(!cache_priv);

	queue = cache_priv->queues[0].porter_queue;
	min_io = ocf_queue_pending_io(queue);
	min_queue = queue;

	for (i = 1; min_io && (i < cpus_no); i++) {
		queue = cache_priv->queues[i].porter_queue;
		cmp_io = ocf_queue_pending_io(queue);
		if (cmp_io < min_io) {
			min_io = cmp_io;
			min_queue = queue;
		}
	}
	return min_queue;
}

void cache_print_each_porter_queue_pending_io(ocf_cache_t cache)
{
	uint32_t cpus_no = num_online_cpus();
	struct cache_priv *cache_priv = ocf_cache_get_priv(cache);
	ocf_queue_t queue;
	uint32_t io;
	int i;

	ENV_BUG_ON(!cache_priv);

	for (i = 0; i < cpus_no; i++) {
		queue = cache_priv->queues[i].porter_queue;
		io = ocf_queue_pending_io(queue);
		printk(KERN_WARNING "Still pending %d IO requests at index %d in cache %s\n", io, i, ocf_cache_get_name(cache));
	}
}

static void _cas_cleaner_complete(ocf_cleaner_t c, uint32_t interval)
{
	struct cas_thread_info *info = ocf_cleaner_get_priv(c);
	uint32_t *ms = info->sync_data;

	*ms = interval;
	complete(&info->sync_compl);
}

static int cleaner_thread_run(void *data)
{
	ocf_cleaner_t c = data;
	ocf_cache_t cache = ocf_cleaner_get_cache(c);
	struct cache_priv *cache_priv = ocf_cache_get_priv(cache);
	struct cas_thread_info *info;
	uint32_t ms;

	BUG_ON(!c);

	ENV_BUG_ON(!cache_priv);
	/* complete the creation of the thread */
	info = ocf_cleaner_get_priv(c);
	BUG_ON(!info);

	CAS_DAEMONIZE(info->thread->comm);

	complete(&info->compl);

	info->sync_data = &ms;
	ocf_cleaner_set_cmpl(c, _cas_cleaner_complete);

	do {
		if (atomic_read(&info->stop))
			break;

		atomic_set(&info->kicked, 0);
		init_completion(&info->sync_compl);
		ocf_cleaner_run(c, cache_get_fastest_porter_queue(cache));
		wait_for_completion(&info->sync_compl);

		/*
		 * In case of nop cleaning policy we don't want to perform cleaning
		 * until cleaner_kick() is called.
		 */
		if (ms == OCF_CLEANER_DISABLE) {
			wait_event_interruptible(info->wq, atomic_read(&info->kicked) ||
					atomic_read(&info->stop));
		} else {
			wait_event_interruptible_timeout(info->wq,
					atomic_read(&info->kicked) || atomic_read(&info->stop),
					msecs_to_jiffies(ms));
		}
	} while (true);

	cache_print_each_porter_queue_pending_io(cache);

	CAS_COMPLETE_AND_EXIT(&info->compl, 0);

	return 0;
}

static int _cas_create_thread(struct cas_thread_info **pinfo,
		int (*threadfn)(void *), void *priv, const char *name, int cpu)
{
	struct cas_thread_info *info;
	struct task_struct *thread;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	atomic_set(&info->stop, 0);
	init_completion(&info->compl);
	init_completion(&info->sync_compl);
	init_waitqueue_head(&info->wq);
	snprintf(info->name, sizeof(info->name), "%s", name);

	thread = kthread_create(threadfn, priv, "%s", info->name);
	if (IS_ERR(thread)) {
		kfree(info);
		/* Propagate error code as PTR_ERR */
		return PTR_ERR(thread);
	}
	info->thread = thread;

	/* Affinitize thread to core */
	if (cpu != CAS_CPUS_ALL)
		kthread_bind(thread, cpu);

	if (pinfo)
		*pinfo = info;

	return 0;

}

static void _cas_start_thread(struct cas_thread_info *info)
{
	wake_up_process(info->thread);
	wait_for_completion(&info->compl);

	printk(KERN_DEBUG "Thread %s started\n", info->name);
}

static void _cas_stop_thread(struct cas_thread_info *info)
{
	if (info && info->thread) {
		reinit_completion(&info->compl);
		atomic_set(&info->stop, 1);
		wake_up(&info->wq);
		wait_for_completion(&info->compl);
		printk(KERN_DEBUG "Thread %s stopped\n", info->name);
	}
	kfree(info);
}

int cas_create_queue_thread(ocf_queue_t q, const char *name, int cpu)
{
	struct cas_thread_info *info;
	int result;

	result = _cas_create_thread(&info, queue_thread_run, q, name, cpu);
	if (!result) {
		ocf_queue_set_priv(q, info);
		_cas_start_thread(info);
	}

	return result;
}

void cas_kick_queue_thread(ocf_queue_t q)
{
	struct cas_thread_info *info = ocf_queue_get_priv(q);
	wake_up(&info->wq);
}


void cas_stop_queue_thread(ocf_queue_t q)
{
	struct cas_thread_info *info = ocf_queue_get_priv(q);
	ocf_queue_set_priv(q, NULL);
	_cas_stop_thread(info);
}

int cas_create_cleaner_thread(ocf_cleaner_t c, const char *name)
{
	struct cas_thread_info *info;
	int result;

	result = _cas_create_thread(&info, cleaner_thread_run, c, name, CAS_CPUS_ALL);
	if (!result) {
		ocf_cleaner_set_priv(c, info);
		_cas_start_thread(info);
	}

	return result;
}

void cas_kick_cleaner_thread(ocf_cleaner_t c)
{
	struct cas_thread_info *info = ocf_cleaner_get_priv(c);
	atomic_set(&info->kicked, 1);
	wake_up(&info->wq);
}

void cas_stop_cleaner_thread(ocf_cleaner_t c)
{
	struct cas_thread_info *info = ocf_cleaner_get_priv(c);
	_cas_stop_thread(info);
	ocf_cleaner_set_priv(c, NULL);
}

