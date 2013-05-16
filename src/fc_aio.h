/*
 * Contributor: Xiaobing Li
 * linkedin: http://www.linkedin.com/pub/xiaobing-li/69/a71/319
 *
 */

#ifndef _FC_AIO_H_
#define _FC_AIO_H_

#include <libaio.h>

struct aioinfo {                       /* (iocb, slab) or (iocb, buf) */

    struct iocb aio_cb;                /* fd, buf, bufsize, off */

    TAILQ_ENTRY(aioinfo) tqe;          /* link in free q */

    struct slabinfo *slab2wt;          /* slab to write: get off */
    uint8_t         *slabbuf;          /* slab data buf header 
			                * slab size is fixed */
};

TAILQ_HEAD(aioinfo_tqh, aioinfo);      /* iocb & buf pool */

rstatus_t aio_init(int fd, size_t fsize);   
void      aio_deinit(void); 

struct aioinfo *aioinfo_get(void);
void   aioinfo_put(struct aioinfo *cb);

rstatus_t async_write(struct slabinfo *src /* in-mem */, 
		      struct slabinfo *dst /* on-ssd */);

#endif/*_FC_SLAB_H_*/
