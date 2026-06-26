#ifndef _EPHMFS_H
#define _EPHMFS_H

#include <linux/kobject.h>
#include <linux/maple_tree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

struct ephmfs_page;
struct ephmfs_sb_info;

/*
 * Ephemeral memory is allocated to a host/VM in chunks of EPHMFS_CHUNK_SIZE
 * and are similarly aligned.
 */
#define EPHMFS_CHUNK_SIZE (256UL * 1024 * 1024)

/* Stores the information for a dax device used by EphMFS */
struct ephmfs_dev_info {
	struct dax_device *dax_dev;
	struct ephmfs_sb_info *sbi; /* The superblock this device belongs to */
	char *dev_name; /* The name of the device, e.g. "dax0.0" */
	void *kaddr; /* Kernel virtual address for the mapped device */
	unsigned long pfn; /* The pfn of the first page of the device */
	struct ephmfs_page *pages; /* EphMFS pages for this device */
	struct list_head free_list;
	struct list_head active_list;
	u64 num_pages;
	u64 free_pages;
	u64 revoked_pages;
	spinlock_t lock;
	struct list_head node;
};

struct ephmfs_page {
	u64 page_num; /* The physical page number within the device */
	u64 page_offset; /* The page offset within the file */
	/*
	 * If we are using huge pages, but an allocation only uses base pages,
	 * this represents the number of base pages in this page.
	 */
	u64 num_base_pages;
	struct inode *inode; /* The inode this page belongs to */
	struct ephmfs_dev_info *dev_info; /* The device this page belongs to */
	bool revoked;
	spinlock_t lock; /* Protects the fields above it in this struct. */
	/*
	 * Linked list node to connect pages to the free/active list.
	 * Protected by ephmfs_dev_info.lock
	 */
	struct list_head node;
	/*
	 * Whether or not this page is on the free list.
	 * Protected by ephmfs_dev_info.lock
	 */
	bool on_free_list;
};

struct ephmfs_sb_info {
	u64 page_size;
	struct kobject sysfs_kobj;
	struct list_head dax_devs;
	rwlock_t lock;
};

struct ephmfs_inode_info {
	atomic_t alloc_count;
	/* Assume that only one task can map the same EphMFS file */
	struct task_struct *owner;
	u64 base_addr;
	/*
	 * Applications can only access a file when it is marked as in the
	 * "attempt" context. This helps ensure that applications understand
	 * the risks of using ephemeral memory before they can use it.
	 */
	unsigned int attempt_count;
	spinlock_t lock;
	struct inode *inode;
	struct maple_tree mt;
};

#endif // _EPHMFS_H