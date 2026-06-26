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
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/statfs.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/asm.h>

#include "ephmfs.h"
#include "ephmfs_uapi.h"

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

/*
 * Zero size bytes of memory starting at addr (should be page-aligned, but I
 * guess doesn't have to).
 * Returns non-zero if the memory was not successfully zeroed.
 */
static inline u64 ephmfs_clear_page(void *addr, u64 size)
{
	/*
	 * Since we are clearing ephemeral memory that can be revoked at any
	 * time, we can't just use memset. Instead, we use rep stosb, and
	 * add that instruction to the exception table, so if an MCE occurs,
	 * we will recover, and size will be non-zero.
	 * See https://gcc.gnu.org/onlinedocs/gcc/Machine-Constraints.html for
	 * information on the ASM constraints.
	 * High level: "c" is register C, "D" is the di register, and "a" is
	 * register A.
	 */
	asm volatile(
		"1: rep stosb\n"
		"2:\n"
		_ASM_EXTABLE_TYPE(1b, 2b, EX_TYPE_DEFAULT_MCE_SAFE)
		: "+c" (size), "+D" (addr)
		: "a" (0)
		: "memory"
	);

	return size;
}

/* Call holding ephmfs_dev_info.lock */
static void ephmfs_revoke_single_page(struct ephmfs_page *page)
{
	struct ephmfs_dev_info *dev_info = page->dev_info;
	spin_lock(&page->lock);

	/*
	 * Should be handled by the check in ephmfs_revoke_page, but check
	 * again just in case.
	 */
	if (unlikely(page->revoked))
		goto unlock;

	list_del(&page->node);
	WRITE_ONCE(page->revoked, true);
	dev_info->revoked_pages++;
	if (page->on_free_list) {
		dev_info->free_pages--;
		page->on_free_list = false;
	}

unlock:
	spin_unlock(&page->lock);
}

static void ephmfs_revoke_page(struct ephmfs_page *page, bool dev_info_held)
{
	struct ephmfs_dev_info *dev_info = page->dev_info;
	struct ephmfs_page *p;
	u64 chunk_page_size;
	u64 start;
	u64 end;

	/*
	 * If this page is already revoked, it's chunk should be revoked too,
	 * so we can just return.
	 */
	if (READ_ONCE(page->revoked))
		return;

	/*
	 * ephmfs_dev_info.sbi and ephmfs_page.page_num are constant, so no
	 * lock required.
	 */
	chunk_page_size = EPHMFS_CHUNK_SIZE / dev_info->sbi->page_size;
	start = page->page_num & ~(chunk_page_size - 1);
	end = min(start + chunk_page_size, dev_info->num_pages);

	if (!dev_info_held)
		spin_lock(&dev_info->lock);

	/*
	 * When one page is revoked, all pages in the same contiguous
	 * EPHMFS_CHUNK_SIZE chunk are also revoked.
	 */
	for (u64 page_num = start; page_num < end; page_num++) {
		p = &dev_info->pages[page_num];

		ephmfs_revoke_single_page(p);
	}

	if (!dev_info_held)
		spin_unlock(&dev_info->lock);
}

static struct ephmfs_page *ephmfs_alloc_page(struct ephmfs_sb_info *sbi)
{
	struct ephmfs_page *page, *ret = NULL;
	struct ephmfs_dev_info *dev_info;
	void *kaddr;
	u64 shift;

	read_lock(&sbi->lock);
	shift = ilog2(sbi->page_size);

	/* Find a device with free pages */
	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		spin_lock(&dev_info->lock);

		if (dev_info->free_pages == 0) {
			spin_unlock(&dev_info->lock);
			continue;
		}

		page = list_first_entry(&dev_info->free_list,
			struct ephmfs_page, node);
		while (!list_entry_is_head(page, &dev_info->free_list, node)) {
			list_del(&page->node);
			list_add_tail(&page->node, &dev_info->active_list);
			page->on_free_list = false;
			dev_info->free_pages--;

			/*
			 * Attempt to clear the page. Release the dev_info lock
			 * since clearing is expensive.
			 */
			spin_unlock(&dev_info->lock);
			kaddr = dev_info->kaddr + (page->page_num << shift);
			if (!ephmfs_clear_page(kaddr, sbi->page_size)) {
				/* dev_info lock is already unlocked*/
				ret = page;
				goto out;
			}

			/* The page is bad, revoke it and try the next one */
			spin_lock(&dev_info->lock);
			ephmfs_revoke_page(page, true);

			page = list_first_entry(&dev_info->free_list,
				struct ephmfs_page, node);
		}

		spin_unlock(&dev_info->lock);
	}

out:
	read_unlock(&sbi->lock);
	return ret;
}

static void ephmfs_free_page(struct ephmfs_page *page)
{
	struct ephmfs_dev_info *dev_info = page->dev_info;

	spin_lock(&dev_info->lock);
	spin_lock(&page->lock);

	page->page_offset = 0;
	page->inode = NULL;
	if (page->revoked)
		goto unlock;

	list_del(&page->node);
	list_add(&page->node, &dev_info->free_list);
	dev_info->free_pages++;
	page->on_free_list = true;

unlock:
	spin_unlock(&page->lock);
	spin_unlock(&dev_info->lock);
}

/*
 * Allocate a page and insert it into the inode's maple tree at the given
 * offset, if a page does not already exist at that offset.
 * Returns either the existing page, the newly allocated page, or an
 * ERR_PTR(-errno) on failure.
 * If new is non NULL, *new will be set to true if a new page was allocated.
 * Takes ephmfs_inode_info.lock internally.
 */
static struct ephmfs_page *ephmfs_alloc_and_insert_page(struct ephmfs_sb_info *sbi,
	struct ephmfs_inode_info *inode_info, u64 page_offset, bool *new)
{
	struct ephmfs_page *new_page;
	struct ephmfs_page *page;
	int ret;

	if (new)
		*new = false;
	/*
	 * Before we do an expensive allocate and zero operation, make sure
	 * another thread hasn't already done so
	 */
	spin_lock(&inode_info->lock);
	page = mtree_load(&inode_info->mt, page_offset);
	spin_unlock(&inode_info->lock);
	if (page)
		return page;

	new_page = ephmfs_alloc_page(sbi);
	if (!new_page)
		return ERR_PTR(-ENOSPC);

	/* Record the file offset and inode before publishing the page into the tree. */
	new_page->page_offset = page_offset;
	new_page->inode = inode_info->inode;

	/* Make sure a racing thread hasn't beat us to the punch */
	spin_lock(&inode_info->lock);
	page = mtree_load(&inode_info->mt, page_offset);
	if (page) {
		spin_unlock(&inode_info->lock);
		ephmfs_free_page(new_page);
		return page;
	}

	ret = mtree_insert(&inode_info->mt, page_offset, new_page, GFP_ATOMIC);
	spin_unlock(&inode_info->lock);
	if (ret) {
		ephmfs_free_page(new_page);
		return ERR_PTR(ret);
	}

	if (new)
		*new = true;
	return new_page;
}

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

static int ephmfs_unmap_pages(struct inode *inode)
{
	/* unmap the whole file */
	return ephmfs_break_layout(inode, 0, LLONG_MAX);
}

static void ephmfs_remap_pages(struct inode *inode)
{
	/* Leave empty for now. Page table will repopulate via page faults. */
}

static int ephmfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct ephmfs_sb_info *sbi = EMFS_SB(inode->i_sb);
	struct ephmfs_inode_info *inode_info = EMFS_INODE(inode);
	struct ephmfs_page *page;
	bool new_page;
	u64 page_offset;
	u64 page_shift;

	page_shift = ilog2(sbi->page_size);
	page_offset = offset >> page_shift;

	iomap->flags = 0;
	iomap->offset = offset;
	iomap->length = length;

	page = ephmfs_alloc_and_insert_page(sbi, inode_info, page_offset, &new_page);
	if (IS_ERR(page))
		return PTR_ERR(page);

	if (new_page)
		iomap->flags |= IOMAP_F_NEW;
	iomap->type = IOMAP_MAPPED;
	iomap->addr = page->page_num << page_shift;
	iomap->dax_dev = page->dev_info->dax_dev;

	return 0;
}

const struct iomap_ops ephmfs_iomap_ops = {
	.iomap_begin = ephmfs_iomap_begin,
};

static void ephmfs_close(struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(vma->vm_file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);

	spin_lock(&info->lock);
	if (info->owner && same_thread_group(info->owner, current)) {
		put_task_struct(info->owner);
		info->owner = NULL;
		info->base_addr = 0;
		info->attempt_count = 0;
	}
	spin_unlock(&info->lock);
}

static int ephmfs_mremap(struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(vma->vm_file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);

	spin_lock(&info->lock);
	if (!info->owner || !same_thread_group(info->owner, current)) {
		spin_unlock(&info->lock);
		return -EPERM;
	}
	info->base_addr = vma->vm_start - (vma->vm_pgoff << PAGE_SHIFT);
	spin_unlock(&info->lock);

	return 0;
}

static vm_fault_t ephmfs_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	bool in_attempt;
	vm_fault_t result = 0;
	unsigned long pfn;

	/*
	 * Hold the invalidate lock shared across the fault so that hole-punch
	 * and truncate (which take it exclusively) cannot free a page out from
	 * under us while we are establishing a mapping for it.
	 */
	filemap_invalidate_lock_shared(inode->i_mapping);
	spin_lock(&info->lock);
	in_attempt = info->attempt_count > 0;
	spin_unlock(&info->lock);
	/* If we're not in the attempt context, we shouldn't be faulting in this page */
	if (in_attempt)
		result = dax_iomap_fault(vmf, order, &pfn, NULL, &ephmfs_iomap_ops);
	else
		result = VM_FAULT_SIGSEGV;
	filemap_invalidate_unlock_shared(inode->i_mapping);

	return result;
}

static vm_fault_t ephmfs_fault(struct vm_fault *vmf)
{
	return ephmfs_huge_fault(vmf, 0);
}

static struct vm_operations_struct ephmfs_vm_ops = {
	.close = ephmfs_close,
	.mremap = ephmfs_mremap,
	.fault = ephmfs_fault,
	.huge_fault = ephmfs_huge_fault,
	.page_mkwrite = ephmfs_fault,
	.pfn_mkwrite = ephmfs_fault,
};

static long ephmfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	long ret = 0;

	if (cmd != EPHMFS_IOCTL_ATTEMPT_ON && cmd != EPHMFS_IOCTL_ATTEMPT_OFF)
		return -ENOTTY;

	filemap_invalidate_lock(inode->i_mapping);
	spin_lock(&info->lock);

	if (!info->owner || !same_thread_group(info->owner, current)) {
		ret = -EPERM;
		goto out_unlock;
	}

	switch (cmd) {
	case EPHMFS_IOCTL_ATTEMPT_ON:
		if (info->attempt_count == 0) {
			/*
			 * Don't need to release inode_info.lock since
			 * ephmfs_remap_pages() is currently a no-op.
			 */
			ephmfs_remap_pages(inode);
		}
		info->attempt_count++;
		break;
	case EPHMFS_IOCTL_ATTEMPT_OFF:
		if (info->attempt_count == 0) {
			ret = -EINVAL;
			goto out_unlock;
		} else if (info->attempt_count == 1) {
			spin_unlock(&info->lock);
			ret = ephmfs_unmap_pages(inode);
			spin_lock(&info->lock);
			if (ret)
				goto out_unlock;
			/*
			 * Check if we raced with ephmfs_close() while we let
			 * go of inode_info->lock and it zeroed the attempt
			 * count.
			 * No need to return error in this case.
			 */
			if (info->attempt_count == 0)
				goto out_unlock;
		}
		info->attempt_count--;
		break;
	}

out_unlock:
	spin_unlock(&info->lock);
	filemap_invalidate_unlock(inode->i_mapping);
	return ret;
}

static int ephmfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	struct ephmfs_inode_info *info = EMFS_INODE(inode);

	/* Only allow mapping of regular files */
	if (!S_ISREG(inode->i_mode))
		return -EINVAL;

	spin_lock(&info->lock);
	if (info->owner) {
		spin_unlock(&info->lock);
		return -EBUSY;
	}
	info->owner = get_task_struct(current);
	info->base_addr = vma->vm_start - (vma->vm_pgoff << PAGE_SHIFT);
	spin_unlock(&info->lock);
	vma->vm_ops = &ephmfs_vm_ops;

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

			spin_lock(&info->lock);
			page = mtree_erase(&info->mt, page_offset);
			spin_unlock(&info->lock);

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

		page = ephmfs_alloc_and_insert_page(sbi, info, page_offset, NULL);
		if (IS_ERR(page))
			return PTR_ERR(page);
	}

	return 0;
}

const struct file_operations ephmfs_file_operations = {
	.unlocked_ioctl = ephmfs_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
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

	spin_lock(&info->lock);
	mt_for_each(&info->mt, page, index, ULONG_MAX) {
		mtree_erase(&info->mt, page->page_offset);
		ephmfs_free_page(page);
	}
	spin_unlock(&info->lock);
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
	info->inode = inode;
	info->owner = NULL;
	info->base_addr = 0;
	info->attempt_count = 0;
	spin_lock_init(&info->lock);
	mt_init(&info->mt);

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
	u64 revoked_pages = 0;

	read_lock(&sbi->lock);

	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		spin_lock(&dev_info->lock);
		num_pages += dev_info->num_pages;
		free_pages += dev_info->free_pages;
		revoked_pages += dev_info->revoked_pages;
		spin_unlock(&dev_info->lock);
	}

	buf->f_type = sb->s_magic;
	buf->f_bsize = PAGE_SIZE;
	buf->f_blocks = num_pages - revoked_pages;
	buf->f_bfree = free_pages;
	buf->f_bavail = free_pages;
	buf->f_files = LONG_MAX;
	buf->f_ffree = LONG_MAX;
	buf->f_namelen = NAME_MAX;

	read_unlock(&sbi->lock);

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

	spin_lock(&info->lock);

	mt_for_each(&info->mt, page, index, ULONG_MAX)
		ephmfs_free_page(page);

	spin_unlock(&info->lock);

	mtree_destroy(&info->mt);
	if (info->owner)
		put_task_struct(info->owner);
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
	/*
	 * Proactive warning for when we allow different page sizes, since a
	 * the chunk size being less than the page size could be problematic
	 * as ephemeral memory could be added/revoked at a granularity smaller
	 * than the page size.
	 */
	WARN_ON(sbi->page_size > EPHMFS_CHUNK_SIZE);
	INIT_LIST_HEAD(&sbi->dax_devs);
	rwlock_init(&sbi->lock);
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
	list_for_each_entry_safe(dev_info, tmp, &sbi->dax_devs, node) {
		list_del(&dev_info->node);
		fs_put_dax(dev_info->dax_dev, dev_info);
		kfree(dev_info->dev_name);
		vfree(dev_info->pages);
		kfree(dev_info);
	}
	kfree(sbi);
	kill_anon_super(sb);
}

// Populates dev_info with the information from the dax device represented by bdev_file.
// Should be called with sbi->lock held with write access.
static int ephmfs_populate_dev_info(struct ephmfs_dev_info *dev_info,
	struct dax_device *dax_dev, struct ephmfs_sb_info *sbi)
{
	int ret = 0;
	long num_base_pages;
	int dax_lock_id;
	long i;

	pr_err("EphMFS: Populating device info for dax device %s\n", dev_info->dev_name);
	dev_info->dax_dev = dax_dev;
	dev_info->sbi = sbi;
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
	dev_info->revoked_pages = 0;

	INIT_LIST_HEAD(&dev_info->free_list);
	INIT_LIST_HEAD(&dev_info->active_list);
	spin_lock_init(&dev_info->lock);

	dev_info->pages = vcalloc(dev_info->num_pages, sizeof(struct ephmfs_page));
	if (!dev_info->pages) {
		pr_err("EphMFS: Failed to allocate ephmfs_page structures for dax device\n");
		return -ENOMEM;
	}
	/* Initially place all pages on the free list */
	for (i = 0; i < dev_info->num_pages; i++) {
		struct ephmfs_page *page = &dev_info->pages[i];
		page->page_num = i;
		page->inode = NULL;
		page->dev_info = dev_info;
		page->revoked = false;
		page->on_free_list = true;
		spin_lock_init(&page->lock);
		list_add(&page->node, &dev_info->free_list);
	}
	return ret;
}

static int ephmfs_kill_procs(struct inode *inode, loff_t index, loff_t count, short lsb, int mf_flags)
{
	struct page *page = NULL;
	bool pre_remove = mf_flags & MF_MEM_PRE_REMOVE;
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	struct task_struct *owner;
	unsigned long addr;
	dax_entry_t cookie;
	int ret = 0;

	cookie = dax_lock_mapping_entry(inode->i_mapping, index, &page);
	if (!cookie) {
		pr_err("EphMFS: Failed to lock mapping for memory failure\n");
		return -EBUSY;
	}
	if (!page)
		goto unlock;

	if (!pre_remove)
		SetPageHWPoison(page);

	spin_lock(&info->lock);
	owner = info->owner;
	addr = info->base_addr + (index << PAGE_SHIFT);
	spin_unlock(&info->lock);

	unmap_mapping_range(inode->i_mapping, index << PAGE_SHIFT, count << PAGE_SHIFT, 0);

	if (!owner)
		goto unlock;

	if ((mf_flags & MF_ACTION_REQUIRED) && same_thread_group(owner, current))
		ret = force_sig_mceerr(BUS_MCEERR_AR, (void __user *)addr, lsb);
	else
		ret = send_sig_mceerr(BUS_MCEERR_AO, (void __user *)addr, lsb, owner);

	if (ret)
		pr_err("EphMFS: Failed to kill processes for memory failure (err=%d)\n", ret);
unlock:
	dax_unlock_mapping_entry(inode->i_mapping, index, cookie);

	return ret;
}

static int ephmfs_notify_failure(struct dax_device *dax_dev, u64 off, u64 len, int mf_flags)
{
	struct ephmfs_dev_info *dev_info = dax_holder(dax_dev);
	u64 page_shift = ilog2(dev_info->sbi->page_size);
	u64 start = off >> page_shift;
	u64 end = (off + len) >> page_shift;
	u64 count = 1UL << (page_shift - PAGE_SHIFT);
	int rc = 0;
	int ret = 0;
	pr_err("EphMFS: Memory failure detected on device %s at offset 0x%llx for length 0x%llx, (0x%x)\n",
		dev_info->dev_name, off, len, mf_flags);

	end = min(end, dev_info->num_pages);

	for (u64 i = start; i < end; i++) {
		struct ephmfs_page *page = &dev_info->pages[i];
		loff_t index;
		struct inode *inode;

		spin_lock(&page->lock);
		inode = page->inode ? igrab(page->inode) : NULL;
		index = page->page_offset << (page_shift - PAGE_SHIFT);
		spin_unlock(&page->lock);

		ephmfs_revoke_page(page, false);

		if (!inode)
			continue;

		rc = ephmfs_kill_procs(inode, index, count, page_shift, mf_flags);
		iput(inode);
		if (rc) {
			ret = rc;
			pr_err("EphMFS: Failed to kill processes for page %llu on device %s (err=%d)\n",
				i, dev_info->dev_name, rc);
		}
	}

	return ret;
}

static const struct dax_holder_operations ephmfs_dax_holder_ops = {
	.notify_failure = ephmfs_notify_failure,
};

static ssize_t ephmfs_devs_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct ephmfs_sb_info *sbi = container_of(kobj, struct ephmfs_sb_info, sysfs_kobj);
	ssize_t len = 0;

	read_lock(&sbi->lock);
	struct ephmfs_dev_info *dev_info;
	list_for_each_entry(dev_info, &sbi->dax_devs, node) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s\n", dev_info->dev_name);
	}
	read_unlock(&sbi->lock);

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

	write_lock(&sbi->lock);
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

	err = ephmfs_populate_dev_info(dev_info, dax_dev, sbi);
	if (err) {
		pr_err("EphMFS: Failed to populate device info for %s (err=%d)\n", dev_name, err);
		goto put_dax_dev;
	}

	list_add(&dev_info->node, &sbi->dax_devs);

	write_unlock(&sbi->lock);

	return count;

put_dax_dev:
	fs_put_dax(dax_dev, dev_info);
free_dev_info:
	kfree(dev_info);
unlock:
	write_unlock(&sbi->lock);
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