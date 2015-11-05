#ifndef __NVFS_H_
#define __NVFS_H_

#include <linux/version.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/buffer_head.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/poll.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/page-flags.h>
#include <linux/writeback.h>
#include <linux/page-flags.h>
#include <linux/swap.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/mman.h>

extern int nvfs_debug_lvl;

#define LOGIT(lvl, ...) do {						\
	if (nvfs_debug_lvl >= lvl)					\
		printk(KERN_NOTICE __VA_ARGS__);			\
} while (0)

#define ENTER { do {							\
	if (nvfs_debug_lvl)						\
		printk(KERN_NOTICE "In %s\n", __FUNCTION__);		\
} while (0)

#define EXIT_RET(a) do {						\
	if (nvfs_debug_lvl)						\
		printk(KERN_NOTICE "Leaving %s with %X\n",		\
				__FUNCTION__, (int)(a));		\
	return(a);							\
} while (0); }

#define EXIT_NORET do {							\
	if (nvfs_debug_lvl)						\
		printk(KERN_ERR "Leaving %s\n", __FUNCTION__);		\
} while (0); }

#define ENTER_MACRO(A) do {						\
	if (nvfs_debug_lvl)						\
		printk(KERN_ERR "Entering macro " # A "\n");		\
} while (0)

#define EXIT_MACRO(A) do {						\
	if (nvfs_debug_lvl)						\
		printk(KERN_ERR "Exiting macro " # A "\n");		\
} while (0)


#ifndef DEFAULT_POLLMASK
#define DEFAULT_POLLMASK (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM)
#endif

#define MIN(x, y) ((x < y) ? (x) : (y))
#define MAX(x, y) ((x > y) ? (x) : (y))

struct nvfs_callback_info {
	struct list_head		next;
	struct file_operations		*reg_f_op;
	struct inode_operations		*reg_i_op;
	struct inode_operations 	*dir_i_op;
	struct inode_operations		*sym_i_op;
	struct super_operations		*sb_op;
	struct dentry_operations	*d_op;
};

struct nvfs_inode_info {
	struct inode	*wii_inode;
	struct inode	vfs_inode;
};

struct nvfs_dentry_info {
	struct dentry	*wdi_dentry;
	struct vfsmount	*wdi_mnt;
};

struct nvfs_sb_info {
	struct super_block	*wsi_sb;
};

struct nvfs_file_info {
	struct file	*wfi_file;
};

#define FILE_TO_PRIVATE(file) ((struct nvfs_file_info *)((file)->private_data))
#define FILE_TO_PRIVATE_SM(file) ((file)->private_data)

#define FILE_TO_LOWER(file) ((FILE_TO_PRIVATE(file))->wfi_file)

#define INODE_TO_PRIVATE(ino) \
	(container_of(ino, struct nvfs_inode_info, vfs_inode))
#define INODE_TO_PRIVATE_SM(ino) \
	((void *)container_of(ino, struct nvfs_inode_info, vfs_inode))


#define INODE_TO_LOWER(ino) (INODE_TO_PRIVATE(ino)->wii_inode)
#define vnode2lower INODE_TO_LOWER

#define SUPERBLOCK_TO_PRIVATE(super) ((struct nvfs_sb_info *)(super)->s_fs_info)
#define SUPERBLOCK_TO_PRIVATE_SM(super) ((super)->s_fs_info)

#define SUPERBLOCK_TO_LOWER(super) (SUPERBLOCK_TO_PRIVATE(super)->wsi_sb)

#define DENTRY_TO_PRIVATE_SM(dentry) ((dentry)->d_fsdata)
#define DENTRY_TO_PRIVATE(dentry) \
	((struct nvfs_dentry_info *)(dentry)->d_fsdata)
#define DENTRY_TO_LOWER(dent) (DENTRY_TO_PRIVATE(dent)->wdi_dentry)
#define nvfs_lower_dentry(dentry) DENTRY_TO_LOWER(dentry)
#define DENTRY_TO_LVFSMNT(dent) (DENTRY_TO_PRIVATE(dent)->wdi_mnt)

extern struct list_head			nvfs_callbacks;
extern struct kmem_cache		*nvfs_inode_cachep;
extern struct file_operations		nvfs_main_fops;
extern struct file_operations		nvfs_dir_fops;
extern struct inode_operations		nvfs_main_iops;
extern struct inode_operations		nvfs_dir_iops;
extern struct inode_operations		nvfs_symlink_iops;
extern struct super_operations		nvfs_sops;
extern struct dentry_operations		nvfs_dops;
extern struct vm_operations_struct	nvfs_shared_vmops;
extern struct vm_operations_struct	nvfs_private_vmops;
extern struct address_space_operations	nvfs_aops;

extern int nvfs_interpose(struct dentry*, struct dentry*,
		struct super_block*, int);
extern int nvfs_init_inodecache(void);
extern void nvfs_destroy_inodecache(void);
extern int register_nvfs_callback(struct nvfs_callback_info *cb, int head);
extern int unregister_nvfs_callback(struct nvfs_callback_info *cb);

#define copy_inode_size(dst, src) do {					\
	i_size_write(dst, i_size_read((struct inode *) src));		\
	dst->i_blocks = src->i_blocks;					\
} while (0)


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
#define ND_PTR_TO_DENTRY(nd) nd->dentry
#define ND_PTR_TO_MNT(nd) nd->mnt
#define ND_TO_DENTRY(nd) nd.dentry
#define ND_TO_MNT(nd) nd.mnt
#else
#define ND_PTR_TO_DENTRY(nd) nd->path.dentry
#define ND_PTR_TO_MNT(nd) nd->path.mnt
#define ND_TO_DENTRY(nd) nd.path.dentry
#define ND_TO_MNT(nd) nd.path.mnt
#endif

#define NVFS_ND_DECLARATIONS	struct dentry *saved_dentry = NULL;	\
				struct vfsmount *saved_vfsmount = NULL;

#define NVFS_ND_SAVE_ARGS(this, _lower_dentry, _lower_mount) do {	\
	saved_dentry = ND_PTR_TO_DENTRY(nd);				\
	saved_vfsmount = ND_PTR_TO_MNT(nd);				\
	ND_PTR_TO_DENTRY(nd) = (_lower_dentry);				\
	ND_PTR_TO_MNT(nd) = (_lower_mount);				\
} while (0)

#define NVFS_ND_RESTORE_ARGS do {					\
	ND_PTR_TO_DENTRY(nd) = saved_dentry;				\
	ND_PTR_TO_MNT(nd) = saved_vfsmount;				\
} while (0)

static inline void
nvfs_copy_attr_atime(struct inode *dest, const struct inode *src)
{
	ENTER;
	dest->i_atime = src->i_atime;
	EXIT_NORET;
}

static inline void
nvfs_copy_attr_ctime(struct inode *dest, const struct inode *src)
{
	ENTER;
	dest->i_ctime = src->i_ctime;
	EXIT_NORET;
}

static inline void
nvfs_copy_attr_times(struct inode *dest, const struct inode *src)
{
	ENTER;
	dest->i_atime = src->i_atime;
	dest->i_mtime = src->i_mtime;
	dest->i_ctime = src->i_ctime;
	EXIT_NORET;
}

static inline void
nvfs_copy_attr_timesizes(struct inode *dest, const struct inode *src)
{
	ENTER;
	nvfs_copy_attr_times(dest, src);
	copy_inode_size(dest, src);
	EXIT_NORET;
}

static inline void
nvfs_copy_attr_all(struct inode *dest, const struct inode *src)
{
	ENTER;
	dest->i_uid = src->i_uid;
	dest->i_gid = src->i_gid;
	dest->i_mode = src->i_mode;
	dest->i_rdev = src->i_rdev;
	dest->i_nlink = src->i_nlink;
	dest->i_blkbits = src->i_blkbits;
	nvfs_copy_attr_timesizes(dest, src);
	dest->i_flags = src->i_flags;
	EXIT_NORET;
}

static inline void
lock_inode(struct inode *i)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
	down(&i->i_sem);
#else
	mutex_lock(&i->i_mutex);
#endif /* version >= 2.6.16 */
}

static inline void
unlock_inode(struct inode *i)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
	up(&i->i_sem);
#else
	mutex_unlock(&i->i_mutex);
#endif /* version >= 2.6.16 */
}

#endif /* __NVFS_H_ */
