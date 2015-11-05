#include "nvr.h"
#include "nvr_queue.h"
#include "nvr_device.h"

extern int trans;

char *dbdir = NULL;
struct list_head nvr_sys_queue;
DECLARE_MUTEX(ritem_lock);
int repitem_count = 0;

#define QUEUE_HWAT_MARK 20000

extern int nvr_debug_lvl;

/**
 * log_record - debug logging function
 */
static int
log_record(struct cdi_record *r)
{
	if (nvr_debug_lvl && r)
		printk(KERN_NOTICE "MOD %s[%d]\n", r->name, r->type);
	return 0;
}

/**
 * nvr_file_write - write a file from kernelspace.
 * @nm: filename to write
 * @buf: buffer to write
 * @len: number of bytes to write
 *
 * Used to create sequence files.
 */
size_t
nvr_file_write(const char *nm, const char *buf, size_t len)
{
	uid_t			o_uid,
				o_fsuid;
	gid_t			o_gid,
				o_fsgid;
	size_t			err;
	struct file		*file;
	mm_segment_t		mm;
	struct fs_struct	*fs;
	struct task_struct	*me;

	mm = get_fs();
	set_fs(get_ds());

	me = current;
	fs = me->fs;
	me->fs = init_task.fs;

	o_uid = me->uid;
	o_fsuid = me->fsuid;
	o_gid = me->gid;
	o_fsgid = me->fsgid;

	me->uid = me->fsuid = me->gid = me->fsgid = 0;

	file = filp_open(nm, O_CREAT|O_EXCL, 0400);

	me->uid = o_uid;
	me->gid = o_gid;
	me->fsuid = o_fsuid;
	me->fsgid = o_fsgid;

	err = PTR_ERR(file);
	if (IS_ERR(file))
		goto out;
	err = file->f_op->write(file, buf, len, &file->f_pos);
	if (err != len)
		printk(KERN_ERR "short write of %s (%ld/%ld)\n", nm, err, len);

	filp_close(file, NULL);

out:
	me->fs = fs;
	set_fs(mm);
	return err;
}

/**
 * nvr_get_repitem - create a repitem
 * @s: filename for repitem
 * @len: length of filename
 * @notused: not used
 * @type: type flag
 * @buf: type descriptor buffer
 * @blen: size of @buf
 * @ubuf: unused
 */
struct cdi_record*
nvr_get_repitem(const char *s, int len, void *notused, int type,
		const unsigned char *buf, int blen, unsigned char *ubuf)
{
	char			*fn;
	struct timeval		now;
	struct cdi_record	*r;

	r = kmalloc(sizeof(struct cdi_record), GFP_KERNEL);
	if (!r)
		goto out_free;

	do_gettimeofday(&now);
	r->timestamp = CONVERT_TIMEVAL(now);

	r->name = kmalloc(len + 1, GFP_KERNEL);
	if (!r->name)
		goto out_free;

	memcpy(r->name, s, len);
	r->name[len] = '\0';
	r->len = len + 1;

	r->type = type;

	if (buf && blen) {
		r->buf = kmalloc(blen, GFP_KERNEL);
		r->bsize = blen;
		memcpy(r->buf, buf, blen);
	} else {
		r->buf = NULL;
		r->bsize = 0;
	}

	INIT_LIST_HEAD(&r->cd_queue);
	r->pid = -1;
	nvr_down(&ritem_lock);
	r->sequence = ++repitem_seq;
	repitem_count++;
	nvr_up(&ritem_lock);

	if (trans && type == NVR_TYPE_DBDATA) {
		fn = kmem_cache_alloc(names_cachep, GFP_KERNEL);
		if (dbdir)
			sprintf(fn, "%s/%lld", dbdir, r->sequence);
		else
			sprintf(fn, "/usr/netvault/replicator/var/db/%lld",
				r->sequence);
		nvr_file_write(fn, buf, blen);
		kmem_cache_free(names_cachep, fn);
	} else {
		r->udata = NULL;
		r->ulen = 0;
	}

	goto out;

out_free:
	kfree(r->name);
	kfree(r);
out:
	return r;
}

/**
 * nvr_put_repitem - free/put a repitem
 * @r: repitem to put
 */
void
nvr_put_repitem(struct cdi_record *r)
{
	if (r) {
		kfree(r->name);
		kfree(r->buf);
		kfree(r);
		nvr_down(&ritem_lock);
		repitem_count--;
		nvr_up(&ritem_lock);
	}
}

/**
 * item_name_okay - shortcut some obvious and frequent items we don't care about
 * @fname: filename being modified
 */
static inline int
item_name_okay(unsigned char *fname)
{
	return strcmp(fname, "/dev/") != 0 &&
		strcmp(fname, "/proc/") != 0 &&
		strcmp(fname, "/usr/netvault/replicator/") != 0;
}

/**
 * coalesce_sizes - handle adjacent file size changes
 * @r: current repitem
 *
 * This makes multiple size changes much more efficient, as long as they're 
 * adjacent in both time and file offset
 */
static inline int
coalesce_sizes(struct cdi_record *r)
{
	int			ret = 0;
	off_t			off1,
				off2;
	struct cdi_record	*l;

	if (r->type != TYPE_SIZE || list_empty(&nvr_sys_queue))
		goto out;

	l = list_entry(nvr_sys_queue.prev, struct cdi_record, cd_queue);
	if (l->pid != -1)
		goto out;

	if (l->type != TYPE_SIZE)
		goto out;

	if (l->len != r->len || memcmp(l->name, r->name, r->len) != 0)
		goto out;

	memcpy(&off1, l->buf, sizeof(off1));
	memcpy(&off2, r->buf, sizeof(off2));

	/*
	** if this offset is smaller than the previous offset,
	** do nothing. If it's bigger, copy this offset into
	** the previous offset and drop this item
	*/
	if (off2 < off1)
		goto out;

	memcpy(l->buf, r->buf, sizeof(off1));
	ret = 1;
out:
	return ret;
}

/**
 * coalesce_writes - coalesce adjacent writes
 * @r: repitem being modified
 *
 * Only writes that are adjacent in time and offset are grouped.
 */
static inline int
coalesce_writes(struct cdi_record *r)
{
	int			ret = 0,
				len1,
				len2;
	long long		pos1,
				pos2;
	struct cdi_record	*l;
	
	if (r->type != TYPE_DATA || list_empty(&nvr_sys_queue))
		goto out;

	l = list_entry(nvr_sys_queue.prev, struct cdi_record, cd_queue);
	if (l->pid != -1)
		goto out;

	if (l->type != TYPE_DATA)
		goto out;

	if (l->len != r->len || memcmp(l->name, r->name, r->len) != 0)
		goto out;

	memcpy(&pos1, l->buf, sizeof(pos1));
	memcpy(&pos2, r->buf, sizeof(pos2));
	memcpy(&len1, l->buf+sizeof(pos1), sizeof(len1));
	memcpy(&len2, r->buf+sizeof(pos2), sizeof(len2));

	if (pos1 == pos2 && (len1 == len2 || len1 > len2)) {
		ret = 1;
		goto out;
	}

	if (pos1 < pos2 && pos1 + len1 > pos2 + len2) {
		ret = 1;
		goto out;
	}

	if (pos1 + len1 != pos2)
		goto out;

	len1 += len2;
	memcpy(l->buf + sizeof(pos1), &len1, sizeof(len1));
	if (logit > 2)
		printk(KERN_ERR "%d bytes coalesced for %s\n",
			len1 + len2, r->name);
	ret = 1;
out:
	return ret;
}

/**
 * nvr_add_queue - enqueue a repitem
 * @r: repitem to be queued
 */
void
nvr_add_queue(struct cdi_record *r)
{
	int	i = 0;

	if (strcmp(current->comm, "nvr_srv") == 0)
		goto out;

	highcount = MAX(highcount, nvr_sys_queue_count);

	while (nvr_sys_queue_count >= highwater && i < 500) {
		schedule();
		highwater_broken = 1;
		i++;
	}
	i = 0;

	if (pause && nvr_sys_queue_count >= pause) {
		printk(KERN_ERR "pause limit %d reached, %s discarded\n",
				pause, r->name);
		nvr_put_repitem(r);
	} else if (item_name_okay(r->name) &&
			nvr_sys_queue_count >= highwater) {
		printk(KERN_ERR "q %ld above highwater mark %d %s discarded\n",
				nvr_sys_queue_count, highwater, r->name);
		nvr_put_repitem(r);
	} else if (xfer_skip && strcmp(current->comm, "nvr_xfer") == 0) {
		nvr_put_repitem(r);
	} else if (replication_active && item_name_okay(r->name)) {
		nvr_down(&nvr_sys_queue_lock);

		if (coalesce_writes(r)) {
			nvr_put_repitem(r);
			goto waitforit;
		}
		if (coalesce_sizes(r)) {
			nvr_put_repitem(r);
			goto waitforit;
		} else {
			nvr_sys_queue_count++;
			list_add_tail(&r->cd_queue, &nvr_sys_queue);
			log_record(r);
		}
waitforit:
		while (nvr_sys_queue_count > QUEUE_HWAT_MARK && i < 25) {
			nvr_up(&nvr_sys_queue_lock);
			schedule();
			nvr_down(&nvr_sys_queue_lock);
			i++;
		}
		nvr_up(&nvr_sys_queue_lock);
	} else
out:
		nvr_put_repitem(r);
	return;
}

/**
 * dentry_to_vfs - get a vfs structure for a dentry
 * @de: dentry to get vfs struct for
 */
static struct vfsmount *
dentry_to_vfs(struct dentry *de)
{
	struct dentry		*tmp;
	struct list_head	*next,
				*head;
	struct vfsmount		*dmnt,
				*droot,
				*rootmnt;
	struct fs_struct	*f;

	dmnt = NULL;

	for (tmp = de; tmp->d_parent != tmp; tmp = tmp->d_parent)
		;

	f = init_task.fs;

	read_lock(&f->lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	rootmnt = mntget(f->rootmnt);
#else
	rootmnt = mntget(f->root.mnt);
#endif
	read_unlock(&f->lock);

	head = &rootmnt->mnt_list;
	next = NULL;

	list_for_each(next, head) {
		droot = list_entry(next, struct vfsmount, mnt_list);
		if (tmp == droot->mnt_root) {
			dmnt = mntget(droot);
			break;
		}
	}
	if (!dmnt) {
		dmnt = rootmnt;
		mntget(dmnt);
	}
	mntput(rootmnt);
	return dmnt;
}

/**
 * dentry_fullpath - convert a dentry to an absolute path
 * @de: dentry to convert
 * @dmnt: vfsmount for dentry
 * @buf: output buffer
 * @len: size of output buffer
 */
static char*
dentry_fullpath(struct dentry *de, struct vfsmount *dmnt, char *buf, int len)
{
	int			err = 0,
				namelen;
	char			*end = buf + len,
				*retval = NULL;
	struct qstr		*q = NULL;
	struct vfsmount		*rootmnt = NULL;
	struct fs_struct	*f = NULL;

	f = init_task.fs;
	read_lock(&f->lock);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,20)
	rootmnt = mntget(f->rootmnt);
#else
	rootmnt = mntget(f->root.mnt);
#endif
	read_unlock(&f->lock);

	*--end = '\0';
	len--;

	retval = end - 1;
	*retval = '/';

	for (;;) {
		struct dentry	*parent = NULL;

		if (de == dmnt->mnt_root || IS_ROOT(de)) {
			if (dmnt->mnt_parent == dmnt)
				goto global_root;
			de = dmnt->mnt_mountpoint;
			dmnt = dmnt->mnt_parent;
			continue;
		}

		parent = de->d_parent;
		q = &de->d_name;
		namelen = q->len;

		len -= namelen + 1;
		if (len < 0)
			break;
		end -= namelen;
		memcpy(end, q->name, namelen);
		*--end = '/';
		retval = end;
		de = parent;
	}
	mntput(rootmnt);
	goto out;

global_root:
	q = &de->d_name;
	namelen = q->len;

	len -= namelen;
	if (len >= 0) {
		retval -= namelen - 1;
		memcpy(retval, q->name, namelen);
	} else
		printk(KERN_ERR "Gack\n");

	mntput(rootmnt);
out:
	return retval;
}

/**
 * nvr_dcache_to_fn - externally visible pathname resolver
 * @de: dentry to resolve
 * @fn: output buffer
 */
int
nvr_dcache_to_fn(struct dentry *de, char *fn)
{
	int		err = -ENOMEM;
	char		*nm,
			*page;
	unsigned long	len;
	struct vfsmount	*dmnt;

	memset(fn, 0, PAGE_SIZE);

	page = (char *)__get_free_page(GFP_USER);
	if (!page)
		goto out;

	dmnt = dentry_to_vfs(de);
	if (!dmnt) {
		printk(KERN_ERR "Unable to dentry_to_vfs\n");
		goto out;
	}

	spin_lock(&dcache_lock);
	nm = dentry_fullpath(de, dmnt, page, PAGE_SIZE);
	spin_unlock(&dcache_lock);

	err = -ERANGE;
	len = PAGE_SIZE + page - nm;
	if (len < PAGE_SIZE) {
		err = len -1;
		memcpy(fn, nm, len);
	}
	mntput(dmnt);
	free_page((long)page);
out:
	return err;
}

/**
 * nvr_add_dentry - enqueue a dentry
 * @d: dentry to add
 * @type: repitem type flag
 * @notused: not used
 * @buf: type descriptor buffer
 * @len: size of buffer
 */
int
nvr_add_dentry(struct dentry *d, int type, void *notused,
		const unsigned char *buf, unsigned int len)
{
	int			ret = -ENOMEM;
	char			*nm = NULL;
	struct cdi_record	*r;

	nm = kmem_cache_alloc(names_cachep, GFP_KERNEL);
	if (!nm)
		goto out;
	ret = nvr_dcache_to_fn(d, nm);
	if (ret >= 0) {
		r = nvr_get_repitem(nm, ret, NULL, type, buf, len, NULL);
		nvr_add_queue(r);
		ret = 0;
	}

out:
	if (nm)
		putname(nm);
	return ret;
}

/**
 * nvr_add_name - enqueue a pathname
 * @nm: name to add
 * @notused: not used
 * @type: repitem type flag
 * @buf: type descriptor buffer
 * @len: size of buffer
 */
int
nvr_add_name(const char *nm, void *notused, int type,
		const unsigned char *buf, unsigned int len)
{
	int			err = 1;
	struct cdi_record	*r;

	if (!nm)
		goto out;
	if (len && !buf)
		goto out;

	err = 0;
	r = nvr_get_repitem(nm, strlen(nm), notused, type, buf, len, NULL);
	nvr_add_queue(r);
out:
	return err;
}
