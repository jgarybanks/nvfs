#include "nvfs.h"

struct kmem_cache *nvfs_inode_cachep;

#define S_CB(func, ...) do {						\
	struct nvfs_callback_info	*cb;				\
	struct list_head		*tmp,				\
					*safe;				\
	list_for_each_safe(tmp, safe, &nvfs_callbacks) {		\
		cb = list_entry(tmp, struct nvfs_callback_info, next);	\
		if (cb && cb->sb_op && cb->sb_op->func)			\
			cb->sb_op->func(__VA_ARGS__);			\
	}								\
} while (0)

static void
nvfs_read_inode(struct inode *inode)
{
	static struct address_space_operations nvfs_empty_aops;

	ENTER;

	INODE_TO_LOWER(inode) = NULL;

	inode->i_version++;
	inode->i_op = &nvfs_main_iops;
	inode->i_fop = &nvfs_main_fops;

	inode->i_sb->s_type->fs_flags |= FS_REQUIRES_DEV;
	inode->i_mapping->a_ops = &nvfs_empty_aops;

	EXIT_NORET;
}

static void
nvfs_put_inode(struct inode *inode)
{
	ENTER;
	/*
	** If i_count == 1, iput will decrement it and this inode
	** will be destroyed. It is currently holding a reference to
	** the lower inode. Therefore, it needs to release that reference
	** by calling iput on the lower inode. iput() _will_ do it for us
	** (by calling our clear_inode), but _only_ if i_nlink == 0.
	** The problem is, NFS keeps i_nlink == 1 for silly_rename'd files.
	** So we must set our i_nlink to 0 here to trick iput() into calling
	** our clear_inode. This needs to be tested to see if it causes
	** problems removing things with existing counts, although the only
	** case that should happen is with rmdir(dir) when dir has one link,
	** which should never happen.
	*/
	if (atomic_read(&inode->i_count) == 1)
		inode->i_nlink = 0;
	EXIT_NORET;
}


static void
nvfs_put_super(struct super_block *sb)
{
	ENTER;

	if (SUPERBLOCK_TO_PRIVATE(sb)) {
	kfree(SUPERBLOCK_TO_PRIVATE(sb));
		SUPERBLOCK_TO_PRIVATE_SM(sb) = NULL;
	}

	EXIT_NORET;
}

static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
nvfs_statfs(struct super_block *sb, struct kstatfs *buf)
{
	int			err = 0;
	struct super_block	*lower = SUPERBLOCK_TO_LOWER(sb);
#else
nvfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int			err = 0;
	struct dentry		*lower = DENTRY_TO_LOWER(dentry);
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16) */

	ENTER;

	S_CB(statfs, lower, buf);

	err = vfs_statfs(lower, buf);

	EXIT_RET(err);
}


static int
nvfs_remount_fs(struct super_block *sb, int *flags, char *data)
{
	ENTER;
	printk(KERN_ERR "Remount not implemented\n");
	EXIT_RET(-ENOSYS);
}

static void
nvfs_clear_inode(struct inode *inode)
{

	ENTER;
	iput(INODE_TO_LOWER(inode));
	EXIT_NORET;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)) || LINUX_VERSION_CODE > KERNEL_VERSION(2,6,20)
static void
nvfs_umount_begin(struct super_block *sb)
{
	struct super_block	*lower_sb;

	ENTER;

	lower_sb = SUPERBLOCK_TO_LOWER(sb);

	S_CB(umount_begin, lower_sb);

	if (lower_sb->s_op->umount_begin)
		lower_sb->s_op->umount_begin(lower_sb);

	EXIT_NORET;
}
#else /* not LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18) */
static void
nvfs_umount_begin(struct vfsmount *vfsmnt, int flags)
{
	struct vfsmount		*lower_vfsmnt;
	struct super_block	*sb,
				*lower_sb;

	ENTER;

	sb = vfsmnt->mnt_sb;
	lower_sb = SUPERBLOCK_TO_LOWER(sb);
	lower_vfsmnt = DENTRY_TO_LVFSMNT(sb->s_root);

	S_CB(umount_begin, lower_vfsmnt, flags);

	if (lower_sb->s_op->umount_begin)
		lower_sb->s_op->umount_begin(lower_vfsmnt, flags);

	EXIT_NORET;
}
#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) */

/* Called to print options in /proc/mounts */
static int
nvfs_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	ENTER;
	EXIT_RET(0);
}

static inline struct nvfs_inode_info*
NVFS_I(struct inode *inode)
{
	return container_of(inode, struct nvfs_inode_info, vfs_inode);
}

static struct inode*
nvfs_alloc_inode(struct super_block *sb)
{
	struct nvfs_inode_info	*wi;

	ENTER;
	wi = kmem_cache_alloc(nvfs_inode_cachep, GFP_KERNEL);
	if (!wi)
		return NULL;
	wi->vfs_inode.i_version = 1;

	EXIT_RET(&wi->vfs_inode);
}

static void
nvfs_destroy_inode(struct inode *inode)
{
	ENTER;
	kmem_cache_free(nvfs_inode_cachep, NVFS_I(inode));
	EXIT_NORET;
}

static void
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
init_once(void *foo, struct kmem_cache *cachep, unsigned long flags)
#else
init_once(void *foo)
#endif
{
	struct nvfs_inode_info *wi = foo;

	ENTER;
	inode_init_once(&wi->vfs_inode);
	EXIT_NORET;
}

int
nvfs_init_inodecache(void)
{
	int	err = 0;
	ENTER;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	nvfs_inode_cachep = kmem_cache_create("nvfs_inode_cache",
			sizeof(struct nvfs_inode_info), 0,
			SLAB_HWCACHE_ALIGN, init_once, NULL);
#else
	nvfs_inode_cachep = kmem_cache_create("nvfs_inode_cache",
			sizeof(struct nvfs_inode_info), 0,
			SLAB_HWCACHE_ALIGN, init_once);
#endif

	if (nvfs_inode_cachep == NULL)
		err = -ENOMEM;
	EXIT_RET(err);
}

void
nvfs_destroy_inodecache(void)
{
	kmem_cache_destroy(nvfs_inode_cachep);
}

struct super_operations nvfs_sops = {
	.statfs		= nvfs_statfs,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	.put_inode	= nvfs_put_inode,
	.read_inode	= nvfs_read_inode,
#endif
	.put_super	= nvfs_put_super,
	.remount_fs	= nvfs_remount_fs,
	.clear_inode	= nvfs_clear_inode,
	.alloc_inode	= nvfs_alloc_inode,
	.umount_begin	= nvfs_umount_begin,
	.show_options	= nvfs_show_options,
	.destroy_inode	= nvfs_destroy_inode,
};
