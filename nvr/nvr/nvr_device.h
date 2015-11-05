#include "nvr.h"
#include "cdi_uaccess.h"
#include "cdi_repitem.h"

extern int pause;
extern int logit;
extern int xfer_skip;
extern int highwater;
extern int highcount;
extern int highwater_broken;
extern int replication_active;
extern long nvr_sys_queue_count;
extern unsigned long long repitem_seq;

int nvr_getinfo(struct cdi_info_buffer *);
int nvr_register(struct cdi_reg_buffer *);
int nvr_unregister(void);

extern struct list_head nvr_sys_queue;
