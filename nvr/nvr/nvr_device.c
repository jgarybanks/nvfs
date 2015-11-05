#include "nvr.h"
#include "cdi_uaccess.h"
#include "nvr_device.h"
#include "nvr_queue.h"

int trans;
int pause;
int logit;
int xfer_skip;
int highwater = 50000;
int highcount;
int highwater_broken;
int replication_active;
char **multiwrite_files;
long nvr_sys_queue_count;
unsigned long long repitem_seq;

#define	EFAULT 14

DECLARE_MUTEX(nvr_sys_queue_lock);
DECLARE_MUTEX(trans_fn_mutex);
DECLARE_MUTEX(ritem_mutex);

enum {
	NVR_SET_HIGHWATER	= 0x5301,
	NVR_REP_PAUSE		= 0x5300,
	NVR_REP_REGISTER	= 0x5299,
	NVR_REP_UNREGISTER	= 0x5298,
	NVR_SET_ACTIVE		= 0x5297,
	NVR_SET_LOGGING		= 0x5296,
	NVR_INFO		= 0x5295,
	NVR_SET_XFER_SKIP	= 0x5294,
	NVR_SEQUENCE_NUMBER	= 0x5293,
	NVR_GET_MJ		= 0x5292,
	NVR_SET_STRICT		= 0x5291,
	NVR_SET_TRANS		= 0x5290,
	NVR_SET_PATH		= 0x5289,
};

/**
 * nvr_register - give filelist to nvr_srv via /dev/nvr
 * @uregp: pointer to output buffer
 */
int
nvr_register(struct cdi_reg_buffer *uregp)
{
	int			err = 0;
	size_t			offset = 0;
	struct list_head	*tmp;
	struct cdi_record	*r;
	struct cdi_reg_buffer	reg;

	if (!replication_active) {
		printk(KERN_NOTICE "not running\n");
		goto out;
	}
	if (pause) {
		printk(KERN_NOTICE "kernel paused\n");
		goto out;
	}

	err = -EFAULT;
	if (nvr_copy_from_user(&reg, uregp, sizeof(*uregp)))
		goto out;

	if (reg.length <= 0)
		goto out;

	nvr_down(&nvr_sys_queue_lock);

	/*
	 * Each entry we copy into the cdi_reg_buffer looks like this :
	 *
	 * sequence, timestamp, type, name length, name, buffer length, buffer
	 *
	 * The buffer contains the buffer size, and type specific info.
	 * For example, in a rename, name is the old name, and the buffer
	 * contains the length of the new name and the new name itself. For
	 * a write, the buffer contains the size of the buffer, the offset
	 * at which data was written, and the size of the user buffer that
	 * was written.
	 *
	 * The callbacks in nvr_main.c user are responsible for building the
	 * buffers that are appropriate for their functions. nvr_srv is
	 * responsible for interpreting the buffers correctly based on the
	 * type.
	 */

	list_for_each(tmp, &nvr_sys_queue) {
		size_t rec_size;

		r = list_entry(tmp, struct cdi_record, cd_queue);

		rec_size = sizeof(r->sequence) +	/* sequence       */
			   sizeof(r->timestamp) +	/* timestamp      */
			   sizeof(r->type) +		/* type           */
			   sizeof(r->len) +		/* length of name */
			   r->len +			/* space for name */
			   sizeof(r->bsize) +		/* length of buf  */
			   r->bsize;			/* space for buf  */

		if (offset + rec_size >= reg.length)
			break;

#define REG_COPY(A, B) do {				\
	if (nvr_copy_to_user(reg.data+offset, A, B))	\
		goto out;				\
	offset += B;					\
} while (0)



		REG_COPY(&r->sequence, sizeof(r->sequence));
		REG_COPY(&r->timestamp, sizeof(r->timestamp));
		REG_COPY(&r->type, sizeof(r->type));
		REG_COPY(&r->len, sizeof(r->len));
		REG_COPY(r->name, r->len);
		REG_COPY(&r->bsize, sizeof(r->bsize));
		if (r->bsize)
			REG_COPY(r->buf, r->bsize);
		r->pid = 1;
	}
	err = offset;
out:
	nvr_up(&nvr_sys_queue_lock);
	return err;
}

/**
 * nvr_unregister - get rid of files processed by nvr_srv
 */
int
nvr_unregister(void)
{
	struct list_head	*tmp, *safe;
	struct cdi_record	*r;

	nvr_down(&nvr_sys_queue_lock);

	list_for_each_safe(tmp, safe, &nvr_sys_queue) {
		r = list_entry(tmp, struct cdi_record, cd_queue);
		if (r->pid == 1) {
			list_del_init(&r->cd_queue);
			nvr_put_repitem(r);
			r = NULL;
			nvr_sys_queue_count--;
		}
	}

	nvr_up(&nvr_sys_queue_lock);
	return 0;
}

/**
 * nvr_getinfo - copy information for nvr_kinfo
 * @arg: output buffer
 */
int
nvr_getinfo(struct cdi_info_buffer *arg)
{
	int err = 0;

	err = nvr_copy_to_user(&arg->sys_queue, &nvr_sys_queue_count,
			sizeof(nvr_sys_queue_count));
	if (!err)
		err = nvr_copy_to_user(&arg->active, &replication_active,
				sizeof(replication_active));
	if (!err)
		err = nvr_copy_to_user(&arg->highwater_broken,
				&highwater_broken, sizeof(highwater_broken));
	if (!err)
		err = nvr_copy_to_user(&arg->highwater_value,
				&highwater, sizeof(highwater));
	if (!err)
		err = nvr_copy_to_user(&arg->highcount, &highcount,
				sizeof(highcount));
	return err;
}

/**
 * nvr_set_trans - process multiwrite files/tag via nvr_srv
 * @arg: input buffer (: delimited file/path names)
 */
int
nvr_set_trans(void *arg)
{
	int			i = 0,
				tmp = 0,
				err = -1,
				cnt = 0;
	char			*cp,
				*cp2,
				*mstring = NULL;
	struct cdi_reg_buffer	reg;

	ENTER;

	down(&trans_fn_mutex);

	err = copy_from_user(&reg, (void *)arg, sizeof(reg));
	if (reg.length < 0 || err)
		goto done;
	if (reg.length == 0) {
		trans = 0;
		kfree(multiwrite_files);
		goto done;
	}
	mstring = kmalloc(reg.length + 1, GFP_KERNEL);
	err = copy_from_user(mstring, reg.data, reg.length);
	if (err) {
		kfree(mstring);
		goto done;
	}
	*(mstring+reg.length) = '\0';
	for (cp = mstring; cp && *cp != '\0'; cp++)
		if (*cp == ';')
			cnt++;
	tmp = cnt+1;

	multiwrite_files = kmalloc(tmp, GFP_KERNEL);
	multiwrite_files[tmp] = NULL;
	cp = mstring;

	for (cp = mstring; cp && *cp != '\0'; cp++) {
		for (cp2 = cp; cp2; cp2++) {
			if (*cp2 == ';' || *cp2 == '\0') {
				multiwrite_files[i] =
					kmalloc(strlen(cp)+1, GFP_KERNEL);
				memset(multiwrite_files[i], 0, strlen(cp)+1);
				memcpy(multiwrite_files[i], cp, cp2-cp);
				if (*cp2 == '\0') {
					cp = cp2;
					goto done;
				}
				i++;
				*cp2 = '\0';
				cp2++;
				cp = cp2;
			}
		}
	}
done:
	trans = tmp;
	up(&trans_fn_mutex);
	EXIT_RET(err);
}

/**
 * nvr_dev_ioctl - ioctl functions for /dev/nvr
 * @inode: unused
 * @file: unused
 * @cmd: ioctl command
 * @arg: input buffer
 */
int
nvr_dev_ioctl(struct inode *inode, struct file *file, u_int cmd, u_long arg)
{
	int	err = -1;

	switch (cmd) {
	case NVR_REP_PAUSE:
		err = nvr_copy_from_user(&pause, (void *)arg, sizeof(pause));
		printk(KERN_NOTICE "kernel pause set to %d\n", pause);
		break;
	case NVR_SET_HIGHWATER:
		err = nvr_copy_from_user(&highwater, (void *)arg,
				sizeof(highwater));
		printk(KERN_NOTICE "Highwater size set to %d\n", highwater);
		printk(KERN_NOTICE "Reset highwater indicator to 0\n");
		highwater_broken = 0;
		break;
	case NVR_SET_TRANS:
		err = nvr_set_trans((void *)arg);
		break;
	case NVR_SET_XFER_SKIP:
		err = nvr_copy_from_user(&xfer_skip, (void *)arg,
				sizeof(xfer_skip));
		break;
	case NVR_SET_LOGGING:
		err = nvr_copy_from_user(&logit, (void *)arg, sizeof(logit));
		printk(KERN_NOTICE "logging %d\n", logit);
		break;
	case NVR_INFO:
		err = nvr_getinfo((struct cdi_info_buffer *)arg);
		break;
	case NVR_REP_REGISTER:
		err = nvr_register((struct cdi_reg_buffer *)arg);
		break;
	case NVR_REP_UNREGISTER:
		err = nvr_unregister();
		break;
	case NVR_SET_ACTIVE:
		err = nvr_copy_from_user(&replication_active, (void *)arg,
				sizeof(replication_active));
		printk(KERN_NOTICE "active=%d\n", replication_active);
		if (replication_active)
			pause = 0;
		break;
	case NVR_SEQUENCE_NUMBER:
		err = nvr_copy_from_user(&repitem_seq, (void *)arg,
				sizeof(repitem_seq));
		printk(KERN_NOTICE "sequence set to %ld\n", repitem_seq);
		break;
	default:
		break;
	}
	return err;
}
