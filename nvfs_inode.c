#include "nvfs.h"

#define I_CB(op, func, ...) do {					\
	struct nvfs_callback_info       *cb;				\
	struct list_head		*tmp,				\
					*safe;				\
	list_for_each_safe(tmp, safe, &nvfs_callbacks) {		\
		cb = list_entry(tmp, struct nvfs_callback_info, next);  \
		if (cb && cb->op && cb->op->func)			\
			cb->op->func(__VA_ARGS__);			\
	}								\
} while (0)

/**
 * nvfs_lock_parent - get and lock a dentry's parent
 * @dentry: dentry who's parent to lock
 */
static inline struct dentry*
nvfs_lock_parent(struct dentry *dentry)
{
	struct dentry	*dir = dget(dentry->d_parent);

	ENTER;
	lock_inode(dir->d_inode);
	EXIT_RET(dir);
}

/**
 * unlock_dir - unlock a dentry's inode and put the dentry
 * @dir: dir to unlock/put
 */
static inline void
unlock_dir(struct dentry *dir)
{
	ENTER;
	unlock_inode(dir->d_inode);
	dput(dir);
	EXIT_NORET;
}

/**
 * nvfs_create - call the underlying create function
 * @dir: directory in which to create
 * @dentry: dentry to create
 * @mode: mode bits for dentry
 * @nd: nameidata for dentry
 */
static int
nvfs_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	int		err;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;
	struct vfsmount	*lower_mount;

	NVFS_ND_DECLARATIONS;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);
	lower_mount = DENTRY_TO_LVFSMNT(dentry);

	lower_dir_dentry = nvfs_lock_parent(lower_dentry);
	err = PTR_ERR(lower_dir_dentry);
	if (IS_ERR(lower_dir_dentry))
		goto out;

	NVFS_ND_SAVE_ARGS(dentry, lower_dentry, lower_mount);

	err = vfs_create(lower_dir_dentry->d_inode, lower_dentry, mode, nd);
	NVFS_ND_RESTORE_ARGS;

	if (err)
		goto out_lock;

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out_lock;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);


out_lock:
	unlock_dir(lower_dir_dentry);
	I_CB(dir_i_op, create, lower_dir_dentry->d_inode,
			lower_dentry, mode, nd);
out:
	EXIT_RET(err);
}


/**
 * nvfs_lookup - call the underlying lookup function
 * @dir: directory in which to look
 * @dentry: dentry to look for
 * @unused: unused
 */
static struct dentry *
nvfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *unused)
{
	int		err = 0;
	const char	*name;
	unsigned int	namelen;
	struct dentry	*lower_dentry = NULL,
			*lower_dir_dentry;
	struct vfsmount	*lower_mount;

	ENTER;
	lower_dir_dentry = nvfs_lower_dentry(dentry->d_parent);
	name = dentry->d_name.name;
	namelen = dentry->d_name.len;

	dentry->d_op = &nvfs_dops;

	lock_inode(lower_dir_dentry->d_inode);
	lower_dentry = lookup_one_len(name, lower_dir_dentry, namelen);
	unlock_inode(lower_dir_dentry->d_inode);

	I_CB(dir_i_op, lookup, lower_dir_dentry->d_inode, lower_dentry, unused);

	lower_mount = mntget(DENTRY_TO_LVFSMNT(dentry->d_parent));

	if (IS_ERR(lower_dentry)) {
		printk(KERN_ERR "ERR from lower_dentry!!!\n");
		err = PTR_ERR(lower_dentry);
		goto out;
	}

	nvfs_copy_attr_atime(dir, lower_dir_dentry->d_inode);
	DENTRY_TO_PRIVATE_SM(dentry) = (struct nvfs_dentry_info *)
		kmalloc(sizeof(struct nvfs_dentry_info), GFP_KERNEL);
	if (!DENTRY_TO_PRIVATE(dentry)) {
		err = -ENOMEM;
		goto out_dput;
	}
	DENTRY_TO_PRIVATE(dentry)->wdi_dentry = lower_dentry;
	DENTRY_TO_PRIVATE(dentry)->wdi_mnt = lower_mount;


	/*
	** We need to handle negative dentries
	*/
	if (!lower_dentry->d_inode) {
		d_add(dentry, NULL);
		goto out;
	}

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 1);
	if (err)
		goto out_free;

	goto out;

out_free:
	d_drop(dentry);
	kfree(DENTRY_TO_PRIVATE(dentry));
	DENTRY_TO_PRIVATE_SM(dentry) = NULL;

out_dput:
	if (lower_dentry)
		dput(lower_dentry);

out:
	EXIT_RET(ERR_PTR(err));
}


/**
 * nvfs_link - call the underlying link function
 * @old_dentry: original dentry
 * @dir: dir inode for new link
 * @new_dentry: new dentry
 */
#ifdef SUSE
static int
nvfs_link(struct dentry *old_dentry, struct inode *dir,
		struct dentry *new_dentry)
{
	int		err;
	struct dentry	*lower_old_dentry,
			*lower_new_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_old_dentry = nvfs_lower_dentry(old_dentry);
	lower_new_dentry = nvfs_lower_dentry(new_dentry);

	dget(lower_old_dentry);
	dget(lower_new_dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_new_dentry);

	I_CB(dir_i_op, link, lower_old_dentry, lower_dir_dentry->d_inode,
		lower_new_dentry);

	err = lower_dir_dentry->d_inode->i_op->link(lower_old_dentry,
			lower_dir_dentry->d_inode, lower_new_dentry);
	if (err)
		goto out_lock;

	err = nvfs_interpose(lower_new_dentry, new_dentry, dir->i_sb, 0);
	if (err)
		goto out_lock;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

	old_dentry->d_inode->i_nlink =
		INODE_TO_LOWER(old_dentry->d_inode)->i_nlink;

out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_new_dentry);
	dput(lower_old_dentry);
	if (!new_dentry->d_inode)
		d_drop(new_dentry);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_link(struct dentry *old_dentry, struct inode *dir,
		struct dentry *new_dentry)
{
	int		err;
	struct dentry	*lower_old_dentry,
			*lower_new_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_old_dentry = nvfs_lower_dentry(old_dentry);
	lower_new_dentry = nvfs_lower_dentry(new_dentry);

	dget(lower_old_dentry);
	dget(lower_new_dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_new_dentry);

	I_CB(dir_i_op, link, lower_old_dentry, lower_dir_dentry->d_inode,
			lower_new_dentry);


	err = vfs_link(lower_old_dentry,
		       lower_dir_dentry->d_inode,
		       lower_new_dentry);
	if (err || !lower_new_dentry->d_inode)
		goto out_lock;

	err = nvfs_interpose(lower_new_dentry, new_dentry, dir->i_sb, 0);
	if (err)
		goto out_lock;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

	old_dentry->d_inode->i_nlink =
		INODE_TO_LOWER(old_dentry->d_inode)->i_nlink;

out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_new_dentry);
	dput(lower_old_dentry);
	if (!new_dentry->d_inode)
		d_drop(new_dentry);

	EXIT_RET(err);
}
#endif /* SUSE */


/**
 * nvfs_unlink - call the unlink syscall
 * @dir: directory inode for dentry
 * @dentry: dentry to unlink
 */
#ifdef SUSE
static int
nvfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int		err = 0;
	struct inode	*lower_dir;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;

	lower_dir = INODE_TO_LOWER(dir);
	lower_dentry = nvfs_lower_dentry(dentry);

	I_CB(dir_i_op, unlink, lower_dir, lower_dentry);

	dget(dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);
	dget(lower_dentry);
	if (lower_dentry->d_parent->d_inode != lower_dir) {
		printk(KERN_ERR "Fixing\n");
		lower_dentry->d_parent->d_inode = lower_dir;
	}

	err = lower_dir->i_op->unlink(lower_dir, lower_dentry);
	dput(lower_dentry);

	if (!err)
		d_delete(lower_dentry);

out_lock:
	nvfs_copy_attr_times(dir, lower_dir);
	dentry->d_inode->i_nlink = INODE_TO_LOWER(dentry->d_inode)->i_nlink;
	nvfs_copy_attr_ctime(dentry->d_inode, dir);

	unlock_dir(lower_dir_dentry);

	if (!err)
		d_drop(dentry);

	dput(dentry);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int		err = 0;
	struct inode	*lower_dir;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;

	lower_dir = INODE_TO_LOWER(dir);
	lower_dentry = nvfs_lower_dentry(dentry);

	I_CB(dir_i_op, unlink, lower_dir, lower_dentry);

	dget(dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);
	dget(lower_dentry);
	if (lower_dentry->d_parent->d_inode != lower_dir) {
		printk(KERN_ERR "Fixing\n");
		lower_dentry->d_parent->d_inode = lower_dir;
	}

	err = vfs_unlink(lower_dir, lower_dentry);
	dput(lower_dentry);

	if (!err)
		d_delete(lower_dentry);

out_lock:
	nvfs_copy_attr_times(dir, lower_dir);
	dentry->d_inode->i_nlink = INODE_TO_LOWER(dentry->d_inode)->i_nlink;
	nvfs_copy_attr_ctime(dentry->d_inode, dir);

	unlock_dir(lower_dir_dentry);

	if (!err)
		d_drop(dentry);

	dput(dentry);

	EXIT_RET(err);
}
#endif /* SUSE */

/**
 * nvfs_symlink - call underlying symlink() function
 * @dir: directory inode for new symlink
 * @dentry: dentry for new symlink
 * @symname: link target
 */
#if defined(SUSE) || defined(SUSE9)
static int
nvfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int		err = 0;
	struct inode	*lower_dir_inode;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	dget(lower_dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, symlink, lower_dir_dentry->d_inode, lower_dentry,
			symname);

	lower_dir_inode = lower_dir_dentry->d_inode;
	err = lower_dir_inode->i_op->symlink(lower_dir_dentry->d_inode,
			lower_dentry, symname);

	if (err || !lower_dentry->d_inode)
		goto out_lock;
	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out_lock;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int		err = 0;
	umode_t		mode;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	dget(lower_dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, symlink, lower_dir_dentry->d_inode, lower_dentry,
			symname);

	mode = S_IALLUGO;

	err = vfs_symlink(lower_dir_dentry->d_inode, lower_dentry,
			symname, mode);

	if (err || !lower_dentry->d_inode)
		goto out_lock;
	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out_lock;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}
#endif /* SUSE */

/**
 * nvfs_mkdir - call underlying mkdir function
 * @dir: directory inode for new directory
 * @dentry: dentry for new directory
 * @mode: mode mask
 */
#ifdef SUSE
static int
nvfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, mkdir, lower_dir_dentry->d_inode,
		lower_dentry, mode);

	err = lower_dir_dentry->d_inode->i_op->mkdir(lower_dir_dentry->d_inode,
			lower_dentry, mode);
	if (err || !lower_dentry->d_inode)
		goto out;

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);
	dir->i_nlink = lower_dir_dentry->d_inode->i_nlink;


out:
	unlock_dir(lower_dir_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, mkdir, lower_dir_dentry->d_inode, lower_dentry, mode);

	err = vfs_mkdir(lower_dir_dentry->d_inode, lower_dentry, mode);
	if (err || !lower_dentry->d_inode)
		goto out;

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;

	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);
	dir->i_nlink = lower_dir_dentry->d_inode->i_nlink;


out:
	unlock_dir(lower_dir_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}
#endif

/**
 * nvfs_rmdir - call underlying rmdir function
 * @dir: containing directory
 * @dentry: dentry to rmdir
 */
#ifdef SUSE
static int
nvfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	dget(dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, rmdir, lower_dir_dentry->d_inode, lower_dentry);

	dget(lower_dentry);
	err = lower_dir_dentry->d_inode->i_op->rmdir(lower_dir_dentry->d_inode,
			lower_dentry);
	dput(lower_dentry);

	if (!err)
		d_delete(lower_dentry);

out_lock:
	nvfs_copy_attr_times(dir, lower_dir_dentry->d_inode);
	dir->i_nlink =  lower_dir_dentry->d_inode->i_nlink;

	unlock_dir(lower_dir_dentry);

	if (!err)
		d_drop(dentry);

	dput(dentry);

	EXIT_RET(err);
}

#else /* SUSE */
static int
nvfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	dget(dentry);
	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, rmdir, lower_dir_dentry->d_inode, lower_dentry);

	dget(lower_dentry);
	err = vfs_rmdir(lower_dir_dentry->d_inode, lower_dentry);
	dput(lower_dentry);

	if (!err)
		d_delete(lower_dentry);

out_lock:
	nvfs_copy_attr_times(dir, lower_dir_dentry->d_inode);
	dir->i_nlink =  lower_dir_dentry->d_inode->i_nlink;

	unlock_dir(lower_dir_dentry);

	if (!err)
		d_drop(dentry);

	dput(dentry);

	EXIT_RET(err);
}
#endif /* SUSE */

/**
 * nvfs_mknod - call underlying mknod function
 * @dir: containing directory
 * @dentry: dentry for new device
 * @mode: mode mask
 * @dev: device to make
 */
#ifdef SUSE
static int
nvfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, mknod, lower_dir_dentry->d_inode, lower_dentry,
			mode, dev);

	err = lower_dir_dentry->d_inode->i_op->mknod(lower_dir_dentry->d_inode,
			lower_dentry, mode, dev);
	if (err || !lower_dentry->d_inode)
		goto out;

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;
	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

out:
	unlock_dir(lower_dir_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	int		err = 0;
	struct dentry	*lower_dentry,
			*lower_dir_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	lower_dir_dentry = nvfs_lock_parent(lower_dentry);

	I_CB(dir_i_op, mknod, lower_dir_dentry->d_inode, lower_dentry,
			mode, dev);

	err = vfs_mknod(lower_dir_dentry->d_inode,
			lower_dentry,
			mode,
			dev);
	if (err || !lower_dentry->d_inode)
		goto out;

	err = nvfs_interpose(lower_dentry, dentry, dir->i_sb, 0);
	if (err)
		goto out;
	nvfs_copy_attr_timesizes(dir, lower_dir_dentry->d_inode);

out:
	unlock_dir(lower_dir_dentry);
	if (!dentry->d_inode)
		d_drop(dentry);

	EXIT_RET(err);
}
#endif /* SUSE */

/**
 * nvfs_rename - call underlying rename function
 * @old_dir: old containing directory
 * @old_dentry: dentry for original name
 * @new_dir: new containing directory
 * @new_dentry: dentry for new name
 */
#ifdef SUSE
static int
nvfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry)
{
	int		err;
	struct dentry	*lower_old_dentry,
			*lower_new_dentry,
			*lower_old_dir_dentry,
			*lower_new_dir_dentry;

	ENTER;

	err = -EFAULT;

	lower_old_dentry = nvfs_lower_dentry(old_dentry);
	if (!lower_old_dentry)
		goto out;

	lower_new_dentry = nvfs_lower_dentry(new_dentry);
	if (!lower_new_dentry)
		goto out;

	LOGIT(1, "Upper old d_fsdata 0x%x\n", old_dentry->d_fsdata);
	LOGIT(1, "Upper new d_fsdata 0x%x\n", new_dentry->d_fsdata);

	if (!old_dentry->d_fsdata)
		goto out;
	if (!new_dentry->d_fsdata)
		goto out;

	LOGIT(1, "dget lower old\n");
	dget(lower_old_dentry);
	LOGIT(1, "dget lower new\n");
	dget(lower_new_dentry);
	LOGIT(1, "dget old parent\n");
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	LOGIT(1, "dget new parent\n");
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	LOGIT(1, "entering CB\n");
	I_CB(dir_i_op, rename, lower_old_dir_dentry->d_inode,
			lower_old_dentry,
			lower_new_dir_dentry->d_inode,
			lower_new_dentry);

	LOGIT(1, "calling vfs_rename\n");
	err = vfs_rename(lower_old_dir_dentry->d_inode, lower_old_dentry,
			DENTRY_TO_LVFSMNT(old_dentry),
			lower_new_dir_dentry->d_inode, lower_new_dentry,
			DENTRY_TO_LVFSMNT(new_dentry));
	if (err) {
		LOGIT(1, "err, going to out_lock\n");
		goto out_lock;
	}

	LOGIT(1, "going to copy attrs\n");
	nvfs_copy_attr_all(new_dir, lower_new_dir_dentry->d_inode);
	if (new_dir != old_dir) {
		LOGIT(1, "copy attrs #2\n");
		nvfs_copy_attr_all(old_dir, lower_old_dir_dentry->d_inode);
	}

out_lock:
	LOGIT(1, "dput lower new\n");
	dput(lower_new_dentry);
	LOGIT(1, "dput lower old\n");
	dput(lower_old_dentry);

out:
	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_rename(struct inode *old_dir, struct dentry *old_dentry,
	      struct inode *new_dir, struct dentry *new_dentry)
{
	int		err;
	struct inode	*lower_dir_inode;
	struct dentry	*lower_old_dentry,
			*lower_new_dentry,
			*lower_old_dir_dentry,
			*lower_new_dir_dentry;

	ENTER;

	lower_old_dentry = nvfs_lower_dentry(old_dentry);
	lower_new_dentry = nvfs_lower_dentry(new_dentry);


	dget(lower_old_dentry);
	dget(lower_new_dentry);
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	I_CB(dir_i_op, rename, lower_old_dir_dentry->d_inode,
			lower_old_dentry, lower_new_dir_dentry->d_inode,
			lower_new_dentry);

	lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);

	lower_dir_inode = lower_old_dir_dentry->d_inode;
	err = vfs_rename(lower_old_dir_dentry->d_inode, lower_old_dentry,
			lower_new_dir_dentry->d_inode, lower_new_dentry);

	if (err)
		goto out_lock;

	nvfs_copy_attr_all(new_dir, lower_new_dir_dentry->d_inode);
	if (new_dir != old_dir)
		nvfs_copy_attr_all(old_dir, lower_old_dir_dentry->d_inode);

out_lock:
	/*
	** unlock_rename will dput the new/old parent dentries whose refcnts
	** were incremented via dget_parent above.
	*/
	dput(lower_new_dentry);
	dput(lower_old_dentry);
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);

	EXIT_RET(err);
}
#endif /* SUSE */

/**
 * nvfs_readlink - call underlying readlink function
 * @dentry: dentry to readlink
 * @buf: output buffer
 * @bufsiz: buffer size
 */
static int
nvfs_readlink(struct dentry *dentry, char *buf, int bufsiz)
{
	int		err;
	struct dentry	*lower_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	if (!lower_dentry->d_inode->i_op ||
	    !lower_dentry->d_inode->i_op->readlink) {
		err = -EINVAL;
		goto out;
	}

	I_CB(sym_i_op, readlink, lower_dentry, buf, bufsiz);

	err = lower_dentry->d_inode->i_op->readlink(lower_dentry, buf, bufsiz);
	if (err > 0)
		nvfs_copy_attr_atime(dentry->d_inode, lower_dentry->d_inode);

out:
	EXIT_RET(err);
}

/**
 * nvfs_follow_link - call underlying functions needed to follow the link
 * @dentry: dentry for link
 * @nd: nameidata for dentry
 */
#ifndef SUSE9
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
static int
#else /* 2.6.13 or newer */
static void *
#endif /* 2.6.13 */
nvfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	char *buf;
	int len = PAGE_SIZE, err;
	mm_segment_t old_fs;

	ENTER;
	/*
	** buf is allocated here, and freed when VFS calls our
	** put_link method
	*/
	err = -ENOMEM;
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		goto out;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = dentry->d_inode->i_op->readlink(dentry, buf, len);
	set_fs(old_fs);
	if (err < 0)
		goto out_free;

	buf[err] = 0;
	err = 0;
	nd_set_link(nd, buf);
	goto out;

out_free:
	kfree(buf);
out:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
	EXIT_RET(err);
#else /* 2.6.13 or newer */
	EXIT_RET(ERR_PTR(err));
#endif /* 2.6.13 or newer */
}
#endif

/**
 * nvfs_put_link - free buffer allocated by nvfs_follow_link
 * @dentry: dentry to put
 * @nd: nameidata for dentry
 */
#ifndef SUSE9
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,13)
void
nvfs_put_link(struct dentry *dentry, struct nameidata *nd)
#else /* 2.6.13 or newer */
void
nvfs_put_link(struct dentry *dentry, struct nameidata *nd, void *unused)
#endif /* 2.6.13 or newer */
{
	ENTER;
	kfree(nd_get_link(nd));
	EXIT_NORET;
}
#endif /* SUSE */

/**
 * nvfs_permission - call underlying (VFS) permission function
 * @inode: inode to check
 * @mask: what kind of check
 * @nd: nameidata for dentry we're checking
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20) || DEBIAN
static int
nvfs_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	int		err;
	struct inode	*lower_inode;
	struct dentry	*lower_dentry;
	struct vfsmount	*lower_mount;

	NVFS_ND_DECLARATIONS;

	ENTER;
	lower_inode = INODE_TO_LOWER(inode);

	if (nd) {
		lower_dentry = nvfs_lower_dentry(ND_PTR_TO_DENTRY(nd));
		lower_mount = DENTRY_TO_LVFSMNT(ND_PTR_TO_DENTRY(nd));
		NVFS_ND_SAVE_ARGS(ND_PTR_TO_DENTRY(nd), lower_dentry,
				lower_mount);
	}

	if (S_ISLNK(lower_inode->i_mode))
		I_CB(sym_i_op, permission, lower_inode, mask, nd);
	else if (S_ISDIR(lower_inode->i_mode))
		I_CB(dir_i_op, permission, lower_inode, mask, nd);
	else
		I_CB(reg_i_op, permission, lower_inode, mask, nd);

	err = permission(lower_inode, mask, nd);

	if (nd)
	    NVFS_ND_RESTORE_ARGS;

out:
	EXIT_RET(err);
}
#else
static int
nvfs_permission(struct inode *inode, int mask)
{
	int		err;
	struct inode	*lower_inode;

	printk(KERN_ERR "%s fixme\n", __FUNCTION__);
	return 0;

	ENTER;
	lower_inode = INODE_TO_LOWER(inode);

	if (S_ISLNK(lower_inode->i_mode))
		I_CB(sym_i_op, permission, lower_inode, mask);
	else if (S_ISDIR(lower_inode->i_mode))
		I_CB(dir_i_op, permission, lower_inode, mask);
	else
		I_CB(reg_i_op, permission, lower_inode, mask);

	err = inode_permission(inode, mask);

	EXIT_RET(err);
}
#endif /* > 2.6.20 */

/**
 * nvfs_setattr - call underlying setattr function
 * @dentry: dentry we're setting
 * @ia: attr structure to set
 */
#ifdef SUSE
static int
nvfs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int		err = 0;
	struct inode	*inode,
			*lower_inode;
	struct dentry	*lower_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);
	inode = dentry->d_inode;
	lower_inode = INODE_TO_LOWER(inode);

	if (S_ISLNK(lower_inode->i_mode))
		I_CB(sym_i_op, setattr, lower_dentry, ia);
	else if (S_ISDIR(lower_inode->i_mode))
		I_CB(dir_i_op, setattr, lower_dentry, ia);
	else
		I_CB(reg_i_op, setattr, lower_dentry, ia);

	lower_dentry->d_inode->i_op->setattr(lower_dentry, ia);

	nvfs_copy_attr_all(inode, lower_inode);

	EXIT_RET(err);
}

#else /* SUSE */

static int
nvfs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int		err = 0;
	struct inode	*inode,
			*lower_inode;
	struct dentry	*lower_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);
	inode = dentry->d_inode;
	lower_inode = INODE_TO_LOWER(inode);

	if (S_ISLNK(lower_inode->i_mode))
		I_CB(sym_i_op, setattr, lower_dentry, ia);
	else if (S_ISDIR(lower_inode->i_mode))
		I_CB(dir_i_op, setattr, lower_dentry, ia);
	else
		I_CB(reg_i_op, setattr, lower_dentry, ia);

	err = notify_change(lower_dentry, ia);

	nvfs_copy_attr_all(inode, lower_inode);

	EXIT_RET(err);
}
#endif /* SUSE */

/*
** nvfs_getattr - call underlying vfs_getattr function
** @mnt: wrapped struct vfsmount
** @dentry: wrapped struct dentry
** @ks: struct kstat to fill.
*/
static int
nvfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *ks)
{
	int		err;
	struct dentry	*lower_dentry;
	struct vfsmount	*lower_mount;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);
	lower_mount = DENTRY_TO_LVFSMNT(dentry);

	err = vfs_getattr(lower_mount, lower_dentry, ks);

	EXIT_RET(err);
}


/* This is lifted from fs/xattr.c */
static void *
xattr_alloc(size_t size, size_t limit)
{
	int	err = -E2BIG;
	void	*ptr = NULL;

	ENTER;

	if (size > limit) {
		ptr = ERR_PTR(-E2BIG);
		goto out;
	}

	if (!size) {
		ptr = NULL;
		goto out;
	}
	else if (size <= PAGE_SIZE)
		ptr = kmalloc((unsigned long) size, GFP_KERNEL);
	else
		ptr = vmalloc((unsigned long) size);

out:
	EXIT_RET(ptr);
}

static void
xattr_free(void *ptr, size_t size)
{
	ENTER;

	if (!size)		  /* size request, no buffer was needed */
		goto out;
	else if (size <= PAGE_SIZE)
		kfree(ptr);
	else
		vfree(ptr);
out:
	EXIT_NORET;
}

/*
** BKL held by caller.
** dentry->d_inode->i_{sem,mutex} down
*/
static ssize_t
nvfs_getxattr(struct dentry *dentry, const char *name, void *value, size_t size)
{
	int		err = -ENOTSUPP;
	struct dentry	*lower_dentry = NULL;

	ENTER;

	lower_dentry = DENTRY_TO_LOWER(dentry);

	if (lower_dentry->d_inode->i_op->getxattr) {

		if (S_ISLNK(lower_dentry->d_inode->i_mode))
			I_CB(sym_i_op, getxattr, lower_dentry,
					name, value, size);
		else if (S_ISDIR(lower_dentry->d_inode->i_mode))
			I_CB(dir_i_op, getxattr, lower_dentry,
					name, value, size);
		else
			I_CB(reg_i_op, getxattr, lower_dentry,
					name, value, size);

		lock_inode(lower_dentry->d_inode);
		err = lower_dentry->d_inode->i_op->getxattr(lower_dentry,
							    name,
							    value,
							    size);
		unlock_inode(lower_dentry->d_inode);

	}

out:
	EXIT_RET(err);
}

/*
** BKL held by caller.
** dentry->d_inode->i_{sem,mutex} down
*/
static int
nvfs_setxattr(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags)

{
	int		err = -ENOTSUPP;
	struct dentry	*lower_dentry = NULL;

	ENTER;

	lower_dentry = DENTRY_TO_LOWER(dentry);

	if (lower_dentry->d_inode->i_op->setxattr) {

		if (S_ISLNK(lower_dentry->d_inode->i_mode))
			I_CB(sym_i_op, setxattr, lower_dentry, name,
				value, size, flags);
		else if (S_ISDIR(lower_dentry->d_inode->i_mode))
			I_CB(dir_i_op, setxattr, lower_dentry, name,
					value, size, flags);
		else
			I_CB(reg_i_op, setxattr, lower_dentry, name,
					value, size, flags);

		lock_inode(lower_dentry->d_inode);
		err = lower_dentry->d_inode->i_op->setxattr(lower_dentry,
				name, value, size, flags);
		unlock_inode(lower_dentry->d_inode);
	}

out:
	EXIT_RET(err);
}

/*
** BKL held by caller.
** dentry->d_inode->i_{sem,mutex} down
*/
static int
nvfs_removexattr(struct dentry *dentry, const char *name)
{
	int		err = -ENOTSUPP;
	struct dentry	*lower_dentry = NULL;

	ENTER;

	lower_dentry = DENTRY_TO_LOWER(dentry);

	if (lower_dentry->d_inode->i_op->removexattr) {

		if (S_ISLNK(lower_dentry->d_inode->i_mode))
			I_CB(sym_i_op, removexattr, lower_dentry, name);
		else if (S_ISDIR(lower_dentry->d_inode->i_mode))
			I_CB(dir_i_op, removexattr, lower_dentry, name);
		else
			I_CB(reg_i_op, removexattr, lower_dentry, name);

		lock_inode(lower_dentry->d_inode);
		err = lower_dentry->d_inode->i_op->removexattr(lower_dentry,
				name);
		unlock_inode(lower_dentry->d_inode);
	}

out:
	EXIT_RET(err);
}

/*
** BKL held by caller.
** dentry->d_inode->i_{sem,mutex} down
*/
static ssize_t
nvfs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	int		err = -ENOTSUPP;
	struct dentry	*lower_dentry = NULL;

	ENTER;

	lower_dentry = DENTRY_TO_LOWER(dentry);

	if (lower_dentry->d_inode->i_op->listxattr) {

		if (S_ISLNK(lower_dentry->d_inode->i_mode))
			I_CB(sym_i_op, listxattr, lower_dentry, list, size);
		else if (S_ISDIR(lower_dentry->d_inode->i_mode))
			I_CB(dir_i_op, listxattr, lower_dentry, list, size);
		else
			I_CB(reg_i_op, listxattr, lower_dentry, list, size);

		lock_inode(lower_dentry->d_inode);
		err = lower_dentry->d_inode->i_op->listxattr(lower_dentry,
				list, size);
		unlock_inode(lower_dentry->d_inode);
	}

out:
	EXIT_RET(err);
}

struct inode_operations nvfs_symlink_iops = {
	.getattr	= nvfs_getattr,
	.setattr	= nvfs_setattr,
	.readlink	= nvfs_readlink,
#ifndef SUSE9
	.put_link	= nvfs_put_link,
#endif
	.setxattr	= nvfs_setxattr,
	.getxattr	= nvfs_getxattr,
	.listxattr	= nvfs_listxattr,
	.permission	= nvfs_permission,
	.removexattr	= nvfs_removexattr,
#ifndef SUSE9
	.follow_link	= nvfs_follow_link,
#endif
};

struct inode_operations nvfs_dir_iops = {
	.link		= nvfs_link,
	.mkdir		= nvfs_mkdir,
	.rmdir		= nvfs_rmdir,
	.mknod		= nvfs_mknod,
	.create		= nvfs_create,
	.lookup		= nvfs_lookup,
	.rename		= nvfs_rename,
	.unlink		= nvfs_unlink,
	.symlink	= nvfs_symlink,
	.getattr	= nvfs_getattr,
	.setattr	= nvfs_setattr,
	.setxattr	= nvfs_setxattr,
	.getxattr	= nvfs_getxattr,
	.listxattr	= nvfs_listxattr,
	.permission	= nvfs_permission,
	.removexattr	= nvfs_removexattr,
};

struct inode_operations nvfs_main_iops = {
	.getattr	= nvfs_getattr,
	.setattr	= nvfs_setattr,
	.setxattr	= nvfs_setxattr,
	.getxattr	= nvfs_getxattr,
	.listxattr	= nvfs_listxattr,
	.permission	= nvfs_permission,
	.removexattr	= nvfs_removexattr,
};
