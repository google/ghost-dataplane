# gHost-benchmarking-suite

We have prototyped a network virtualization stack that mimics Google’s Andromeda (NSDI’18) and can be scheduled by ghOSt. The platform is composed of DPDK threads that act as packet processing entities. Benchmarking applications deployed within containers, communicate with one another using the DPDK dataplane. In the background, the ghOSt agent makes scheduling decisions for DPDK threads and containers to optimize application tail latency.
