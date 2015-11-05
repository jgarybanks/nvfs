#ifndef __NVR_H__
#define __NVR_H__

#include "../nvfs.h"

#define NVR_TYPE_COPYALL	0
#define NVR_TYPE_MDATA		1
#define NVR_TYPE_DATA		2
#define NVR_TYPE_UNLINK		3
#define NVR_TYPE_SIZE		8
#define NVR_TYPE_DBDATA		9
#define NVR_TYPE_RENAME		128

int nvr_dev_ioctl(struct inode*, struct file*, unsigned int, unsigned long);
int nvr_add_dentry(struct dentry *, int, void*,
		const unsigned char*, unsigned int);
int nvr_add_name(const char*, void*, int, const unsigned char*, unsigned int);
int nvr_dcache_to_fn(struct dentry*, char*);

asmlinkage void nvr_down(struct semaphore *);
asmlinkage void nvr_up(struct semaphore *);

asmlinkage long nvr_copy_to_user(void*, void*, long);
asmlinkage long nvr_copy_from_user(void*, void*, long);

#endif /* __NVR_H__ */
