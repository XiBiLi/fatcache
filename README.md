Fork to Learn and Extend fatcache, which is SSD-backed memcached for Big Data. 

### Initial Repository 
[here](https://github.com/twitter/fatcache/)

### Proposal Abstract for [Fatcache@gsoc'13](https://github.com/twitter/twitter.github.com/wiki/Google-Summer-of-Code-2013)

**Improve the performance of fatcache with asynchronous I/O** 

The most important feature of systems designed for Big Data applications 
is horizontal scalability. And this is also true for distributed caching 
system, which plays a critical role in today's web services. 
But pure in-memory cache systems, like memcached, are difficult to scale out
due to the high cost and power consumption of DRAM 
[[1](https://github.com/twitter/fatcache/)].
Fatcache, a cache system for Big Data, addresses the scalability issue
by utilizing SSD, which has higher capacity per dollar, lower power consumption 
per byte, as well as comparable read latency to that of network 
[[1](https://github.com/twitter/fatcache/),[2](https://gist.github.com/jboner/2841832)].

Currently, fatcache adopts blocking synchronous disk I/O; both SSD read and
write operations are on the critical path to respond the requests from users.
But SSD write does not service requests directly, and should be decoupled 
from the critical path to improve the system throughput. 
Therefore, as indicated in [[1](https://github.com/twitter/fatcache/)], 
I am using kernel aio to enhance the parallelism among CPU, SSD and network 
within fatcache. In addition, I propose to drain and evict slabs according 
to the characteristics of workload instead of the currently assumed FIFO policy.

[Detailed version](https://github.com/cloudXane/fatcache/wiki/Proposal-of-GSOC%2713)

### Implementation Details

The extension is mainly in src/fc\_aio, with little modification to src/fc\_slab.
Kernel aio and Eventfd are used to implement this asynchronous mechanism. 

Kernel aio provides some APIs to prepare and submit asynchronous I/O operations.
The eventfd is registered to epoll to wait for I/O completion notification in 
another thread; then post-processing is done to keep system state consistent. 
I choose to use one more thread is because the existing epoll for network communication 
is encapsulated specifically for **struct conn**, and not easy to be reused for SSD I/O.

Currently, no full-coverage debug is conducted due to the lack of devices. 
Workload-aware caching policy is being studied.

### Study Notes 
[here](https://github.com/cloudXane/fatcache/blob/asyncIO/learning.txt)

### Personal Information
[LinkedIn](http://www.linkedin.com/pub/xiaobing-li/69/a71/319)
