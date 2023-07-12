# Intel_Address_Hash
The codes here use hardware performance counters to find the mapping of addresses to L3 slices in Intel processors.

## Background
The distributed, shared L3 caches in Intel multicore processors are composed of “slices” (typically one “slice” per core), each assigned responsibility for a fraction of the address space. A high degree of interleaving of consecutive cache lines across the slices provides the appearance of a single cache resource shared by all cores. A family of undocumented hash functions is used to distribute addresses to slices, with a different hash function required for different number of L3 slices. 

There are two main reasons for determining these mappings:
1. When combined with a mapping of the locations of cores and locations of L3 slices, knowing the mapping of addresses to L3 slices allows one to map the traffic for commands, snoops, and responses on the processor's on-chip mesh network.  This can be used for visualization of data traffic or both analytical performance models.
2. The hash functions are subject to severe conflicts in the L3 cache and in the Snoop Filter (used to track which lines are cached in a core's private L1 and/or L2 caches)[^1].  Measuring the mappings helps to understand the precise nature of the conflict and provides ideas about workarounds.

## Technical Details

In all systems studied to date, the hash consists of a relatively short (16 to 16384 elements) “base sequence” of slice numbers, which is repeated with binary permutations for consecutive blocks of memory. The specific binary permutation used is selected by XOR-reductions of different subsets of the higher-order address bits. A [technical report with data files](http://dx.doi.org/10.26153/tsw/14539) provides the base sequences and permutation select masks for Intel Xeon Scalable Processors (1st and 2nd generation) with 14, 16, 18, 20, 22, 24, 26, 28 slices, for 3rd Generation Intel Xeon Scalable Processors with 28 slices, and for Xeon Phi x200 processors with 38 slices.

The "SnoopFilterMapper.c" codes use a fairly simple approach to determining the mapping of addresses to L3 slices:
- Bind the process to a single core.
- Allocate a large region on a 2MiB boundary with transparent hugepages enabled.  Default is 2GiB.
- For each 2MiB-aligned region:
  - For each cacheline:
    - Read the "LLC_LOOKUP.READ" performance counter event at each of the CHA/SF/LLC boxes in the uncore of the socket.
    - Repeat 1000 times:
      - Load the target cacheline
      - Flush the target cacheline
    - Read the "LLC_LOOKUP.READ" performance counter event at each of the CHA/SF/LLC boxes in the uncore of the socket.
    - The L3 slice that shows approximately 1000 read accesses is the own that owns the cache line address.

The full implementation includes a number of features that help reliability and throughput:
- Results for each 2MiB page are stored in a binary file using the 2MiB-aligned base address as part of the name.  Before performing the tests on a 2MiB range the code tests to see if that 2MiB page has already been mapped, is readable, and contains 32768 byte entries.
- Several heuristics are applied when reviewing the LLC_LOOKUP.READ data to identify most cases of contention.  If the heuristics fail, the testing for the line is repeated.  After a number of repeats the code sleeps for 1 second (to allow a bit more time for a conflicting process to complete).  The code aborts if passing results are not obtained for a cache line after 10 back-off sleeps. Because of feature (a), a new test can be launched at any time and will not repeat any of the mappings already completed.
- To avoid repeatedly checking the same 2MiB physical address in consecutive runs, the code does not access the 2MiB virtual address regions contiguously.  A large prime stride is used with modulo indexing to test virtual addresses higher in the buffer's range -- these are more likely to be mapped to 2MiB physical pages that have not yet been tested.

## References and Notes

[^1]: The presence of strong conflicts in the Snoop Filters was presented at the [IXPUG 2018 Fall Conference](https://www.ixpug.org/events/ixpug-fallconf-2018) [presentation](https://www.ixpug.org/components/com_solutionlibrary/assets/documents/1538092216-IXPUG_Fall_Conf_2018_paper_20%20-%20John%20McCalpin.pdf).  The performance impact on the High Performance LINPACK benchmark and the DGEMM matrix multiplication kernel was the subject of a [paper](https://ieeexplore.ieee.org/document/8665801) at the SuperComputing 2018 conference, with [annotated slides](https://sites.utexas.edu/jdm4372/2019/01/07/sc18-paper-hpl-and-dgemm-performance-variability-on-intel-xeon-platinum-8160-processors/).
