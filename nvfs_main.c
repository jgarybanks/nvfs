#include "nvfs.h"

struct list_head nvfs_callbacks;

/**
 * nvfs_interpose - stack dentries
 * @lower_dentry: "real" filesystem dentry
 * @dentry: upper "stacked" dentry
 * @sb: superblock containing @dentry
 * @flag: add or instantiate
 */
int
nvfs_interpose(struct dentry *lower_dentry, struct dentry *dentry,
		struct super_block *sb, int flag)
{
	int		err = 0;
	struct inode	*inode,
			*lower_inode;

	ENTER;

	lower_inode = lower_dentry->d_inode;

	if (lower_inode->i_sb != SUPERBLOCK_TO_LOWER(sb)) {
		err = -EXDEV;
		goto out;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	inode = iget(sb, lower_inode->i_ino);
#else
	inode = iget_locked(sb, lower_inode->i_ino);
#endif

	if (!inode) {
		err = -EACCES;
		goto out;
	}

	if (INODE_TO_LOWER(inode) == NULL)
		INODE_TO_LOWER(inode) = igrab(lower_inode);


	if (S_ISLNK(lower_inode->i_mode))
		inode->i_op = &nvfs_symlink_iops;
	else if (S_ISDIR(lower_inode->i_mode))
		inode->i_op = &nvfs_dir_iops;

	if (S_ISDIR(lower_inode->i_mode))
		inode->i_fop = &nvfs_dir_fops;


	if (special_file(lower_inode->i_mode))
		init_special_inode(inode, lower_inode->i_mode,
				lower_inode->i_rdev);


	if (inode->i_mapping->a_ops != lower_inode->i_mapping->a_ops)
		inode->i_mapping->a_ops = lower_inode->i_mapping->a_ops;

	if (flag)
		d_add(dentry, inode);
	else
		d_instantiate(dentry, inode);

	nvfs_copy_attr_all(inode, lower_inode);
out:
	EXIT_RET(err)
}

/**
 * nvfs_parse_options - get what amounts to our block device
 * @sb: our superblock
 * @name: the name of our mount device
 * @lower_root: output parameter for the root dentry
 * @lower_mount: output parameter for the underlying vfsmount
 */
int
nvfs_parse_options(struct super_block *sb, char *name,
		struct dentry **lower_root, struct vfsmount **lower_mount)
{
	int			err = 0;
	struct nameidata	nd;

	ENTER;

	err = path_lookup(name, LOOKUP_FOLLOW, &nd);
	if (err)
		goto out;

	*lower_root = ND_TO_DENTRY(nd);
	*lower_mount = ND_TO_MNT(nd);
out:
	EXIT_RET(err);
}

/**
 * nvfs_read_super - read our superblock
 * @sb: upper superblock
 * @dname: device name
 * @silent: not used
 */
static int
nvfs_read_super(struct super_block *sb, void *dname, int silent)
{
	int			err = 0;
	struct dentry		*lower_root = NULL;
	struct vfsmount		*lower_mount = NULL;
	const struct qstr	name = { .name = "/", .len = 1 };

	ENTER;

	if (!dname) {
		err = -EINVAL;
		goto out_no_raw;
	}

	SUPERBLOCK_TO_PRIVATE_SM(sb) =
		kmalloc(sizeof(struct nvfs_sb_info), GFP_KERNEL);
	if (!SUPERBLOCK_TO_PRIVATE(sb)) {
		printk(KERN_WARNING "%s: out of memory\n", __FUNCTION__);
		err = -ENOMEM;
		goto out;
	}
	memset(SUPERBLOCK_TO_PRIVATE(sb), 0, sizeof(struct nvfs_sb_info));

	err = nvfs_parse_options(sb, dname, &lower_root, &lower_mount);
	if (err)
		goto out_free;

	SUPERBLOCK_TO_LOWER(sb) = lower_root->d_sb;

	sb->s_maxbytes = lower_root->d_sb->s_maxbytes;
	sb->s_export_op = lower_root->d_sb->s_export_op;

	sb->s_op = &nvfs_sops;

	sb->s_root = d_alloc(NULL, &name);
	if (IS_ERR(sb->s_root)) {
		printk(KERN_WARNING "nvfs_read_super: d_alloc failed\n");
		err = -ENOMEM;
		goto out_dput;
	}

	sb->s_root->d_op = &nvfs_dops;
	sb->s_root->d_sb = sb;
	sb->s_root->d_parent = sb->s_root;

	DENTRY_TO_PRIVATE_SM(sb->s_root) =
		kmalloc(sizeof(struct nvfs_dentry_info), GFP_KERNEL);

	if (!DENTRY_TO_PRIVATE(sb->s_root)) {
		err = -ENOMEM;
		goto out_dput2;
	}
	DENTRY_TO_LOWER(sb->s_root) = lower_root;
	DENTRY_TO_LVFSMNT(sb->s_root) = lower_mount;

	err = nvfs_interpose(lower_root, sb->s_root, sb, 0);
	if (err)
		goto out_dput2;

	goto out;

out_dput2:
	dput(sb->s_root);
out_dput:
	dput(lower_root);
out_free:
	mntput(lower_mount);
	kfree(SUPERBLOCK_TO_PRIVATE(sb));
	SUPERBLOCK_TO_PRIVATE_SM(sb) = NULL;
out:
out_no_raw:
	EXIT_RET(err);
}



/**
 * nvfs_get_sb - get our sb
 * @fs_type: our filesystem type struct
 * @flags: passed to get_sb_nodev
 * @dev_name: passed to get_sb_nodev
 * @raw_data: passed to get_sb_nodev
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18)
static struct super_block *nvfs_get_sb(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *raw_data)
{

	return get_sb_nodev(fs_type, flags, dev_name, nvfs_read_super);
}
#else
static int nvfs_get_sb(struct file_system_type *fs_type,
		int flags, const char *dev_name,
		void *raw_data, struct vfsmount *mnt)
{

	return get_sb_nodev(fs_type, flags, (char *)dev_name,
			nvfs_read_super, mnt);
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(2,6,18) */

/**
 * nvfs_kill_block_super - shut down our superblock
 * @sb: superblock to shut down
 */
void
nvfs_kill_block_super(struct super_block *sb)
{
	generic_shutdown_super(sb);
}

static struct file_system_type nvfs_fs_type = {
	.name		= "nvfs",
	.owner		= THIS_MODULE,
	.get_sb		= nvfs_get_sb,
	.kill_sb	= nvfs_kill_block_super,
	.fs_flags	= 0,
};

/**
 * register_nvfs_callbacks - allow FS plugins to register a callback
 * @callback: callback to register
 * @head: whether to put this callback on the front or back of the list
 */
int
register_nvfs_callback(struct nvfs_callback_info *callback, int head)
{
	int	err = 0;

	ENTER;

	if (callback) {
		INIT_LIST_HEAD(&callback->next);
		if (head)
			list_add(&callback->next, &nvfs_callbacks);
		else
			list_add_tail(&callback->next, &nvfs_callbacks);
	} else
		err = -EINVAL;

	EXIT_RET(err);
}
EXPORT_SYMBOL(register_nvfs_callback);

/**
 * unregister_nvfs_callbacks - allow a FS plugin to unregister callback
 * @callback: callback to unregister
 */
int
unregister_nvfs_callback(struct nvfs_callback_info *callback)
{
	int				err = 0;
	struct nvfs_callback_info	*ptr;

	ENTER;

	if (callback) {
		struct list_head	*tmp,
					*safe;
		list_for_each_safe(tmp, safe, &nvfs_callbacks) {
			ptr = list_entry(tmp, struct nvfs_callback_info, next);
			if (ptr == callback)
				list_del_init(&ptr->next);
		}
	}
	EXIT_RET(err);
}
EXPORT_SYMBOL(unregister_nvfs_callback);

/**
 * init_nvfs_fs - initialize FS driver
 */
static int __init
init_nvfs_fs(void)
{
	int	err = 0;

	ENTER;

	printk(KERN_NOTICE "Registering nvfs filesystem module\n");

	INIT_LIST_HEAD(&nvfs_callbacks);
	err = nvfs_init_inodecache();
	if (err)
		goto out1;
	err = register_filesystem(&nvfs_fs_type);
	if (err)
		goto out;
	goto out1;
out:
	nvfs_destroy_inodecache();
out1:
	EXIT_RET(err);
}

/**
 * exit_nvfs_fs - shut down FS driver
 */
static void __exit
exit_nvfs_fs(void)
{
	printk(KERN_NOTICE "Unregistering nvfs filesystem module\n");
	nvfs_destroy_inodecache();
	unregister_filesystem(&nvfs_fs_type);
}

MODULE_AUTHOR("Justin Banks");
MODULE_DESCRIPTION("nvfs Filesystem");
MODULE_LICENSE("Dual BSD/GPL");

int nvfs_debug_lvl = 0;
EXPORT_SYMBOL(nvfs_debug_lvl);
module_param(nvfs_debug_lvl, int, 0644);
MODULE_PARM_DESC(nvfs_debug_lvl, "Debug level");

module_init(init_nvfs_fs)
module_exit(exit_nvfs_fs)
