This is a stackable filesystem that allows other kernel modules to register
to receive notifications of filesystem events. It can be mounted anywhere in
the filesystem, including mounting directories on top of themselves. For 
example, mounting /data on top of /data is allowed like so :

mount -t nvfs /data /data

This ensures that any modifications that happen in or under the directory
/data will be available to any module that has registered callbacks via
the register_nvfs_callback function. The register_nvfs_callback function
takes a single argument of type nvfs_callback_info which is defined as


struct nvfs_callback_info {
        struct list_head                next;
        struct dentry_operations        *d_op;
        struct super_operations         *sb_op;
        struct file_operations          *reg_f_op;
        struct inode_operations         *reg_i_op;
        struct inode_operations         *dir_i_op;
        struct inode_operations         *sym_i_op;
};

All callbacks occur before the actual filesystem operation occurs, with no
locking other than that of the VFS layer of the normal linux kernel, and
all functions are called with the normal parameters. The nvfs filesystem
can be mounted at any time, including at boot time, with the appropriate
fstab entry, as so :


/src                    /src                    nvfs    defaults        0 0

This will allow all data and metadata changes that occur at or under the
directory /src to be available to users of the nvfs filesystem.

An example module works like this :

struct file_operations f_op = {
        .write          = my_write,
};

struct inode_operations i_dir_op = {
        .link           = my_link,
        .mkdir          = my_mkdir,
        .rmdir          = my_rmdir,
        .mknod          = my_mknod,
        .unlink         = my_unlink,
        .rename         = my_rename,
        .symlink        = my_symlink,
        .setattr        = my_setattr,
        .setxattr       = my_setxattr,
        .removexattr    = my_removexattr,
};

struct inode_operations i_reg_op = {
        .setattr        = my_setattr,
        .setxattr       = my_setxattr,
        .removexattr    = my_removexattr,
};

struct inode_operations i_sym_op = {
        .setattr        = my_setattr,
        .setxattr       = my_setxattr,
        .removexattr    = my_removexattr,
};

struct nvfs_callback_info ci = {
        .reg_f_op       = &f_op,
        .dir_i_op       = &i_dir_op,
        .reg_i_op       = &i_reg_op,
};

static int __init
init_me(void)
{
	register_nvfs_callback(&ci, 0);
}

static void __exit
exit_me(void)
{
        unregister_nvfs_callback(&ci);
}

module_init(init_me)
module_exit(exit_me)

All functions for which callbacks are defined will be called prior to the
invocation of the underlying operation, and may modify any parameters
passed to them. The second parameter to the register_nvfs_callback function
determines whether the registered callback functions will be added to the
front of the list of callbacks, or the last.

