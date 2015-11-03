#include "nvfs.h"
#include <linux/mount.h>
#include <linux/security.h>

/*
 * Callback loop for file functions
 */
#define F_CB(op, func, ...) do {					\
	struct nvfs_callback_info       *cb;				\
	struct list_head		*tmp,				\
					*safe;				\
	list_for_each_safe(tmp, safe, &nvfs_callbacks) {		\
		cb = list_entry(tmp, struct nvfs_callback_info, next);	\
		if (cb && cb->op && cb->op->func)			\
			cb->op->func(__VA_ARGS__);			\
	}								\
} while (0)


/**
 * nvfs_llseek - call the underlying llseek function
 * @file: file struct in which to seek
 * @offset: where we're seeking to
 * @origin: what the starting point for the seek is
 */
static loff_t
nvfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t		err;
	struct file	*lower_file = NULL;

	ENTER;

	lower_file = FILE_TO_LOWER(file);
	lower_file->f_pos = file->f_pos;

	memcpy(&(lower_file->f_ra), &(file->f_ra),
			sizeof(struct file_ra_state));

	F_CB(reg_f_op, llseek, lower_file, offset, origin);

	if (lower_file->f_op && lower_file->f_op->llseek)
		err = lower_file->f_op->llseek(lower_file, offset, origin);
	else
		err = generic_file_llseek(lower_file, offset, origin);

	if (err < 0)
		goto out;

	if (err != file->f_pos) {
		file->f_pos = err;
		file->f_version++;
	}
out:
	EXIT_RET((int) err);
}


/**
 * nvfs_read - call the underlying read function
 * @file: file struct we're reading from
 * @buf: buffer into which we're reading
 * @count: number of bytes we're reading
 * @ppos: file position pointer
 */
static ssize_t
nvfs_read(struct file *file, char *buf, size_t count, loff_t *ppos)
{
	int		err = -EINVAL;
	loff_t		pos = *ppos;
	struct file	*lower_file = NULL;

	ENTER;

	lower_file = FILE_TO_LOWER(file);

	if (!lower_file->f_op || !lower_file->f_op->read)
		goto out;

	F_CB(reg_f_op, read, lower_file, buf, count, ppos);

	err = lower_file->f_op->read(lower_file, buf, count, &pos);

	if (err >= 0)
		nvfs_copy_attr_atime(file->f_dentry->d_inode,
				lower_file->f_dentry->d_inode);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	if (ppos == &file->f_pos)
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0) */
		lower_file->f_pos = *ppos = pos;

	memcpy(&(file->f_ra), &(lower_file->f_ra),
			sizeof(struct file_ra_state));

out:
	EXIT_RET(err);
}

/**
 * nvfs_write - call the underlying write function
 * @file: file into which we're writing
 * @buf: buffer from which we're writing
 * @count: how many bytes we're writing
 * @ppos: pointer to file position.
 *
 * We also need to copy/update attributes, including file offset
 *
 */
static ssize_t
nvfs_write(struct file *file, const char *buf, size_t count, loff_t *ppos)
{
	int		err = -EINVAL;
	loff_t		pos = *ppos;
	struct file	*lower_file = NULL;
	struct inode	*inode,
			*lower_inode;

	ENTER;

	lower_file = FILE_TO_LOWER(file);

	inode = file->f_dentry->d_inode;
	lower_inode = INODE_TO_LOWER(inode);

	/* adjust for append -- seek to the end of the file */
	if ((file->f_flags & O_APPEND) && (count != 0))
		pos = i_size_read(inode);

	F_CB(reg_f_op, write, lower_file, buf, count, &pos);

	if (!lower_file->f_op || !lower_file->f_op->write)
		goto out;

	if (count != 0)
		err = lower_file->f_op->write(lower_file, buf, count, &pos);
	else
		err = 0;

	/*
	 * copy ctime and mtime from lower layer attributes
	 * atime is unchanged for both layers
	 */
	if (err >= 0)
		nvfs_copy_attr_times(inode, lower_inode);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
	if (ppos == &file->f_pos)
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0) */
		lower_file->f_pos = *ppos = pos;

	if (pos > i_size_read(inode))
		i_size_write(inode, pos);

out:
	EXIT_RET(err);
}


/**
 * nvfs_readdir - call underlying readdir function via vfs_readdir
 * @file: file pointer for directory
 * @dirent: dirent to fill/return
 * @filldir: function callback to fill dirent
 */
static int
nvfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	int		err = -ENOTDIR;
	struct file	*lower_file = NULL;
	struct inode	*inode;

	ENTER;

	lower_file = FILE_TO_LOWER(file);
	inode = file->f_dentry->d_inode;
	lower_file->f_pos = file->f_pos;

	F_CB(reg_f_op, readdir, lower_file, dirent, filldir);

	err = vfs_readdir(lower_file, filldir, dirent);

	file->f_pos = lower_file->f_pos;
	if (err >= 0)
		nvfs_copy_attr_atime(inode, lower_file->f_dentry->d_inode);

	EXIT_RET(err);
}

/**
 * nvfs_poll - call the underlying poll function
 * @file: file to poll
 * @wait: pointer to waiters
 */
static unsigned int
nvfs_poll(struct file *file, poll_table *wait)
{
	unsigned int	mask = DEFAULT_POLLMASK;
	struct file	*lower_file = NULL;

	ENTER;

	lower_file = FILE_TO_LOWER(file);

	F_CB(reg_f_op, poll, lower_file, wait);

	if (!lower_file->f_op || !lower_file->f_op->poll)
		goto out;

	mask = lower_file->f_op->poll(lower_file, wait);

out:
	EXIT_RET(mask);
}

/**
 * nvfs_ioctl - call the underlying ioctl function
 * @inode: device inode
 * @file: file structure upon which ioctl is performed
 * @cmd: ioctl command
 * @arg: arg to command
 */
static int
nvfs_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
		unsigned long arg)
{
	int		val, err = 0;
	struct file	*lower_file = NULL;

	ENTER;

	switch (cmd) {
	default:
		lower_file = FILE_TO_LOWER(file);
		if (lower_file && lower_file->f_op && lower_file->f_op->ioctl) {
			F_CB(reg_f_op, ioctl, INODE_TO_LOWER(inode),
					lower_file, cmd, arg);
			err = lower_file->f_op->ioctl(INODE_TO_LOWER(inode),
					lower_file, cmd, arg);
		} else
			err	= -ENOTTY;
	}

	EXIT_RET(err);
}

/**
 * nvfs_mmap - call the underlying mmap fuction
 * @file: file to map
 * @vma: vm area structure
 */
static int
nvfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	int		err = 0;
	struct file	*lower_file = NULL;
	struct inode	*inode,
			*lower_inode;

	ENTER;

	lower_file = FILE_TO_LOWER(file);
	if (!lower_file->f_op || !lower_file->f_op->mmap) {
		err = -ENODEV;
		goto out;
	}

	F_CB(reg_f_op, mmap, lower_file, vma);

	vma->vm_file = lower_file;
	err = lower_file->f_op->mmap(lower_file, vma);
	get_file(lower_file);
	fput(file);

out:
	EXIT_RET(err);
}

/**
 * nvfs_open - call the underlying open function
 * @inode: dummy
 * @file: file to open
 */
static int
nvfs_open(struct inode *inode, struct file *file)
{
	int		lower_flags, err = 0;
	struct file	*lower_file = NULL;
	struct dentry	*lower_dentry = NULL;

	ENTER;

	FILE_TO_PRIVATE_SM(file) = kmalloc(sizeof(struct nvfs_file_info),
			GFP_KERNEL);
	if (!FILE_TO_PRIVATE(file)) {
		err = -ENOMEM;
		goto out;
	}

	lower_dentry = nvfs_lower_dentry(file->f_dentry);

	dget(lower_dentry);

	lower_flags = file->f_flags;

	/*
	** dentry_open will decrement mnt refcnt if err.
	** otherwise fput() will do an mntput() for us upon file close.
	*/
	mntget(DENTRY_TO_LVFSMNT(file->f_dentry));
	lower_file = dentry_open(lower_dentry,
			DENTRY_TO_LVFSMNT(file->f_dentry), lower_flags);

	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		goto out;
	}

	F_CB(reg_f_op, open, INODE_TO_LOWER(inode), lower_file);

	FILE_TO_LOWER(file) = lower_file;

out:
	if (err < 0 && FILE_TO_PRIVATE(file))
		kfree(FILE_TO_PRIVATE(file));

	EXIT_RET(err);
}

/**
 * nvfs_flush - call the underlying flush function
 * @file: file to flush
 */
static int
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
nvfs_flush(struct file *file)
#else
nvfs_flush(struct file *file, fl_owner_t id)
#endif
{
	int		err = 0;
	struct file	*lower_file = NULL;

	ENTER;

	lower_file = FILE_TO_LOWER(file);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
	F_CB(reg_f_op, flush, lower_file);
#else
	F_CB(reg_f_op, flush, lower_file, id);
#endif

	if (!lower_file->f_op || !lower_file->f_op->flush)
		goto out;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,16)
	err = lower_file->f_op->flush(lower_file);
#else
	err = lower_file->f_op->flush(lower_file, id);
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18) */

out:
	EXIT_RET(err);
}


/**
 * nvfs_release - call the underlying release function
 * @inode: inode needed to update i_blocks
 * @file: file to put
 */
static int
nvfs_release(struct inode *inode, struct file *file)
{
	int		err = 0;
	struct file	*lower_file = NULL;
	struct inode	*lower_inode = NULL;
	struct dentry	*lower_dentry;

	ENTER;

	lower_file = FILE_TO_LOWER(file);
	kfree(FILE_TO_PRIVATE(file));

	lower_inode = INODE_TO_LOWER(inode);

	F_CB(reg_f_op, release, lower_inode, lower_file);

	lower_dentry = lower_file->f_dentry;
	fput(lower_file);
	inode->i_blocks = lower_inode->i_blocks;

	EXIT_RET(err);
}

/**
 * nvfs_fsync - call the underlying fsync function
 * @file: file to sync, or NULL (see comments)
 * @dentry: dentry containing inode to sync
 * @datasync: flag to lower fsync
 */
static int
nvfs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	int		err = -EINVAL;
	struct file	*lower_file = NULL;
	struct dentry	*lower_dentry;

	ENTER;

	/*
	** when exporting upper file system through NFS with sync option,
	** nfsd_sync_dir() sets struct file as NULL. Use inode's
	** i_fop->fsync instead of file's. see fs/nfsd/vfs.c
	*/
	if (file == NULL) {
		lower_dentry = nvfs_lower_dentry(dentry);

		if (lower_dentry->d_inode->i_fop &&
				lower_dentry->d_inode->i_fop->fsync) {
			lock_inode(lower_dentry->d_inode);
			F_CB(reg_f_op, fsync, NULL, lower_dentry, datasync);
			err = lower_dentry->d_inode->i_fop->fsync(lower_file,
					lower_dentry, datasync);
			unlock_inode(lower_dentry->d_inode);
		}
	} else {
		if (FILE_TO_PRIVATE(file) != NULL) {
			lower_file = FILE_TO_LOWER(file);
			BUG_ON(!lower_file);

			lower_dentry = nvfs_lower_dentry(dentry);

			F_CB(reg_f_op, fsync, lower_file,
					lower_dentry, datasync);
			if (lower_file->f_op && lower_file->f_op->fsync) {
				lock_inode(lower_dentry->d_inode);
				err = lower_file->f_op->fsync(lower_file,
						lower_dentry, datasync);
				unlock_inode(lower_dentry->d_inode);
			}
		}
	}

	EXIT_RET(err);
}

/**
 * nvfs_fasync - call the lower fasync function
 * @fd: descriptor to sync
 * @file: struct file to sync
 * @flag: flag for lower fasync function
 */
static int
nvfs_fasync(int fd, struct file *file, int flag)
{
	int		err = 0;
	struct file	*lower_file = NULL;

	ENTER;
	lower_file = FILE_TO_LOWER(file);

	F_CB(reg_f_op, fasync, fd, lower_file, flag);

	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);

	EXIT_RET(err);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)

/**
 * nvfs_sendfile - call underlying sendfile function
 */
static
ssize_t
nvfs_sendfile(struct file *file, loff_t *ppos,
		size_t count, read_actor_t actor, void *target)
{
	int		err = -EINVAL;
	struct file	*lower_file = NULL;

	ENTER;

	if (FILE_TO_PRIVATE(file) != NULL)
		lower_file = FILE_TO_LOWER(file);

	F_CB(reg_f_op, sendfile, lower_file, ppos, count, actor, target);

	if (lower_file->f_op && lower_file->f_op->sendfile)
		err = lower_file->f_op->sendfile(lower_file, ppos, count,
						actor, target);

	EXIT_RET(err);
}

#endif

struct file_operations nvfs_dir_fops = {
	.read		= nvfs_read,
	.poll		= nvfs_poll,
	.mmap		= nvfs_mmap,
	.open		= nvfs_open,
	.ioctl		= nvfs_ioctl,
	.write		= nvfs_write,
	.flush		= nvfs_flush,
	.fsync		= nvfs_fsync,
	.llseek		= nvfs_llseek,
	.fasync		= nvfs_fasync,
	.readdir	= nvfs_readdir,
	.release	= nvfs_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	.sendfile	= nvfs_sendfile,
#endif
	/* not needed: readv */
	/* not needed: writev */
	/* not implemented: sendpage */
	/* not implemented: get_unmapped_area */
};

struct file_operations nvfs_main_fops = {
	.read		= nvfs_read,
	.poll		= nvfs_poll,
	.mmap		= nvfs_mmap,
	.open		= nvfs_open,
	.fsync		= nvfs_fsync,
	.flush		= nvfs_flush,
	.write		= nvfs_write,
	.ioctl		= nvfs_ioctl,
	.fasync		= nvfs_fasync,
	.llseek		= nvfs_llseek,
	.readdir	= nvfs_readdir,
	.release	= nvfs_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	.sendfile	= nvfs_sendfile,
#endif
	/* not needed: readv */
	/* not needed: writev */
	/* not implemented: sendpage */
	/* not implemented: get_unmapped_area */
};
