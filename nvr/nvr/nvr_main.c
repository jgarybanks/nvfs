#include "nvr.h"
#include "nvr_queue.h"
#include "../nvfs.h"

#define makedev(major, minor) (((unsigned int)major)<<8 | (unsigned int)minor)

extern int trans;
extern char **multiwrite_files;

int nvr_major_number = 0;
module_param(nvr_major_number, int, 0444);
MODULE_PARM_DESC(nvr_major_number, "Major device number");

/**
 * nvr_down - wrapper for semaphore down()
 * @s: semaphore to down
 */
asmlinkage void
nvr_down(struct semaphore *s)
{
	ENTER;
	down(s);
	EXIT_NORET;
}

/**
 * nvr_up - wrapper for semaphore up()
 * @s: semaphore to up
 */
asmlinkage void
nvr_up(struct semaphore *s)
{
	ENTER;
	up(s);
	EXIT_NORET;
}

/**
 * nvr_copy_to_user - wrapper for copy_to_user()
 * @to: output buffer
 * @from: input buffer
 * @n: bytes to copy
 */
asmlinkage long
nvr_copy_to_user(void *to, void *from, long n)
{
	ENTER;
	EXIT_RET(copy_to_user(to, from, n));
}

/**
 * nvr_copy_from_user - wrapper for copy_from_user()
 * @to: output buffer
 * @from: input buffer
 * @n: bytes to copy
 */
asmlinkage long
nvr_copy_from_user(void *to, void *from, long n)
{
	ENTER;
	EXIT_RET(copy_from_user(to, from, n));
}

/**
 * nvr_llseek - llseek NVR callback
 * @file: file to seek
 * @offset: where to seek
 * @whence: from where to seek
 *
 * We need this for sparse file support.
 */
static loff_t
nvr_llseek(struct file *file, loff_t offset, int whence)
{
	int		ret = 0,
			len;
	char		*fn = NULL,
			*cp = NULL;

	ENTER;

	if (offset >= i_size_read(file->f_dentry->d_inode)) {
		cp = kmalloc(sizeof(offset), GFP_KERNEL);
		if (!cp)
			goto out;
		memcpy(cp, &offset, sizeof(offset));
		fn = kmem_cache_alloc(names_cachep, GFP_KERNEL);
		if (!fn)
			goto out;

		nvr_dcache_to_fn(file->f_dentry, fn);
		nvr_add_name(fn, NULL, NVR_TYPE_SIZE, cp, sizeof(offset));
	}

out:
	kfree(cp);
	if (fn)
		kmem_cache_free(names_cachep, fn);

	EXIT_RET(offset);
}

/**
 * fn_trans_match - check to see if filename is covered by multiwrite
 * @filename: filename to check
 */
static int
fn_trans_match(const char *filename)
{
	int	i,
		ret = 0;
	char	*fn;
	size_t	len1,
		len2;

	ENTER;

	if (!filename)
		goto out;

	len1 = strlen(filename);

	for (i = 0; i < trans; i++) {
		fn = multiwrite_files[i];
		len2 = strlen(fn);

		if (len2 < len1) {
			if (strncmp(fn, filename, len2) == 0) {
				ret = 1;
				goto out;
			}
		} else if (len1 == len2) {
			if (strcmp(fn, filename) == 0) {
				ret = 1;
				goto out;
			}
		}
	}
out:
	EXIT_RET(ret);
}

/**
 * nvr_write_trans - handle multiwrite write()s
 * @fn: filename being modified
 * @buf: user supplied buffer
 * @len: size of buffer
 * @oldpos: position at which write is happening
 * @numbytes: unused, for compatibility
 */
static int
nvr_write_trans(const char *fn, char *buf, size_t len,
		loff_t oldpos, ssize_t numbytes)
{
	int			bufsize;
	char			*cp,
				*mybuf;
	struct cdi_record	*r;

	bufsize = sizeof(oldpos) + sizeof(len);
	cp = mybuf = kmalloc(bufsize, GFP_KERNEL);
	memcpy(cp, &oldpos, sizeof(oldpos));
	cp += sizeof(oldpos);
	memcpy(cp, &len, sizeof(len));

	r = nvr_get_repitem(fn, strlen(fn), NULL, TYPE_DBDATA, mybuf,
				bufsize, buf);
	kfree(mybuf);
	nvr_add_queue(r);
}

/**
 * nvr_write - handle write() of regular file
 * @file: file being written
 * @ubuf: user buffer
 * @count: bytes to write
 * @ppos: file offset (before write)
 */
static int
nvr_write(struct file *file, const char *ubuf, size_t count, loff_t *ppos)
{
	int	len,
		bufsize;
	char	*fn = NULL,
		*cp = NULL,
		*buf = NULL;
	loff_t	pos;

	ENTER;

	pos = *ppos;
	bufsize = sizeof(pos) + sizeof(count);
	cp = buf = kmalloc(bufsize, GFP_KERNEL);
	if (!cp)
		goto free_and_out;
	memcpy(cp, &pos, sizeof(pos));
	cp += sizeof(pos);
	memcpy(cp, &count, sizeof(count));

	fn = kmem_cache_alloc(names_cachep, GFP_KERNEL);
	if (!fn)
		goto free_and_out;

	len = nvr_dcache_to_fn(file->f_dentry, fn);
	if (fn_trans_match(fn))
		nvr_write_trans(fn, ubuf, count, pos, 0);
	else
		nvr_add_name(fn, NULL, NVR_TYPE_DATA, buf, bufsize);

free_and_out:
	kfree(buf);
	if (fn)
		kmem_cache_free(names_cachep, fn);

	EXIT_RET(0);
}

/**
 * nvr_link - handle link() system call
 * @odentry: old (existing) dentry
 * @dir: unused
 * @ndentry: new dentry
 */
static int
nvr_link(struct dentry *odentry, struct inode *dir, struct dentry *ndentry)
{
	ENTER;
	nvr_add_dentry(odentry, NVR_TYPE_COPYALL, NULL, NULL, 0);
	nvr_add_dentry(ndentry, NVR_TYPE_COPYALL, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_mkdir - handle mkdir() system call
 * @dir: unused
 * @dentry: dentry for new directory
 * @mode: unused
 */
static int
nvr_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_rmdir - handle rmdir()
 * @dir: unused
 * @dentry: dentry being removed
 */
static int
nvr_rmdir(struct inode *dir, struct dentry *dentry)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_UNLINK, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_mknod - handle mknod()
 * @dir: unused
 * @dentry: dentry being created
 * @mode: unused
 * @dev: unused
 */
static int
nvr_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_unlink - handle unlink()
 * @dir: unused
 * @dentry: dentry being unlinked
 */
static int
nvr_unlink(struct inode *dir, struct dentry *dentry)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_rename - handle rename() syscall
 * @odir: unused
 * @odentry: old dentry
 * @ndir: unused
 * @ndentry: new dentry
 */
static int
nvr_rename(struct inode *odir, struct dentry *odentry,
		struct inode *ndir, struct dentry *ndentry)
{
	int	o_len,
		n_len,
		buflen;
	char	*o_fn = NULL,
		*n_fn = NULL,
		*dbuf = NULL,
		*cp;

	ENTER;
	o_fn = kmem_cache_alloc(names_cachep, GFP_KERNEL);
	if (!o_fn)
		goto free_and_out;
	o_len = nvr_dcache_to_fn(odentry, o_fn);

	n_fn = kmem_cache_alloc(names_cachep, GFP_KERNEL);
	if (!n_fn)
		goto free_and_out;
	n_len = nvr_dcache_to_fn(ndentry, n_fn);

	buflen = 1 + n_len + sizeof(n_len);
	cp = dbuf = kmalloc(buflen, GFP_KERNEL);
	if (!cp)
		goto free_and_out;
	memcpy(cp, &n_len, sizeof(n_len));
	cp += sizeof(n_len);

	memcpy(cp, n_fn, n_len);
	cp +=  n_len;
	*cp = '\0';
	nvr_add_name(o_fn, NULL, NVR_TYPE_RENAME, dbuf, buflen);

free_and_out:
	kfree(dbuf);
	if (o_fn)
		kmem_cache_free(names_cachep, o_fn);
	if (n_fn)
		kmem_cache_free(names_cachep, n_fn);

	EXIT_RET(0);
}

/**
 * nvr_symlink - handle symlink()
 * @dir: unused
 * @dentry: dentry being created
 * @symname: unused
 */
static int
nvr_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_setattr - handle attribute updates (timestamps etc)
 * @dentry: dentry being modified
 * @ia: attribute structure
 */
static int
nvr_setattr(struct dentry *dentry, struct iattr *ia)
{
	unsigned int	flags;

	ENTER;
	flags = ia->ia_valid;

	if (flags & ATTR_SIZE) {
		int	bufsize;
		char	*cp;
		loff_t	ia_size;

		ia_size = ia->ia_size;
		bufsize = sizeof(ia->ia_size);
		cp = kmalloc(bufsize, GFP_KERNEL);
		if (!cp)
			goto out;
		memcpy(cp, &ia_size, bufsize);
		nvr_add_dentry(dentry, NVR_TYPE_SIZE, NULL, cp, bufsize);
		kfree(cp);
	} else
		nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);

out:
	EXIT_RET(0);
}

/**
 * nvr_setxattr - handle extended attribute set
 * @dentry: dentry being modified
 * @name: unused
 * @value: unused
 * @size: unused
 * @flags: unused
 */
static int
nvr_setxattr(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

/**
 * nvr_removexattr - handle extended attribute removal
 * @dentry: dentry being modified
 * @name: unused
 */
static int
nvr_removexattr(struct dentry *dentry, const char *name)
{
	ENTER;
	nvr_add_dentry(dentry, NVR_TYPE_MDATA, NULL, NULL, 0);
	EXIT_RET(0);
}

struct file_operations f_op = {
	.write		= nvr_write,
	.llseek		= nvr_llseek,
};

struct inode_operations i_dir_op = {
	.link		= nvr_link,
	.mkdir		= nvr_mkdir,
	.rmdir		= nvr_rmdir,
	.mknod		= nvr_mknod,
	.unlink		= nvr_unlink,
	.rename		= nvr_rename,
	.symlink	= nvr_symlink,
	.setattr	= nvr_setattr,
	.setxattr	= nvr_setxattr,
	.removexattr	= nvr_removexattr,
};

struct inode_operations i_reg_op = {
	.setattr	= nvr_setattr,
	.setxattr	= nvr_setxattr,
	.removexattr	= nvr_removexattr,
};

struct inode_operations i_sym_op = {
	.setattr	= nvr_setattr,
	.setxattr	= nvr_setxattr,
	.removexattr	= nvr_removexattr,
};

struct nvfs_callback_info ci = {
	.reg_f_op	= &f_op,
	.dir_i_op	= &i_dir_op,
	.reg_i_op	= &i_reg_op,
};

static struct file_operations nvr_dev_f_op;

/**
 * nvr_setup_device - register our character device
 *
 * This sets the major_dev value which is retrived via /sys in
 * userspace, where the actual device file is created
 */
int
nvr_setup_device(void)
{
	int major_dev = 0;

	ENTER;
	nvr_dev_f_op.owner = THIS_MODULE;
	nvr_dev_f_op.ioctl = nvr_dev_ioctl;

	major_dev = register_chrdev(0, "nvr", &nvr_dev_f_op);
	EXIT_RET(major_dev);
}

/**
 * init_nvr - initialize the kernel module
 *
 * Handles creating the device, callback registration, etc.
 */
static int __init
init_nvr(void)
{
	int		err = 0,
			major_dev;

	ENTER;

	printk(KERN_NOTICE "Initializing NVR kernel module\n");
	INIT_LIST_HEAD(&nvr_sys_queue);
	major_dev = nvr_setup_device();
	if (major_dev)
		nvr_major_number = major_dev;
	else {
		printk(KERN_ERR "Unable to create device file\n");
		err = -EINVAL;
		goto out;
	}

	printk(KERN_NOTICE "Registering NVR callbacks\n");
	register_nvfs_callback(&ci, 0);

out:
	EXIT_RET(err);
}

/**
 * exit_nvr - unregister callbacks and cleanup
 */
static void __exit
exit_nvr(void)
{
	printk(KERN_NOTICE "Unregistering NVR callbacks\n");
	unregister_nvfs_callback(&ci);
}

MODULE_AUTHOR("Justin Banks <justinb@bakbone.com>");
MODULE_DESCRIPTION("NetVault Replicator Filesystem Plugin");
MODULE_LICENSE("Dual BSD/GPL");

int nvr_debug_lvl = 0;
module_param(nvr_debug_lvl, int, 0644);
MODULE_PARM_DESC(nvr_debug_lvl, "Debug level");

module_init(init_nvr)
module_exit(exit_nvr)
