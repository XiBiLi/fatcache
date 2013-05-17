/*
 * Contributor: Xiaobing Li
 * linkedin: http://www.linkedin.com/pub/xiaobing-li/69/a71/319
 *
 */

#include <fc_core.h>

extern uint32_t nfree_msinfoq;         /* # free memory slabinfo q */
extern struct slabhinfo free_msinfoq;  /* free memory slabinfo q */
extern uint32_t nfull_dsinfoq;         /* from fc_slab */
extern struct slabhinfo full_dsinfoq;  /* from fc_slab */      
extern pthread_mutex_t full_dsinfoq_lock;

extern struct settings settings;       /* get slab size */
static int    ssdfd;                   /* ssd fd from fc_slab.c */

static uint32_t nfree_aioinfoq;        /* # free aioinfo (iocb, buf) */
static struct   aioinfo_tqh free_aioinfoq; /* free aioinfoq */

static io_context_t    aio_ctx;        /* context for IO operation */

static int             efd;            /* eventfd for completion notice*/
static const int32_t   ev_min = 1;     /* min # of completion events to wait */
static const int32_t   ev_max = 4096;  /* max # of completion events to wait */
struct io_event       *aio_events;     /* buffer for returned events */


static pthread_t reaper;               /* epoll to reap the comp evt */
static bool      stop_reaper = false;

static int       handle_completion(void);

    static void 
aioinfo_free(struct aioinfo *cb)
{
    log_debug(LOG_NOTICE, "free aioinfo %p", cb);

    fc_free(cb->slabbuf);
    fc_free(cb);
}


    static void *
reap_loop(void *ptr)
{
    int ep;                   /* epoll descriptor */
    int n;                    /* return status */

    ep = epoll_create(10);
    if (ep < 0) {
	log_error("reap epoll create failed: %s", strerror(errno));
	return NULL;
    }

    struct epoll_event event, dummy_evt; 
    event.events = (uint32_t)(EPOLLIN | EPOLLET);

    /* add eventfd to get io compeletion notice */
    n = epoll_ctl (ep, EPOLL_CTL_ADD, efd, &event);
    if (n < 0) {
	log_error("epoll add eventfd %d failed: %s", efd, strerror(errno));

	return NULL;
    }

    while (!stop_reaper) {

	log_debug(LOG_NOTICE, "epoll waits for wrt complete"); 
	n = epoll_wait(ep, &dummy_evt, 1, -1);

	if (n == 1) {
	    log_debug(LOG_NOTICE, "got wrt completion notification"); 

	    ASSERT(dummy_evt.events | EPOLLIN);

	    uint64_t eval = 0;

	    for (;;) {
		if (read(efd, &eval, sizeof(eval)) != sizeof(eval)) {
		    log_error("read eventfd %d failed: %s", efd, strerror(errno));

		    break;
		}

		handle_completion();
	    }
#if 0
	    int r;
	    log_debug(LOG_NOTICE, "eventfd return %lld", eval);
	    while (eval > 0) {
		r = handle_completion();

		if (r > 0) {
		    eval -= r;
		    log_debug(LOG_NOTICE, "got %ld/%ld results so far\n", r, eval);
		}
	    }
#endif
	} else if (n < 0) {
	    if (errno == EINTR) {
		continue;
	    }

	    log_error("epoll wait on e %d failed: %s", ep, strerror(errno));

	    break;
	}
    }

    return NULL;
}


    rstatus_t
aio_init(int fd, size_t fsize)
{
    ssdfd = fd;                        /* aio_init called by slab_init */

    log_debug(LOG_NOTICE, "aioinfo size %d", sizeof(struct aioinfo));

    nfree_aioinfoq = 0;
    TAILQ_INIT(&free_aioinfoq);

    /* init io queue, min(ssdsize/slabsize, MAX) */
    uint32_t ndchunk = fsize / settings.slab_size;

    loga("ndchunk %d, ev_max %d", ndchunk, ev_max);

    if (io_queue_init(MAX(ndchunk, ev_max), &aio_ctx)) {
	log_error("io_queue_init failed: %s", strerror(errno));
	return FC_ERROR;
    }

    /* init eventfd */
    if ((efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) == -1) {
	log_error("eventfd failed: %s", strerror(errno));
	return FC_ERROR;
    }

    /* init the events array */
    aio_events = fc_alloc(ev_max * sizeof(*aio_events));

    /* init reaper thread, start  */
    rstatus_t status = pthread_create(&reaper, NULL, reap_loop, NULL);
    if (status != 0) {
	log_error("create reaper failed: %s", strerror(status));
	return FC_ERROR;
    }

    return FC_OK;
}


    void 
aio_deinit(void)
{
    stop_reaper = true;

    if (aio_events != NULL) {
	fc_free(aio_events);
    }

    close(efd);

    io_queue_release(aio_ctx);

    struct aioinfo *aioinfo, *naioinfo;/* current and next aioinfo */

    for (aioinfo = TAILQ_FIRST(&free_aioinfoq); aioinfo != NULL;
	    aioinfo = naioinfo, nfree_aioinfoq--) {
	ASSERT(nfree_aioinfoq > 0);

	naioinfo = TAILQ_NEXT(aioinfo, tqe);
	aioinfo_free(aioinfo);
    }

    ASSERT(nfree_aioinfoq == 0);
}


    struct aioinfo *
aioinfo_get(void)
{
    struct aioinfo *aioinfo;

    if (!TAILQ_EMPTY(&free_aioinfoq)) {
	ASSERT(nfree_aioinfoq > 0);

	aioinfo = TAILQ_FIRST(&free_aioinfoq);
	nfree_aioinfoq--;
	TAILQ_REMOVE(&free_aioinfoq, aioinfo, tqe);
    } else {
	aioinfo = fc_alloc(sizeof(*aioinfo));
	if (aioinfo == NULL) {
	    return NULL;
	}

	aioinfo->slabbuf = fc_alloc(sizeof(uint8_t) * 
		settings.slab_size);

	if (aioinfo->slabbuf == NULL) {/* (iocb, buf) */
	    fc_free(aioinfo);
	    return NULL;
	}
    }

    memset(&(aioinfo->aio_cb), 0, sizeof(struct iocb));
    aioinfo->slab2wt = NULL;

    return aioinfo;
}


    void 
aioinfo_put(struct aioinfo *cb)
{
    ASSERT(cb->slab2wt == NULL);

    log_debug(LOG_NOTICE, "put aioinfo %p", cb);

    /* FIFO to avoid the tiny data inconsistency issue */
    nfree_aioinfoq++;
    TAILQ_INSERT_TAIL(&free_aioinfoq, cb, tqe);
}


/* 
 * TODO to submit io req in a batch 
 * static array outreq; 
 * io_submit(aio_ctx, outreq->nelem, outreq->elem); 
 * 
 * */
    static rstatus_t
submit_wrt(struct aioinfo *cb)
{
    struct iocb *piocb = (struct iocb *) &(cb->aio_cb);

    int n = io_submit(aio_ctx, 1, &piocb);
    if (n != 1) {
	log_error("io_submit failed: %s", strerror(errno));

	aioinfo_put(cb);               /* return to pool */

	return FC_ERROR;
    }

    return FC_OK;
}

/*
 * ISSUES:
 * async write make ssd data not as consistent as sync write 
 * because ssd slabs drained out might still in the temp buf
 *
 * The methods affected by this issue are:
 *
 * 1. slab_evict() which reads ssd slab in the full_dsinfoq,
 * TO resolve inconsistency by delaying add the drained slab into 
 * full_dsinfoq until the async write is done; 
 * ---- change _slab_drain()/handle_completion()
 *
 * 2. slab_read_item() when read from ssd slab
 * TO resolve inconsistency by adding 'persistent' flag and reading 
 * from the write-buf 
 * ---- change slab_read_item()
 *
 */
    rstatus_t
async_write(struct slabinfo *src, struct slabinfo *dst)
{
    struct aioinfo *cb = aioinfo_get();

    if (cb == NULL) {
	log_error("get aioinfo for slab %d failed: %s", src->sid,
		strerror(errno));

	return FC_ENOMEM;
    }

    size_t size = settings.slab_size;
    uint8_t *buf = cb->slabbuf;

    ASSERT(src->persisted == 1);       /* in mem/ssd is persistent */
    cb->slab2wt = src;

    /* copy data from memslab to slabbuf */
    struct slab *slab = slab_from_maddr(src->addr, true);
    fc_memcpy(buf, slab, size);

    ASSERT(((struct slab *)buf)->magic == SLAB_MAGIC);

    src->persisted = 0;                /* ephemerally in buf */
    src->tempaddr = buf;                   

    /* setup the iocb to write the buf to ssd async */
    off_t off = slab_to_daddr(dst); 
    log_debug(LOG_NOTICE, "when asyncwrite offset is %llu", off);

    io_prep_pwrite(&(cb->aio_cb), ssdfd, buf, size, off);
    io_set_eventfd(&(cb->aio_cb), efd);

    (cb->aio_cb).data = cb;            /* pass to event.data */

    /* ensure the timing logic */
    slab_swap_addr(src, dst);
    nfree_msinfoq++;
    TAILQ_INSERT_TAIL(&free_msinfoq, dst, tqe);

    log_debug(LOG_NOTICE, "src slab");
    slabinfo_print(src);
    log_debug(LOG_NOTICE, "dst slab");
    slabinfo_print(dst);

    return submit_wrt(cb);
}


    int       
handle_completion()
{
    struct aioinfo *cb;
    struct slabinfo *ssdslab;

    struct timespec tmo;
    tmo.tv_sec = 0;
    tmo.tv_nsec = 0;

    int num_evts, i;

    num_evts = io_getevents(aio_ctx, ev_min, ev_max, aio_events, &tmo);
    loga("io get events # %d", num_evts);

    while (num_evts > 0) {

	for (i = 0; i < num_evts; i++) {
	    cb = (struct aioinfo *) (aio_events[i]).data;

	    ssdslab = cb->slab2wt;

	    ASSERT(((struct slab *)cb->slabbuf)->magic == SLAB_MAGIC);

#if 0
	    off_t off1 = slab_to_daddr(ssdslab);
	    size_t size1 = settings.slab_size;
	    pwrite(ssdfd, cb->slabbuf, size1, off1);
	    write(ssdfd, cb->slabbuf, size1);

	    /* read back to check slab integrity, OK! */
	    log_debug(LOG_NOTICE, "read back to test integrity from cb->slab2wt");
	    struct slab *slab = (struct slab *)ssdslab->tempaddr;
	    size_t size = settings.slab_size;
	    off_t off = slab_to_daddr(ssdslab);
	    int n = pread(ssdfd, slab, size, off);
	    ASSERT(slab->magic == SLAB_MAGIC);
	    ASSERT(slab->sid == ssdslab->sid);
	    ASSERT(slab->cid == ssdslab->cid);
#endif

	    ASSERT(ssdslab->persisted == 0);
	    ssdslab->persisted = 1;    /* data is persisted on ssd */

	    slabinfo_print(ssdslab);

	    pthread_mutex_lock(&full_dsinfoq_lock);

	    nfull_dsinfoq++;           /* when slab written on ssd, add to full q */
	    TAILQ_INSERT_TAIL(&full_dsinfoq, ssdslab, tqe);
	    //TAILQ_INSERT_HEAD(&full_dsinfoq, ssdslab, tqe);

	    pthread_mutex_unlock(&full_dsinfoq_lock);

#if 0
	    /* read back to check slab integrity from queue */
	    log_debug(LOG_NOTICE, "read back to test integrity from queue");
	    struct slabinfo *sinfo = TAILQ_FIRST(&full_dsinfoq);
	    size_t size = settings.slab_size;
	    off_t off = slab_to_daddr(sinfo);
	    log_debug(LOG_NOTICE, "rd back from q, off is %lld", off);

	    struct slab *slab = (struct slab *) fc_alloc(size);
	    int n = pread(ssdfd, slab, size, off);

	    log_debug(LOG_NOTICE, "slab.magic %x, SLAB_MAGIC %x", 
		    slab->magic, SLAB_MAGIC);

	    ASSERT(slab->magic == SLAB_MAGIC);
	    ASSERT(slab->sid == ssdslab->sid);
	    ASSERT(slab->cid == ssdslab->cid);
	    fc_free(slab);
#endif

	    cb->slab2wt = NULL;
	    aioinfo_put(cb);           /* return to pool */
	}

	num_evts = io_getevents(aio_ctx, ev_min, ev_max, aio_events, &tmo);
	loga("io get events # %d", num_evts);
    }

    return num_evts;
}
