#include "nvr.h"
#include "cdi_repitem.h"

#define CONVERT_TIMEVAL(T) ({						\
		unsigned long long msec;				\
		msec = ((T).tv_sec * 1000ULL);				\
		msec += (unsigned long long)((T).tv_usec / 1000);	\
		msec; })

extern struct list_head nvr_sys_queue;
extern int repitem_count;
extern unsigned long long repitem_seq;
extern struct semaphore nvr_sys_queue_lock;

struct cdi_record *nvr_get_repitem(const char*, int, void*, int,
		const unsigned char*, int, unsigned char*);
void nvr_put_repitem(struct cdi_record*);
void nvr_add_queue(struct cdi_record*);
