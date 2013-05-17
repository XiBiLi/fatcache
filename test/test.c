#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h>
#include <limits.h>

#include <errno.h>
#include <time.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <libaio.h>
#include <pthread.h>
#include <sys/eventfd.h>

#include <fcntl.h>

io_context_t aio_ctx;        
int fd;   /* file fd */
int efd;  /* event fd */            

int stop_reaper = 0;
pthread_t reaper;               
pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

uint32_t wt_magic = 0xdeadbeaf;
uint32_t rd_magic = 0;

int handle_completion(void);

void *
reap_loop(void *ptr)
{
    int ep;                   /* epoll descriptor */
    int n;                    /* return status */

    printf("reaper thread starting... \n");

    ep = epoll_create(10);
    if (ep < 0) {
	printf("reap epoll create failed: %s\n", strerror(errno));
	return NULL;
    }

    struct epoll_event event, dummy_evt; 
    event.events = (uint32_t)(EPOLLIN | EPOLLET);

    /* add eventfd to get io compeletion notice */
    n = epoll_ctl (ep, EPOLL_CTL_ADD, efd, &event);
    if (n < 0) {
	printf("epoll add eventfd %d failed: %s\n", efd, strerror(errno));
	return NULL;
    }

    while (!stop_reaper) {
	printf("epoll waiting... \n");

	n = epoll_wait(ep, &dummy_evt, 1, -1);

	if (n == 1) {
	    printf("got wrt completion notification\n"); 
	    uint64_t eval = 0;
	    for (;;) {
		if (read(efd, &eval, sizeof(eval)) != sizeof(eval)) {
		    printf("read eventfd %d failed: %s\n", efd, strerror(errno));
		    break;
		}
		handle_completion();
	    }
	} else if (n < 0) {
	    if (errno == EINTR) {
		continue;
	    }
	    printf("epoll wait on %d failed: %s\n", ep, strerror(errno));
	    break;
	}
    }

    pthread_mutex_unlock(&lk);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_mutex_lock(&lk);

    //fd = open("/dev/loop0", O_RDWR | O_DIRECT, 0644);
    fd = open("/dev/loop0", O_RDWR, 0644);

    efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    io_queue_init(32, &aio_ctx);

    pthread_create(&reaper, NULL, reap_loop, NULL);

    struct iocb cb;     

    printf("preparing io...\n"); 
    io_prep_pwrite(&cb, fd, &wt_magic, sizeof(wt_magic), 0);
    io_set_eventfd(&cb, efd);

    /* submit io */
    printf("submiting io...\n"); 
    struct iocb *piocb = (struct iocb *) &cb;
    int n = io_submit(aio_ctx, 1, &piocb);
    if(n <= 0) {
	perror("io_submit");
	return -1;
    }

    printf("main thread waiting... \n");

    /* wait for thread stop */
    pthread_mutex_lock(&lk);

    printf("main thread stopping... \n");

    io_queue_release(aio_ctx);
    close(fd);
    close(efd);
}

    int       
handle_completion()
{
    printf("handle completion...\n");

    int num_evts, i;
    struct iocb *piocb;     

    struct timespec tmo;
    tmo.tv_sec = 0;
    tmo.tv_nsec = 0;

    struct io_event  aio_events[1];     

    num_evts = io_getevents(aio_ctx, 1, 1, aio_events, &tmo);

    printf("io get events # %d\n", num_evts);

    while (num_evts > 0) {
	pread(fd, &rd_magic, sizeof(rd_magic), 0);

	printf("wt_magic %x, rd_magic %x\n", wt_magic, rd_magic);

	num_evts = io_getevents(aio_ctx, 1, 1, aio_events, &tmo);
    }

    /* let the thread stop */
    stop_reaper = 1;
}
