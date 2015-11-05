#include "nvfs.h"

#define D_CB(func, ...) do {						\
	struct nvfs_callback_info	*cb;				\
	struct list_head		*tmp,				\
					*safe;				\
	list_for_each_safe(tmp, safe, &nvfs_callbacks) {		\
		cb = list_entry(tmp, struct nvfs_callback_info, next);	\
		if (cb && cb->d_op && cb->d_op->func)			\
			cb->d_op->func(__VA_ARGS__);			\
	}								\
} while (0)

static int
nvfs_d_revalidate(struct dentry *dentry, struct nameidata *nd)
{
	int		err = 1;
	struct dentry	*lower_dentry;
	struct vfsmount	*lower_mount;

	NVFS_ND_DECLARATIONS;

	ENTER;

	lower_dentry = nvfs_lower_dentry(dentry);

	D_CB(d_revalidate, lower_dentry, nd);

	if (!lower_dentry || !lower_dentry->d_op ||
	    !lower_dentry->d_op->d_revalidate)
		goto out;

	lower_mount = DENTRY_TO_LVFSMNT(dentry);

	NVFS_ND_SAVE_ARGS(dentry, lower_dentry, lower_mount);
	err = lower_dentry->d_op->d_revalidate(lower_dentry, nd);
	NVFS_ND_RESTORE_ARGS;

out:
	EXIT_RET(err);
}


static int
nvfs_d_hash(struct dentry *dentry, struct qstr *name)
{
	int		err = 0;
	struct dentry	*lower_dentry;

	ENTER;
	lower_dentry = nvfs_lower_dentry(dentry);

	D_CB(d_hash, lower_dentry, name);

	if (!lower_dentry || !lower_dentry->d_op || !lower_dentry->d_op->d_hash)
		goto out;

	err = lower_dentry->d_op->d_hash(lower_dentry, name);

out:
	EXIT_RET(err);
}


static int
nvfs_d_compare(struct dentry *dentry, struct qstr *a, struct qstr *b)
{
	int		err;
	struct dentry	*lower_dentry;

	ENTER;

	lower_dentry = nvfs_lower_dentry(dentry);

	D_CB(d_compare, lower_dentry, a, b);

	if (lower_dentry && lower_dentry->d_op &&
			lower_dentry->d_op->d_compare)
		err = lower_dentry->d_op->d_compare(lower_dentry, a, b);
	else
		err = ((a->len != b->len) || memcmp(a->name, b->name, b->len));

	EXIT_RET(err);
}


int
nvfs_d_delete(struct dentry *dentry)
{
	int		err = 0;
	struct dentry	*lower_dentry = 0;

	ENTER;

	if (!dentry)
		goto out;

	if (!DENTRY_TO_PRIVATE(dentry))
		goto out;

	lower_dentry = DENTRY_TO_LOWER(dentry);
	if (!lower_dentry)
		goto out;

	D_CB(d_delete, lower_dentry);

	if (lower_dentry && lower_dentry->d_op &&
			lower_dentry->d_op->d_delete)
		err = lower_dentry->d_op->d_delete(lower_dentry);
out:
	EXIT_RET(err);
}


void
nvfs_d_release(struct dentry *dentry)
{
	struct dentry	*lower_dentry;

	ENTER;

	if (!dentry)
		goto out;

	if (!DENTRY_TO_PRIVATE(dentry))
		goto out;

	lower_dentry = DENTRY_TO_LOWER(dentry);

	D_CB(d_release, lower_dentry);

	mntput(DENTRY_TO_LVFSMNT(dentry));
	kfree(DENTRY_TO_PRIVATE(dentry));
	if (lower_dentry)
		dput(lower_dentry);
out:
	EXIT_NORET;
}

struct dentry_operations nvfs_dops = {
	.d_hash		= nvfs_d_hash,
	.d_delete	= nvfs_d_delete,
	.d_compare	= nvfs_d_compare,
	.d_release	= nvfs_d_release,
	.d_revalidate	= nvfs_d_revalidate,
};
