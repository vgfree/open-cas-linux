/*
* Copyright(c) 2012-2022 Intel Corporation
* SPDX-License-Identifier: BSD-3-Clause
*/
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/blkpg.h>
#include <linux/elevator.h>
#include <linux/blk-mq.h>

#include "disk.h"
#include "exp_obj.h"
#include "linux_kernel_version.h"
#include "cas_cache.h"
#include "debug.h"

#define CAS_DEV_MINORS 16
#define KMEM_CACHE_MIN_SIZE sizeof(void *)

static inline struct cas_exp_obj *cas_kobj_to_exp_obj(struct kobject *kobj)
{
	return container_of(kobj, struct cas_exp_obj, kobj);
}

static inline int bd_claim_by_disk(struct block_device *bdev, void *holder,
				   struct gendisk *disk)
{
	return bd_link_disk_holder(bdev, disk);
}

static inline void bd_release_from_disk(struct block_device *bdev,
					struct gendisk *disk)
{
	return bd_unlink_disk_holder(bdev, disk);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 3, 0)
	#define KRETURN(x)	({ return (x); })
	#define MAKE_RQ_RET_TYPE blk_qc_t
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 2, 0)
	#define KRETURN(x)	return
	#define MAKE_RQ_RET_TYPE void
#else
	#define KRETURN(x)	({ return (x); })
	#define MAKE_RQ_RET_TYPE int
#endif

int __init cas_init_exp_objs(void)
{
	CAS_DEBUG_TRACE();

	cas_module.exp_obj_cache = kmem_cache_create("cas_exp_obj",
			sizeof(struct cas_exp_obj), 0, 0, NULL);
	if (!cas_module.exp_obj_cache)
		return -ENOMEM;

	return 0;
}

void cas_deinit_exp_objs(void)
{
	CAS_DEBUG_TRACE();

	kmem_cache_destroy(cas_module.exp_obj_cache);
}

static MAKE_RQ_RET_TYPE _cas_exp_obj_submit_bio(struct bio *bio)
{
	struct cas_disk *dsk;

	BUG_ON(!bio);
	dsk = CAS_BIO_GET_GENDISK(bio)->private_data;

	dsk->exp_obj->ops->submit_bio(dsk, bio, dsk->private);

	KRETURN(0);
}

static MAKE_RQ_RET_TYPE _cas_exp_obj_make_rq_fn(struct request_queue *q,
						 struct bio *bio)
{
	_cas_exp_obj_submit_bio(bio);
	cas_blk_queue_exit(q);
	KRETURN(0);
}

static int _cas_del_partitions(struct cas_disk *dsk)
{
	struct block_device *bd = cas_disk_get_blkdev(dsk);
	struct file *bd_file;
	unsigned long __user usr_bpart;
	unsigned long __user usr_barg;
	struct blkpg_partition bpart;
	struct blkpg_ioctl_arg barg;
	int result = 0;
	int part_no;

	bd_file = filp_open(dsk->path, 0, 0);
	if (IS_ERR(bd_file))
		return PTR_ERR(bd_file);

	usr_bpart = cas_vm_mmap(NULL, 0, sizeof(bpart));
	if (IS_ERR((void *)usr_bpart)) {
		result = PTR_ERR((void *)usr_bpart);
		goto out_map_bpart;
	}

	usr_barg = cas_vm_mmap(NULL, 0, sizeof(barg));
	if (IS_ERR((void *)usr_barg)) {
		result = PTR_ERR((void *)usr_barg);
		goto out_map_barg;
	}


	memset(&bpart, 0, sizeof(bpart));
	memset(&barg, 0, sizeof(barg));
	barg.data = (void __user *)usr_bpart;
	barg.op = BLKPG_DEL_PARTITION;

	result = copy_to_user((void __user *)usr_barg, &barg, sizeof(barg));
	if (result) {
		result = -EINVAL;
		goto out_copy;
	}

	while ((part_no = cas_bd_get_next_part(bd))) {
		bpart.pno = part_no;
		result = copy_to_user((void __user *)usr_bpart, &bpart,
				sizeof(bpart));
		if (result) {
			result = -EINVAL;
			break;
		}
		result = cas_vfs_ioctl(bd_file, BLKPG, usr_barg);
		if (result == 0) {
			printk(KERN_INFO "Partition %d on %s hidden\n",
				part_no, bd->bd_disk->disk_name);
		} else {
			printk(KERN_ERR "Error(%d) hiding the partition %d on %s\n",
				result, part_no, bd->bd_disk->disk_name);
			break;
		}
	}

out_copy:
	cas_vm_munmap(usr_barg, sizeof(barg));
out_map_barg:
	cas_vm_munmap(usr_bpart, sizeof(bpart));
out_map_bpart:
	filp_close(bd_file, NULL);
	return result;
}

#ifdef GENHD_FL_NO_PART_SCAN
static int _cas_flags = GENHD_FL_NO_PART_SCAN | GENHD_FL_EXT_DEVT;
#else
static int _cas_flags = GENHD_FL_EXT_DEVT;
#endif

static int _cas_exp_obj_hide_parts(struct cas_disk *dsk)
{
	struct block_device *bd = cas_disk_get_blkdev(dsk);
	struct gendisk *gdsk = cas_disk_get_gendisk(dsk);

	if (bd != cas_bdev_whole(bd))
		/* It is partition, no more job required */
		return 0;

	if (disk_max_parts(dsk->bd->bd_disk) > 1) {
		if (_cas_del_partitions(dsk)) {
			printk(KERN_ERR "Error deleting a partition on thedevice %s\n",
				gdsk->disk_name);

			/* Try restore previous partitions by rescaning */
			cas_reread_partitions(bd);
			return -EINVAL;
		}
	}

	/* Save original flags and minors */
	dsk->gd_flags = gdsk->flags & _cas_flags;
	dsk->gd_minors = gdsk->minors;

	/* Setup disk of bottom device as not partitioned device */
	gdsk->flags &= ~_cas_flags;
	gdsk->minors = 1;
	/* Rescan partitions */
	cas_reread_partitions(bd);

	return 0;
}

static int _cas_exp_obj_set_dev_t(struct cas_disk *dsk, struct gendisk *gd)
{
	int flags;
	int minors = disk_max_parts(cas_disk_get_gendisk(dsk));
	struct block_device *bdev;

	bdev = cas_disk_get_blkdev(dsk);
	BUG_ON(!bdev);

	if (cas_bdev_whole(bdev) != bdev) {
		minors = 1;
		flags = 0;
	} else {
		if (_cas_exp_obj_hide_parts(dsk))
			return -EINVAL;
		flags = dsk->gd_flags;
	}

	gd->first_minor = cas_disk_allocate_minors(minors);
	if (gd->first_minor < 0) {
		CAS_DEBUG_DISK_ERROR(dsk, "Cannot allocate %d minors", minors);
		return -EINVAL;
	}
	gd->minors = minors;

	gd->major = cas_module.disk_major;
	gd->flags |= flags;

	return 0;
}

static void _cas_exp_obj_clear_dev_t(struct cas_disk *dsk)
{
	struct block_device *bdev = cas_disk_get_blkdev(dsk);
	struct gendisk *gdsk = cas_disk_get_gendisk(dsk);

	if (cas_bdev_whole(bdev) == bdev) {
		/* Restore previous configuration of bottom disk */
		gdsk->minors = dsk->gd_minors;
		gdsk->flags |= dsk->gd_flags;
		cas_reread_partitions(bdev);
	}
}

static int _cas_exp_obj_open(struct block_device *bdev, fmode_t mode)
{
	struct cas_disk *dsk = bdev->bd_disk->private_data;
	int result = -ENAVAIL;

	mutex_lock(&dsk->openers_lock);

	if (!dsk->claimed) {
		if (unlikely(dsk->openers == UINT_MAX)) {
			result = -EBUSY;
		} else {
			dsk->openers++;
			result = 0;
		}
	}

	mutex_unlock(&dsk->openers_lock);
	return result;
}

static void _cas_exp_obj_close(struct gendisk *gd, fmode_t mode)
{
	struct cas_disk *dsk = gd->private_data;

	BUG_ON(dsk->openers == 0);

	mutex_lock(&dsk->openers_lock);
	dsk->openers--;
	mutex_unlock(&dsk->openers_lock);

}

static const struct block_device_operations _cas_exp_obj_ops = {
	.owner = THIS_MODULE,
	.open = _cas_exp_obj_open,
	.release = _cas_exp_obj_close,
	CAS_SET_SUBMIT_BIO(_cas_exp_obj_submit_bio)
};

static int cas_exp_obj_alloc(struct cas_disk *dsk)
{
	struct cas_exp_obj *exp_obj;

	BUG_ON(!dsk);
	BUG_ON(dsk->exp_obj);

	CAS_DEBUG_DISK_TRACE(dsk);

	exp_obj = kmem_cache_zalloc(cas_module.exp_obj_cache, GFP_KERNEL);
	if (!exp_obj) {
		CAS_DEBUG_ERROR("Cannot allocate memory");
		return -ENOMEM;
	}

	dsk->exp_obj = exp_obj;

	return 0;
}

void cas_exp_obj_free(struct cas_disk *dsk)
{
	struct cas_exp_obj *exp_obj;

	CAS_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	if (!exp_obj)
		return;

	kobject_put(&exp_obj->kobj);
	dsk->exp_obj = NULL;
}

static void __cas_exp_obj_release(struct cas_exp_obj *exp_obj)
{
	kmem_cache_free(cas_module.exp_obj_cache, exp_obj);
}
static void _cas_exp_obj_release(struct kobject *kobj)
{
	struct cas_exp_obj *exp_obj;
	struct module *owner;

	BUG_ON(!kobj);

	exp_obj = cas_kobj_to_exp_obj(kobj);
	BUG_ON(!exp_obj);

	CAS_DEBUG_TRACE();

	owner = exp_obj->owner;

	kfree(exp_obj->dev_name);
	__cas_exp_obj_release(exp_obj);

	if (owner)
		module_put(owner);
}

static struct kobj_type cas_exp_obj_ktype = {
	.release = _cas_exp_obj_release
};

static int _cas_exp_obj_init_kobject(struct cas_disk *dsk)
{
	int result = 0;
	struct cas_exp_obj *exp_obj = dsk->exp_obj;

	kobject_init(&exp_obj->kobj, &cas_exp_obj_ktype);
	result = kobject_add(&exp_obj->kobj, &dsk->kobj,
			     "%s", exp_obj->dev_name);
	if (result)
		CAS_DEBUG_DISK_ERROR(dsk, "Cannot register kobject");

	return result;
}

static CAS_BLK_STATUS_T _cas_exp_obj_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	return CAS_BLK_STS_NOTSUPP;
}

static struct blk_mq_ops cas_mq_ops = {
	.queue_rq       = _cas_exp_obj_queue_rq,
#ifdef CAS_BLK_MQ_OPS_MAP_QUEUE
	.map_queue	= blk_mq_map_queue,
#endif
};

static void _cas_init_queues(struct cas_disk *dsk)
{
	struct request_queue *q = dsk->exp_obj->queue;
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (!hctx->nr_ctx || !hctx->tags)
			continue;

		hctx->driver_data = dsk;
	}
}

static int _cas_init_tag_set(struct cas_disk *dsk, struct blk_mq_tag_set *set)
{
	BUG_ON(!dsk);
	BUG_ON(!set);

	set->ops = &cas_mq_ops;
	set->nr_hw_queues = num_online_cpus();
	set->numa_node = NUMA_NO_NODE;
	/*TODO: Should we inherit qd from core device? */
	set->queue_depth = BLKDEV_MAX_RQ;

	set->cmd_size = 0;
	set->flags = BLK_MQ_F_SHOULD_MERGE | CAS_BLK_MQ_F_STACKING | CAS_BLK_MQ_F_BLOCKING;

	set->driver_data = dsk;

	return blk_mq_alloc_tag_set(set);
}

int cas_exp_obj_create(struct cas_disk *dsk, const char *dev_name,
		struct module *owner, struct cas_exp_obj_ops *ops, void *priv)
{
	struct cas_exp_obj *exp_obj;
	struct request_queue *queue;
	struct gendisk *gd;
	int result = 0;

	BUG_ON(!owner);
	BUG_ON(!dsk);
	BUG_ON(!ops);
	BUG_ON(dsk->exp_obj);

	CAS_DEBUG_DISK_TRACE(dsk);

	if (strlen(dev_name) >= DISK_NAME_LEN)
		return -EINVAL;

	result = cas_exp_obj_alloc(dsk);
	if (result)
		goto error_exp_obj_alloc;

	exp_obj = dsk->exp_obj;

	exp_obj->dev_name = kstrdup(dev_name, GFP_KERNEL);
	if (!exp_obj->dev_name) {
		result = -ENOMEM;
		goto error_kstrdup;
	}

	if (!try_module_get(owner)) {
		CAS_DEBUG_DISK_ERROR(dsk, "Cannot get reference to module");
		result = -ENAVAIL;
		goto error_module_get;
	}
	exp_obj->owner = owner;
	exp_obj->ops = ops;

	result = _cas_exp_obj_init_kobject(dsk);
	if (result) {
		goto error_init_kobject;
	}

	result = _cas_init_tag_set(dsk, &dsk->tag_set);
	if (result) {
		goto error_init_tag_set;
	}

	result = cas_alloc_mq_disk(&gd, &queue, &dsk->tag_set);
	if (result) {
		goto error_alloc_mq_disk;
	}

	exp_obj->gd = gd;

	result = _cas_exp_obj_set_dev_t(dsk, gd);
	if (result)
		goto error_exp_obj_set_dev_t;

	BUG_ON(queue->queuedata);
	queue->queuedata = dsk;
	exp_obj->queue = queue;

	dsk->private = priv;

	_cas_init_queues(dsk);

	gd->fops = &_cas_exp_obj_ops;
	gd->private_data = dsk;
	strlcpy(gd->disk_name, exp_obj->dev_name, sizeof(gd->disk_name));

	cas_blk_queue_make_request(queue, _cas_exp_obj_make_rq_fn);

	if (exp_obj->ops->set_geometry) {
		result = exp_obj->ops->set_geometry(dsk, dsk->private);
		if (result)
			goto error_set_geometry;
	}

	return 0;

error_set_geometry:
	dsk->private = NULL;
	_cas_exp_obj_clear_dev_t(dsk);
error_exp_obj_set_dev_t:
	cas_cleanup_mq_disk(exp_obj);
	dsk->exp_obj->gd = NULL;
error_alloc_mq_disk:
	blk_mq_free_tag_set(&dsk->tag_set);
error_init_tag_set:
	kobject_put(&exp_obj->kobj);
	/* kobject put does all the cleanup below internally */
	return result;
error_init_kobject:
	module_put(owner);
	dsk->exp_obj->owner = NULL;
error_module_get:
	kfree(exp_obj->dev_name);
error_kstrdup:
	__cas_exp_obj_release(exp_obj);
	dsk->exp_obj = NULL;
error_exp_obj_alloc:
	return result;

}

struct request_queue *cas_exp_obj_get_queue(struct cas_disk *dsk)
{
	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	return dsk->exp_obj->queue;
}

struct gendisk *cas_exp_obj_get_gendisk(struct cas_disk *dsk)
{
	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	return dsk->exp_obj->gd;
}

static bool _cas_exp_obj_exists(const char *path)
{
	struct file *exported;

	exported = filp_open(path, O_RDONLY, 0);

	if (!exported || IS_ERR(exported)) {
		/*failed to open file - it is safe to assume,
		 * it does not exist
		 */
		return false;
	}

	filp_close(exported, NULL);
	return true;
}

int cas_exp_obj_activate(struct cas_disk *dsk)
{
	char *path;
	int result;

	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	BUG_ON(!dsk->exp_obj->gd);
	BUG_ON(dsk->exp_obj->activated);

	CAS_DEBUG_DISK_TRACE(dsk);

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	snprintf(path, PATH_MAX, "/dev/%s", dsk->exp_obj->dev_name);
	if (_cas_exp_obj_exists(path)) {
		printk(KERN_ERR "Could not activate exported object, "
				"because file %s exists.\n", path);
		kfree(path);
		return -EEXIST;
	}
	kfree(path);

	dsk->exp_obj->activated = true;
	add_disk(dsk->exp_obj->gd);

	result = bd_claim_by_disk(dsk->bd, dsk, dsk->exp_obj->gd);
	if (result)
		goto error_bd_claim;

	result = sysfs_create_link(&dsk->exp_obj->kobj,
				   &disk_to_dev(dsk->exp_obj->gd)->kobj,
				   "blockdev");
	if (result)
		goto error_sysfs_link;

	CAS_DEBUG_DISK(dsk, "Activated exp object %s", dsk->exp_obj->dev_name);

	return 0;

error_sysfs_link:
	bd_release_from_disk(dsk->bd, dsk->exp_obj->gd);
error_bd_claim:
	del_gendisk(dsk->exp_obj->gd);
	dsk->exp_obj->activated = false;
	return result;
}

int cas_exp_obj_lock(struct cas_disk *dsk)
{
	struct cas_exp_obj *exp_obj;
	int result = -EBUSY;

	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);

	CAS_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	mutex_lock(&dsk->openers_lock);

	if (dsk->openers == 0) {
		dsk->claimed = true;
		result = 0;
	}

	mutex_unlock(&dsk->openers_lock);
	return result;
}

int cas_exp_obj_unlock(struct cas_disk *dsk)
{
	BUG_ON(!dsk);
	CAS_DEBUG_DISK_TRACE(dsk);

	mutex_lock(&dsk->openers_lock);
	dsk->claimed = false;
	mutex_unlock(&dsk->openers_lock);

	return 0;
}

int cas_exp_obj_destroy(struct cas_disk *dsk)
{
	struct cas_exp_obj *exp_obj;

	BUG_ON(!dsk);

	if (!dsk->exp_obj)
		return -ENODEV;

	CAS_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	if (dsk->exp_obj->activated) {
		sysfs_remove_link(&exp_obj->kobj, "blockdev");
		bd_release_from_disk(dsk->bd, exp_obj->gd);
		_cas_exp_obj_clear_dev_t(dsk);
		del_gendisk(exp_obj->gd);
	}

	if (exp_obj->queue)
		blk_cleanup_queue(exp_obj->queue);

	blk_mq_free_tag_set(&dsk->tag_set);

	put_disk(exp_obj->gd);

	return 0;

}
