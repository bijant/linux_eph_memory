#include <linux/kernel.h>
#include <linux/blkdev.h>
#include <linux/dax.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/iomap.h>
#include <linux/kobject.h>
#include <linux/maple_tree.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/statfs.h>
#include <linux/types.h>

#include "ephmfs.h"

static const struct super_operations ephmfs_ops;
static const struct inode_operations ephmfs_dir_inode_ops;
static struct kobj_attribute ephmfs_devs_attr;

static struct ephmfs_sb_info *EMFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static struct ephmfs_inode_info *EMFS_INODE(struct inode *inode)
{
	return inode->i_private;
}

static struct ephmfs_page *ephmfs_alloc_page(struct ephmfs_sb_info *sbi)
{
	struct ephmfs_page *page = NULL;
	struct ephmfs_dev_info *dev_info;
	void *kaddr;

	spin_lock(&sbi->lock);

	/* Find a device with free pages */
	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		spin_lock(&dev_info->lock);

		if (dev_info->free_pages == 0) {
			spin_unlock(&dev_info->lock);
			continue;
		}

		page = list_first_entry(&dev_info->free_list, struct ephmfs_page, node);
		list_del(&page->node);
		list_add_tail(&page->node, &dev_info->active_list);
		dev_info->free_pages--;

		spin_unlock(&dev_info->lock);

		break;
	}

	/* Make sure to zero the page */
	if (page) {
		kaddr = dev_info->kaddr + (page->page_num << ilog2(sbi->page_size));
		memset(kaddr, 0, sbi->page_size);
	}

	spin_unlock(&sbi->lock);

	return page;
}

static void ephmfs_free_page(struct ephmfs_page *page)
{
	struct ephmfs_dev_info *dev_info = page->dev_info;

	spin_lock(&dev_info->lock);

	list_del(&page->node);
	list_add(&page->node, &dev_info->free_list);
	dev_info->free_pages++;

	spin_lock(&page->lock);
	page->page_offset = 0;
	page->inode = NULL;
	spin_unlock(&page->lock);

	spin_unlock(&dev_info->lock);
}

/*
 * Allocate a page and insert it into the inode's maple tree at the given offset.
 * Returns the allocated page, or NULL on failure.
 * The inode_info->mt_lock should be held by the caller, and will still be held
 * when this function returns.
 */
static struct ephmfs_page *ephmfs_alloc_and_insert_page(struct ephmfs_sb_info *sbi,
	struct ephmfs_inode_info *inode_info, u64 page_offset)
{
	struct ephmfs_page *page;
	int ret;

	page = ephmfs_alloc_page(sbi);
	if (!page)
		return NULL;

	/* Record the file offset before publishing the page into the tree. */
	page->page_offset = page_offset;

	ret = mtree_insert(&inode_info->mt, page_offset, page, GFP_KERNEL);
	if (ret) {
		ephmfs_free_page(page);
		return NULL;
	}

	return page;
}

static int ephmfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct ephmfs_sb_info *sbi = EMFS_SB(inode->i_sb);
	struct ephmfs_inode_info *inode_info = EMFS_INODE(inode);
	struct ephmfs_page *page;
	u64 page_offset;
	u64 page_shift;
	int ret = 0;

	page_shift = ilog2(sbi->page_size);
	page_offset = offset >> page_shift;

	iomap->flags = 0;
	iomap->offset = offset;
	iomap->length = length;

	spin_lock(&inode_info->mt_lock);
	page = mtree_load(&inode_info->mt, page_offset);

	if (!page) {
		page = ephmfs_alloc_and_insert_page(sbi, inode_info, page_offset);
		if (!page) {
			ret = -ENOSPC;
			goto out_unlock;
		}

		iomap->flags |= IOMAP_F_NEW;
		iomap->type = IOMAP_MAPPED;
		iomap->addr = page->page_num << page_shift;
		iomap->dax_dev = page->dev_info->dax_dev;
	} else {
		/* There is already a page allocated. Just use that */
		iomap->type = IOMAP_MAPPED;
		iomap->addr = page->page_num << page_shift;
		iomap->dax_dev = page->dev_info->dax_dev;
	}

out_unlock:
	spin_unlock(&inode_info->mt_lock);
	return ret;
}

const struct iomap_ops ephmfs_iomap_ops = {
	.iomap_begin = ephmfs_iomap_begin,
};

/*
 * Callback invoked by dax_break_layout() while it waits for a mapped DAX page
 * to become idle. Drop the invalidate lock so the process holding the mapping
 * can unmap/unpin the page, then reacquire it before retrying.
 */
static void ephmfs_wait_dax_page(struct inode *inode)
{
	filemap_invalidate_unlock(inode->i_mapping);
	schedule();
	filemap_invalidate_lock(inode->i_mapping);
}

/*
 * Unmap [start, end) from all processes and remove the corresponding DAX
 * entries from the mapping, waiting for any outstanding page references to
 * drain. After this returns successfully the backing pages in the range are no
 * longer mapped and may be freed. Must be called with the inode's invalidate
 * lock held exclusively.
 */
static int ephmfs_break_layout(struct inode *inode, loff_t start, loff_t end)
{
	return dax_break_layout(inode, start, end, ephmfs_wait_dax_page);
}

static vm_fault_t ephmfs_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t result = 0;
	unsigned long pfn;

	/*
	 * Hold the invalidate lock shared across the fault so that hole-punch
	 * and truncate (which take it exclusively) cannot free a page out from
	 * under us while we are establishing a mapping for it.
	 */
	filemap_invalidate_lock_shared(inode->i_mapping);
	result = dax_iomap_fault(vmf, order, &pfn, NULL, &ephmfs_iomap_ops);
	filemap_invalidate_unlock_shared(inode->i_mapping);

	return result;
}

static vm_fault_t ephmfs_fault(struct vm_fault *vmf)
{
	return ephmfs_huge_fault(vmf, 0);
}

static struct vm_operations_struct ephmfs_vm_ops = {
	.fault = ephmfs_fault,
	.huge_fault = ephmfs_huge_fault,
	.page_mkwrite = ephmfs_fault,
	.pfn_mkwrite = ephmfs_fault,
};

static int ephmfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);

	/* Only allow mapping of regular files */
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	vma->vm_ops = &ephmfs_vm_ops;
	info->mapping = file->f_mapping;

	return 0;
}

static unsigned long ephmfs_get_unmapped_area(struct file *file,
	unsigned long addr, unsigned long len, unsigned long pgoff,
	unsigned long flags)
{
	return thp_get_unmapped_area(file, addr, len, pgoff, flags);
}

static long ephmfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	struct ephmfs_sb_info *sbi = EMFS_SB(inode->i_sb);
	struct ephmfs_page *page;
	u64 page_shift;
	u64 off;

	page_shift = ilog2(sbi->page_size);

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		int ret;

		/*
		 * Unmap the range and drain any references before freeing the
		 * backing pages. Otherwise a process that still has the range
		 * mapped would keep accessing pages that have been returned to
		 * the device free list (and possibly reallocated to another
		 * inode). The invalidate lock keeps faults from re-establishing
		 * a mapping while we tear it down.
		 */
		filemap_invalidate_lock(inode->i_mapping);

		ret = ephmfs_break_layout(inode, offset, offset + len);
		if (ret) {
			filemap_invalidate_unlock(inode->i_mapping);
			return ret;
		}

		// Free pages in the desired range
		for (off = offset; off < offset + len; off += sbi->page_size) {
			u64 page_offset = off >> page_shift;

			spin_lock(&info->mt_lock);
			page = mtree_erase(&info->mt, page_offset);
			spin_unlock(&info->mt_lock);

			if (page)
				ephmfs_free_page(page);
		}

		filemap_invalidate_unlock(inode->i_mapping);
		return 0;
	} else if (mode != 0) {
		return -EOPNOTSUPP;
	}

	// Allocate pages for the desired range
	for (off = offset; off < offset + len; off += sbi->page_size) {
		u64 page_offset = off >> page_shift;

		spin_lock(&info->mt_lock);
		page = mtree_load(&info->mt, page_offset);
		if (!page) {
			page = ephmfs_alloc_and_insert_page(sbi, info, page_offset);
			if (!page) {
				spin_unlock(&info->mt_lock);
				return -ENOSPC;
			}
		}
		spin_unlock(&info->mt_lock);
	}

	return 0;
}

const struct file_operations ephmfs_file_operations = {
	.mmap = ephmfs_mmap,
	.fop_flags = FOP_MMAP_SYNC,
	.fsync = noop_fsync,
	.splice_read = copy_splice_read,
	.splice_write = iter_file_splice_write,
	.llseek = generic_file_llseek,
	.get_unmapped_area = ephmfs_get_unmapped_area,
	.fallocate = ephmfs_fallocate,
};

/*
 * Free every backing page that lies entirely beyond @newsize. The page that
 * contains @newsize (if any) is kept. Caller must hold the invalidate lock and
 * must have already broken the DAX layout for the truncated range.
 */
static void ephmfs_truncate_pages(struct inode *inode, loff_t newsize)
{
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	struct ephmfs_sb_info *sbi = EMFS_SB(inode->i_sb);
	u64 page_shift = ilog2(sbi->page_size);
	/* First page index fully beyond the new size. */
	unsigned long index = (newsize + sbi->page_size - 1) >> page_shift;
	struct ephmfs_page *page;

	spin_lock(&info->mt_lock);
	mt_for_each(&info->mt, page, index, ULONG_MAX) {
		mtree_erase(&info->mt, page->page_offset);
		ephmfs_free_page(page);
	}
	spin_unlock(&info->mt_lock);
}

static int ephmfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(idmap, dentry, attr);
	if (error)
		return error;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size < inode->i_size) {
		/*
		 * Shrinking: unmap and free the backing pages beyond the new
		 * size. Hold the invalidate lock exclusively so faults cannot
		 * re-establish a mapping while we tear it down, and break the
		 * DAX layout before truncating the page cache (otherwise
		 * truncate_inode_pages would warn about leftover DAX entries)
		 * and before returning pages to the device.
		 */
		filemap_invalidate_lock(inode->i_mapping);

		error = ephmfs_break_layout(inode, attr->ia_size, LLONG_MAX);
		if (error) {
			filemap_invalidate_unlock(inode->i_mapping);
			return error;
		}

		truncate_setsize(inode, attr->ia_size);
		ephmfs_truncate_pages(inode, attr->ia_size);

		filemap_invalidate_unlock(inode->i_mapping);
	} else if (attr->ia_valid & ATTR_SIZE) {
		truncate_setsize(inode, attr->ia_size);
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations ephmfs_file_inode_ops = {
	.getattr = simple_getattr,
	.setattr = ephmfs_setattr,
};

const struct address_space_operations ephmfs_aops = {
	.direct_IO = noop_direct_IO,
	.dirty_folio = noop_dirty_folio,
};

static struct inode *ephmfs_get_inode(struct super_block *sb, const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	struct ephmfs_inode_info *info;
	if (!inode)
		return NULL;

	info = kzalloc(sizeof(struct ephmfs_inode_info), GFP_KERNEL);
	if (!info) {
		iput(inode);
		return NULL;
	}

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &ephmfs_aops;
	inode->i_flags |= S_DAX;
	inode->i_private = info;

	switch (mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &ephmfs_file_inode_ops;
		inode->i_fop = &ephmfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &ephmfs_dir_inode_ops;
		inode->i_fop = &simple_dir_operations;

		/* Directory inodes start off with a reference count of 2 (for "." entry) */
		inc_nlink(inode);
		break;
	default:
		return NULL;
	}

	return inode;
}

static int
ephmfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode *inode = ephmfs_get_inode(dir->i_sb, dir, mode, dev);
	if (!inode)
		return -ENOMEM;

	d_instantiate(dentry, inode);
	dget(dentry); /* Extra count for dentry */
	return 0;
}

static struct dentry *ephmfs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return ERR_PTR(-EINVAL);
}

static int ephmfs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return ephmfs_mknod(idmap, dir, dentry, mode | S_IFREG, 0);
}

static int ephmfs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *link)
{
	return -EINVAL;
}

static int ephmfs_tmpfile(struct mnt_idmap *idmap, struct inode *dir, struct file *filp, umode_t mode)
{
	struct inode *inode = ephmfs_get_inode(filp->f_path.dentry->d_sb, dir, S_IFREG | 0600, 0);
	if (!inode)
		return -ENOMEM;

	d_tmpfile(filp, inode);
	return finish_open_simple(filp, 0);
}

static const struct inode_operations ephmfs_dir_inode_ops = {
	.create  = ephmfs_create,
	.lookup  = simple_lookup,
	.link    = simple_link,
	.unlink  = simple_unlink,
	.symlink = ephmfs_symlink,
	.mkdir   = ephmfs_mkdir,
	.rmdir   = simple_rmdir,
	.mknod   = ephmfs_mknod,
	.rename  = simple_rename,
	.tmpfile = ephmfs_tmpfile,
};

static int ephmfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ephmfs_sb_info *sbi = EMFS_SB(sb);
	struct ephmfs_dev_info *dev_info;
	u64 num_pages = 0;
	u64 free_pages = 0;

	spin_lock(&sbi->lock);

	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		num_pages += dev_info->num_pages;
		free_pages += dev_info->free_pages;
	}

	buf->f_type = sb->s_magic;
	buf->f_bsize = PAGE_SIZE;
	buf->f_blocks = num_pages;
	buf->f_bfree = free_pages;
	buf->f_bavail = free_pages;
	buf->f_files = LONG_MAX;
	buf->f_ffree = LONG_MAX;
	buf->f_namelen = NAME_MAX;

	spin_unlock(&sbi->lock);

	return 0;
}

static void ephmfs_evict_inode(struct inode *inode)
{
	dax_break_layout_final(inode);
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void ephmfs_free_inode(struct inode *inode)
{
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	struct ephmfs_page *page;
	unsigned long index = 0;

	spin_lock(&info->mt_lock);

	mt_for_each(&info->mt, page, index, ULONG_MAX)
		ephmfs_free_page(page);

	spin_unlock(&info->mt_lock);

	mtree_destroy(&info->mt);
	kfree(info);
}

static int ephmfs_show_options(struct seq_file *m, struct dentry *root)
{
	return 0;
}

static const struct super_operations ephmfs_ops = {
	.statfs = ephmfs_statfs,
	.evict_inode = ephmfs_evict_inode,
	.free_inode = ephmfs_free_inode,
	.drop_inode = inode_just_drop,
	.show_options = ephmfs_show_options,
};

static int ephmfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	return 0;
}

static const struct kobj_type ephmfs_kobj_type = {
	.sysfs_ops = &kobj_sysfs_ops,
};

static int ephmfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root_inode;
	struct ephmfs_sb_info *sbi;
	int err;

	sbi = kzalloc(sizeof(struct ephmfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->page_size = PAGE_SIZE;
	INIT_LIST_HEAD(&sbi->dax_devs);
	spin_lock_init(&sbi->lock);
	kobject_init(&sbi->sysfs_kobj, &ephmfs_kobj_type);
	err = kobject_add(&sbi->sysfs_kobj, fs_kobj, "ephmfs");
	if (err) {
		pr_err("EphMFS: Failed to add sysfs kobject\n");
		goto err_out;
	}
	err = sysfs_create_file(&sbi->sysfs_kobj, &ephmfs_devs_attr.attr);
	if (err) {
		pr_err("EphMFS: Failed to create sysfs devs attribute\n");
		goto kobj_put;
	}

	sb->s_fs_info = sbi;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_magic = 0xE1E2E3E4;
	sb->s_op = &ephmfs_ops;
	sb->s_time_gran = 1;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;

	root_inode = ephmfs_get_inode(sb, NULL, S_IFDIR | 0755, 0);
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("EphMFS: Failed to add make root inode\n");
		err = -ENOMEM;
		goto kobj_put;
	}

	return 0;
kobj_put:
	kobject_put(&sbi->sysfs_kobj);
err_out:
	kfree(sbi);

	return err;
}

static int ephmfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, ephmfs_fill_super);
}

static void ephmfs_free_fc(struct fs_context *fc)
{
}

static const struct fs_context_operations ephmfs_context_ops = {
	.parse_param = ephmfs_parse_param,
	.get_tree = ephmfs_get_tree,
	.free = ephmfs_free_fc,
};

static int ephmfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &ephmfs_context_ops;
	return 0;
}

static void ephmfs_kill_sb(struct super_block *sb)
{
	struct ephmfs_dev_info *dev_info, *tmp;
	struct ephmfs_sb_info *sbi = EMFS_SB(sb);

	sysfs_remove_file(&sbi->sysfs_kobj, &ephmfs_devs_attr.attr);
	kobject_put(&sbi->sysfs_kobj);

	/* Free all dax device info structures */
	spin_lock(&sbi->lock);
	list_for_each_entry_safe(dev_info, tmp, &sbi->dax_devs, node) {
		list_del(&dev_info->node);
		fs_put_dax(dev_info->dax_dev, dev_info);
		kfree(dev_info->dev_name);
		kfree(dev_info);
	}
	spin_unlock(&sbi->lock);
	kfree(sbi);
	kill_anon_super(sb);
}

// Populates dev_info with the information from the dax device represented by bdev_file.
// Should be called with sbi->lock held.
static int ephmfs_populate_dev_info(struct ephmfs_dev_info *dev_info, struct dax_device *dax_dev)
{
	int ret = 0;
	struct ephmfs_page *cursor, *tmp;
	long num_base_pages;
	int dax_lock_id;
	long i;

	pr_err("EphMFS: Populating device info for dax device %s\n", dev_info->dev_name);
	dev_info->dax_dev = dax_dev;
	if (!dev_info->dax_dev) {
		pr_err("EphMFS: Failed to get dax device for block device\n");
		return -ENODEV;
	}

	// Determine how many pages are in the device
	dax_lock_id = dax_read_lock();
	num_base_pages = dax_direct_access(dev_info->dax_dev, 0, LONG_MAX / PAGE_SIZE,
		DAX_ACCESS, &dev_info->kaddr, &dev_info->pfn);
	dax_read_unlock(dax_lock_id);
	if (num_base_pages <= 0) {
		pr_err("EphMFS: Failed to access dax device for block device\n");
		return num_base_pages < 0 ? num_base_pages : -EIO;
	}

	// TODO: this will need to be updated when we start supporting more
	// than just base pages.
	dev_info->num_pages = num_base_pages;
	dev_info->free_pages = num_base_pages;

	INIT_LIST_HEAD(&dev_info->free_list);
	INIT_LIST_HEAD(&dev_info->active_list);
	spin_lock_init(&dev_info->lock);

	/* Initially place all pages on the free list */
	for (i = 0; i < dev_info->num_pages; i++) {
		struct ephmfs_page *page = kzalloc(sizeof(struct ephmfs_page), GFP_KERNEL);
		if (!page) {
			ret = -ENOMEM;
			goto err_out;
		}
		page->page_num = i;
		page->inode = NULL;
		page->dev_info = dev_info;
		spin_lock_init(&page->lock);
		list_add(&page->node, &dev_info->free_list);
	}
	return ret;

err_out:
	list_for_each_entry_safe(cursor, tmp, &dev_info->free_list, node) {
		list_del(&cursor->node);
		kfree(cursor);
	}
	return ret;
}

static int ephmfs_notify_failure(struct dax_device *dax_dev, u64 off, u64 len, int mf_flags)
{
	struct ephmfs_dev_info *dev_info = dax_holder(dax_dev);
	pr_err("EphMFS: Memory failure detected on device %s at offset %llu for length %llu, (0x%x)\n",
		dev_info->dev_name, off, len, mf_flags);

	return -EOPNOTSUPP;
}

static const struct dax_holder_operations ephmfs_dax_holder_ops = {
	.notify_failure = ephmfs_notify_failure,
};

static ssize_t ephmfs_devs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct ephmfs_sb_info *sbi = container_of(kobj, struct ephmfs_sb_info, sysfs_kobj);
	ssize_t len = 0;

	spin_lock(&sbi->lock);
	struct ephmfs_dev_info *dev_info;
	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n", dev_info->dev_name);
	}
	spin_unlock(&sbi->lock);

	return len;
}

/*
 * From John Groves
 * https://lore.kernel.org/linux-fsdevel/86694a1a663ab0b6e8e35c7b187f5ad179103482.1714409084.git.john@groves.net/
 */
static int
lookup_daxdev(char *pathname, dev_t *devno)
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname || !devno)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}

	if (!may_open_dev(&path)) {
		err = -EACCES;
		goto out_path_put;
	}

	/* If it's dax, i_rdev is struct dax_device */
	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

static struct dax_device *fs_dax_get_by_path(char *path, void *holder, const struct dax_holder_operations *ops)
{
	struct dax_device *dax_dev;
	dev_t devno;
	int err;

	err = lookup_daxdev(path, &devno);
	if (err) {
		pr_err("EphMFS: Failed to lookup dax device by path %s (err=%d)\n", path, err);
		return ERR_PTR(err);
	}

	dax_dev = dax_dev_get(devno);
	if (!dax_dev) {
		pr_err("EphMFS: Failed to get dax device for devno %llu\n", (unsigned long long)devno);
		return ERR_PTR(-ENODEV);
	}

	err = fs_dax_get(dax_dev, holder, ops);
	if (err) {
		pr_err("EphMFS: Failed to get dax device for devno %llu (err=%d)\n", (unsigned long long)devno, err);
		return ERR_PTR(err);
	}

	return dax_dev;
}

static ssize_t ephmfs_devs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	char *dev_name;
	struct ephmfs_sb_info *sbi;
	struct ephmfs_dev_info *dev_info;
	struct dax_device *dax_dev;
	int err;

	dev_name = kmalloc(count + 1, GFP_KERNEL);
	if (!dev_name)
		return -ENOMEM;
	strncpy(dev_name, buf, count);
	if (dev_name[count - 1] == '\n')
		dev_name[count - 1] = '\0';
	pr_err("EphMFS: Adding device %s\n", dev_name);

	sbi = container_of(kobj, struct ephmfs_sb_info, sysfs_kobj);

	spin_lock(&sbi->lock);
	// Make sure the device hasn't already been added
	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		if (strcmp(dev_info->dev_name, dev_name) == 0) {
			err = -EEXIST;
			goto unlock;
		}
	}

	dev_info = kzalloc(sizeof(struct ephmfs_dev_info), GFP_KERNEL);
	if (!dev_info) {
		err = -ENOMEM;
		goto unlock;
	}
	dev_info->dev_name = dev_name;

	dax_dev = fs_dax_get_by_path(dev_name, dev_info, &ephmfs_dax_holder_ops);
	if (IS_ERR(dax_dev)) {
		pr_err("EphMFS: Failed to open dax device for %s\n", dev_name);
		err = PTR_ERR(dax_dev);
		goto free_dev_info;
	}

	err = ephmfs_populate_dev_info(dev_info, dax_dev);
	if (err) {
		pr_err("EphMFS: Failed to populate device info for %s (err=%d)\n", dev_name, err);
		goto put_dax_dev;
	}

	list_add(&dev_info->node, &sbi->dax_devs);

	spin_unlock(&sbi->lock);

	return count;

put_dax_dev:
	fs_put_dax(dax_dev, dev_info);
free_dev_info:
	kfree(dev_info);
unlock:
	spin_unlock(&sbi->lock);
	kfree(dev_name);
	return err;
}

static struct kobj_attribute ephmfs_devs_attr =
__ATTR(devs, 0644, ephmfs_devs_show, ephmfs_devs_store);

static struct file_system_type ephmfs_type = {
	.owner = THIS_MODULE,
	.name = "EphMFS",
	.init_fs_context = ephmfs_init_fs_context,
	.kill_sb = ephmfs_kill_sb,
	.fs_flags = FS_USERNS_MOUNT,
};

static int __init ephmfs_init(void)
{
	return register_filesystem(&ephmfs_type);
}
module_init(ephmfs_init);

static void __exit ephmfs_exit(void)
{
	unregister_filesystem(&ephmfs_type);
}
module_exit(ephmfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bijan Tabatabai");
MODULE_DESCRIPTION("EphMFS: Filesystem used by Consumer VMs of ephemeral memory");