# gHost-benchmarking-suite

We have prototyped a network virtualization stack that mimics Google’s Andromeda (NSDI’18) and can be scheduled by ghOSt. The platform is composed of DPDK threads that act as packet processing entities. Benchmarking applications deployed within containers, communicate with one another using the DPDK dataplane. In the background, the ghOSt agent makes scheduling decisions for DPDK threads and containers to optimize application tail latency.

## Subdir Engine

This platform mimics Google’s host networking stack, Andromeda. It is designed for low-latency, high throughput packet delivery while providing network functionality and telemetry. The core design principles of Andromeda are:

- One logical CPU executes busy polling Fast Path.
- The Fast Path is used for performance-critical flows and currently has a 300ns per-packet CPU budget. Expensive per-packet work on-host is performed on the Coprocessor Path.
- Coprocessor stages include encryption, DoS, abuse detection, and WAN traffic shaping. 
- Coprocessor stages have to be enabled. Furthermore, the coprocessor thread may have to be woken up. The Fast path and coprocessor path communicate using a single producer, single consumer queue.
- Coprocessor stages are executed in a per-VM floating Coprocessor thread, whose CPU time is attributed to the container of the corresponding VM. Please note that the stages are executed in a pipeline.

### Virtual Port
To ensure applications get a traditional TCP/IP stack, supporting retransmission capabilities, we use DPDK KNI. To prevent modifying the application, we refrain from using MTCP. Using KNI, we create virtual ports visible to the kernel. We then add these ports to the container namespace and configure the port alongside the ARP/IP Route tables. 

### Fast Path
The fast path employs a core that polls the NIC, virtual ports, and coprocessors queues for packets. Being a lightweight pipeline, it only identifies the packet's egress port based on the destination IP and forwards the packet. The fast path is also responsible for identifying whether the packet warrants expensive processing (e.g., firewall, NAT, DoS). If yes, the packet is forwarded to the coprocessor, which may have to be woken up.

### Coprocessor Path
The coprocessors are intended for expensive packet processing such as longest prefix matching. Each coprocessor consists of multiple stages that have to be enabled for the packet. The fast path enqueues/dequeues packet descriptors to and from coprocessor ring buffers. As coprocessors are per-customer-instance floating threads that may have to be woken up, we use sem_post and sem_wait to block the coprocessor when there are no packets in the ring buffer. 

## Subdir Orchestrator

[CloudSuite](https://github.com/parsa-epfl/cloudsuite) is a benchmark suite for cloud services. It consists of applications that have been selected based on their popularity in today's data centers. The benchmarks are based on real-world software stacks and represent real-world setups. We have integrated the following applications in our platform:

- Memcached key value store (Data Caching)
- Apache Solr (Web Search)
- Hadoop Map Reduce (Data Analytics)

These applications emulate cloud customer applications and report QoS metrics when running on our platform. Each application consists of multiple containers that communicate using the DPDK pipeline. Applications are unmodified in our benchmarking suite.

## Subdir ghOSt_agent

Our baseline setup uses Linux default scheduler - CFS. Our goal is to investigate whether a workload-specific scheduler, deployed via ghOSt, can lead to better performance. A ghOSt enclave is defined as the cpus allocated for the container, plus an additional core for the agent itself. The ghOSt agent schedules waiting threads on any available CPU core, preferring the last CPU the thread ran on, if possible. If no CPU core is idle, it tries to preempt either a memcached or a firewall thread running on any CPU core as explained below.

The preemption of the firewall thread is preferred over memcached. We maintain a list of available CPU cores. If this list is non-empty, the waiting thread is scheduled on a randomly selected CPU core, honouring its history for cache locality. On the other hand, if there are no available CPU cores, we construct a list of preemptable CPU cores. All cores running firewall threads and cores that have a memcached thread running for at least 300us (with 2% chance) are candidates for preemption. A CPU core is then randomly selected for preemption. If there are no available CPU cores for preemption, the thread is scheduled in the next round. The policy is illustrated in the following pseudo code.




