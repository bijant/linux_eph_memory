#ifndef _EPHMFS_H
#define _EPHMFS_H

#include <linux/kobject.h>
#include <linux/maple_tree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* Stores the information for a dax device used by EphMFS */
struct ephmfs_dev_info {
	struct file *bdev_file;
	struct dax_device *dax_dev;
	char *dev_name; /* The name of the device, e.g. "dax0.0" */
	void *kaddr; /* Kernel virtual address for the mapped device */
	unsigned long pfn; /* The pfn of the first page of the device */
	struct list_head free_list;
	struct list_head active_list;
	u64 num_pages;
	u64 free_pages;
	spinlock_t lock;
	struct list_head node;
};

struct ephmfs_sb_info {
	u64 num_pages;
	u64 page_size;
	struct kobject sysfs_kobj;
	struct list_head dax_devs;
	spinlock_t lock;
};

struct ephmfs_inode_info {
	atomic_t alloc_count;
	spinlock_t mt_lock;
	struct address_space *mapping;
	struct maple_tree mt;
};

#endif // _EPHMFS_H