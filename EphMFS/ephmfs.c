#include <linux/kernel.h>
#include <linux/dax.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/kobject.h>
#include <linux/maple_tree.h>
#include <linux/mm.h>
#include <linux/module.h>
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

static vm_fault_t ephmfs_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	return VM_FAULT_SIGBUS;
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
	return -EOPNOTSUPP;
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

const struct inode_operations ephmfs_file_inode_ops = {
	.getattr = simple_getattr,
	.setattr = simple_setattr,
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

	buf->f_type = sb->s_magic;
	buf->f_bsize = PAGE_SIZE;
	buf->f_blocks = 0;
	buf->f_bfree = 0;
	buf->f_files = LONG_MAX;
	buf->f_ffree = LONG_MAX;
	buf->f_namelen = NAME_MAX;

	return 0;
}

static void ephmfs_free_inode(struct inode *inode)
{
	struct ephmfs_inode_info *info = EMFS_INODE(inode);
	kfree(info);
}

static int ephmfs_show_options(struct seq_file *m, struct dentry *root)
{
	return 0;
}

static const struct super_operations ephmfs_ops = {
	.statfs = ephmfs_statfs,
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

	sbi->num_pages = 0;
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
		fs_put_dax(dev_info->dax_dev, NULL);
		fput(dev_info->bdev_file);
		kfree(dev_info->dev_name);
		kfree(dev_info);
	}
	spin_unlock(&sbi->lock);
	kfree(sbi);
	kill_anon_super(sb);
}

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

static ssize_t ephmfs_devs_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	return -EOPNOTSUPP;
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