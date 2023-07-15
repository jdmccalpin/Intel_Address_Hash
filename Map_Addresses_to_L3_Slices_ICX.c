// John D. McCalpin, mccalpin@tacc.utexas.edu
static char const rcsid[] = "$Id: SnoopFilterMapper.c,v 1.3 2023/01/12 17:53:11 mccalpin Exp mccalpin $";

// include files
#include <stdio.h>				// printf, etc
#include <stdint.h>				// standard integer types, e.g., uint32_t
#include <signal.h>				// for signal handler
#include <stdlib.h>				// exit() and EXIT_FAILURE
#include <string.h>				// strerror() function converts errno to a text string for printing
#include <fcntl.h>				// for open()
#include <errno.h>				// errno support
#include <assert.h>				// assert() function
#include <unistd.h>				// sysconf() function, sleep() function
#include <sys/mman.h>			// support for mmap() function
#include <linux/mman.h>			// required for 1GiB page support in mmap()
#include <math.h>				// for pow() function used in RAPL computations
#include <time.h>
#include <sys/time.h>			// for gettimeofday

# define ARRAYSIZE 2147483648L

// MYHUGEPAGE_1GB overrides default of 2MiB for hugepages
#if defined MYHUGEPAGE_1GB
#define MYPAGESIZE 1073741824UL
#define NUMPAGES 2L
#define PAGES_MAPPED 2L			// this is still specifying how many 2MiB pages to map
#else
#define MYPAGESIZE 2097152L
#define NUMPAGES 2048L			// 40960L (80 GiB) for big production runs
#define PAGES_MAPPED 16L		// 128L or 256L for production runs
#endif // MYHUGEPAGE_1GB


#define SPECIAL_VALUE (-1)

// interfaces for va2pa_lib.c
void print_pagemap_entry(unsigned long long pagemap_entry);
unsigned long long get_pagemap_entry( void * va );

int dumpall;			// when set to 1, will cause dump of lots of stuff for debugging
int report;
int nwraps;				// track number of performance counter wraps

double *array;					// array pointer to mmap on 1GiB pages
double *page_pointers[NUMPAGES];		// one pointer for each page allocated
uint64_t pageframenumber[NUMPAGES];	// one PFN entry for each page allocated

// constant value defines
# define NUM_SOCKETS 2				// 
# define NUM_IMC_CHANNELS 8			// includes channels on all IMCs in a socket
# define NUM_IMC_COUNTERS 5			// 0-3 are the 4 programmable counters, 4 is the fixed-function DCLK counter
# define NUM_CHA_BOXES 40
# define NUM_CHA_USED 40
# define NUM_CHA_COUNTERS 4

long imc_counts[NUM_SOCKETS][NUM_IMC_CHANNELS][NUM_IMC_COUNTERS][2];	// including the fixed-function (DCLK) counter as the final entry
long imc_pkg_sums[NUM_SOCKETS][NUM_IMC_COUNTERS];						// sum across channels for each chip
char imc_event_name[NUM_SOCKETS][NUM_IMC_CHANNELS][NUM_IMC_COUNTERS][32];		// reserve 32 characters for the IMC event names for each socket, channel, counter
uint32_t imc_perfevtsel[NUM_IMC_COUNTERS];			// expected control settings for the counters
uint32_t imc_vid_did[3];							// PCIe configuration space vendor and device IDs for the IMC blocks 
long cha_counts[NUM_SOCKETS][NUM_CHA_BOXES][NUM_CHA_COUNTERS][2];		// 2 sockets, 28 tiles per socket, 4 counters per tile, 2 times (before and after)
uint64_t cha_perfevtsel[NUM_CHA_COUNTERS];
long cha_pkg_sums[NUM_SOCKETS][NUM_CHA_COUNTERS];

#define MAXCORES 112
#define CORES_USED 28
// New feature -- core counters.
// upgrade to include counters for all cores 
long core_counters[MAXCORES][4][2];					// 24 cores & 24 threads on one socket, 4 counters, before and after
long fixed_counters[MAXCORES][4][2];				// 24 cores with 4 fixed-function core counters (Instr, CoreCyc, RefCyc, TSC)
long core_pkg_sums[NUM_SOCKETS][4];					// four core counters
long fixed_pkg_sums[NUM_SOCKETS][4];				// four fixed-function counters per core (Instr, CoreCyc, RefCyc, TSC)

int8_t cha_by_page[NUMPAGES][32768];				// L3 numbers for each of the 32,768 cache lines in each of the first PAGES_MAPPED 2MiB pages
uint64_t paddr_by_page[NUMPAGES];					// physical addresses of the base of each of the first PAGES_MAPPED 2MiB pages used
long lines_by_cha[NUM_CHA_USED];			// bulk count of lines assigned to each CHA

#ifdef DEBUG
FILE *log_file;					// log file for debugging -- should not be needed in production
#endif // DEBUG
unsigned int *mmconfig_ptr;         // must be pointer to 32-bit int so compiler will generate 32-bit loads and stores

struct timeval tp;		// seconds and microseconds from gettimeofday
struct timezone tzp;	// required, but not used here.

double ssum(double *a, long vl);

double mysecond()
{
        struct timeval tp;
        struct timezone tzp;
        int i;

        i = gettimeofday(&tp,&tzp);
        return ( (double) tp.tv_sec + (double) tp.tv_usec * 1.e-6 );
}

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif


#include "low_overhead_timers.c"

#ifdef IMC_COUNTS
#include "SKX_IMC_BusDeviceFunctionOffset.h"
#endif // IMC_COUNTS

#include "MSR_defs.h"

// ===========================================================================================================================================================================
// Convert PCI(bus:device.function,offset) to uint32_t array index
uint32_t PCI_cfg_index(unsigned int Bus, unsigned int Device, unsigned int Function, unsigned int Offset)
{
    uint32_t byteaddress;
    uint32_t index;
    assert (Device >= 0);
    assert (Function >= 0);
    assert (Offset >= 0);
    assert (Device < (1<<5));
    assert (Function < (1<<3));
    assert (Offset < (1<<12));
    byteaddress = (Bus<<20) | (Device<<15) | (Function<<12) | Offset;
    index = byteaddress / 4;
    return ( index );
}

// Compute the difference of 48-bit counter values, correcting
// for a single overflow of the counter if necessary
long corrected_delta48(int tag, long end, long start)
{
    long result;
	int i;
    if (end >= start) {
        result = (long) (end - start);
    } else {
        // result = (long) ((end + (1UL<<48)) - start);
        result = 0;
		if (report == 1) {
			nwraps++;
			dumpall = 1;
			i = gettimeofday(&tp,&tzp);
			printf("DEBUG: wrap detected at %ld.%.6ld tag %d end %ld (0x%lx) start %ld (0x%lx) result %ld (0x%lx)\n",tp.tv_sec,tp.tv_usec,tag,end,end,start,start,result,result);
		}
    }
    return (result);
}

// ===========================================================================================================================================================================
int main(int argc, char *argv[])
{
	// local declarations
	// int cpuid_return[4];
	int i;
	int retries;
	int zeros;
	int tag;
	int rc;
	ssize_t rc64;
	char description[100];
	size_t len;
	long arraylen;
	long l2_contained_size, inner_repetitions;
	unsigned long pagemapentry;
	unsigned long paddr, basephysaddr;
	unsigned long pagenum, basepagenum;
	uint32_t bus, device, function, offset, ctl_offset, ctr_offset, value, index;
	uint32_t socket, imc, channel, counter, controller;
	long count,delta;
	long j,k,page_number,page_base_index,line_number;
	long jstart[CORES_USED], jend[CORES_USED], mycore, vl[CORES_USED];
	uint32_t low_0, high_0, low_1, high_1;
	char filename[100];
	int pkg, tile;
	int nr_cpus;
	uint64_t msr_val, msr_num;
	int mem_fd;
	int msr_fd[2];				// one for each socket
	int proc_in_pkg[2];			// one Logical Processor number for each socket
	uid_t my_uid;
	gid_t my_gid;
	double sum,expected;
	double t0, t1;
	double avg_cycles;
	unsigned long tsc_start, tsc_end;
	double tsc_rate = 2.1e9;
	double sf_evict_rate;
	double bandwidth;
    unsigned long mmconfig_base=0x80000000;		// DOUBLE-CHECK THIS ON NEW SYSTEMS!!!!!   grep MMCONFIG /proc/iomem | awk -F- '{print $1}'
    unsigned long mmconfig_size=0x10000000;
	double private_sum,partial_sums[CORES_USED];
	long iters,iteration_counts[CORES_USED];
	long BaseOffset;

	BaseOffset = 0;
#ifdef RANDOMOFFSETS
	if (argc != 2) {
		printf("Must Provide a Random Offset cache line offset value (an integer between 0 and 2^24-375000 (16,402,216))\n");
		exit(1);
	} else {
		BaseOffset = atol(argv[1]);
		printf("Random Cache Line Offset is %ld\n",BaseOffset);
		BaseOffset = BaseOffset*8;
		printf("Starting index for summation is %ld\n",BaseOffset);
	}
#endif // RANDOMOFFSETS

	retries = 0;
	zeros = 0;
	report = 1;
	dumpall = 0;
	nwraps = 0;
	l2_contained_size = 125000 * CORES_USED;		// about 95% of the L2 space in the cores used
	for (i=0; i<CORES_USED; i++) {
		iters = 0;
		jstart[i] = BaseOffset + i*l2_contained_size/CORES_USED;
		jend[i] = jstart[i] + l2_contained_size/CORES_USED;
		vl[i] = jend[i]-jstart[i];
		printf("thread %d jstart %ld jend %ld vl %ld\n",i,jstart[i],jend[i],vl[i]);

		partial_sums[i] = 0.0;
		iteration_counts[i] = 0;
		for (counter=0; counter<4; counter++) {
			core_counters[i][counter][0] = SPECIAL_VALUE;
			core_counters[i][counter][1] = SPECIAL_VALUE;
			fixed_counters[i][counter][0] = SPECIAL_VALUE;
			fixed_counters[i][counter][1] = SPECIAL_VALUE;
		}
	}
	// initialize the array that will hold the L3 numbers for each cache line for each of the first NUMPAGES 2MiB pages
	for (i=0; i<NUMPAGES; i++) {
		for (line_number=0; line_number<32768; line_number++) {
			cha_by_page[i][line_number] = -1; 	// special value -- if set properly, all values should be in the range of 0..23
		}
	}

	// allocate working array on a huge pages -- either 1GiB or 2MiB
	len = NUMPAGES * MYPAGESIZE;
#if defined MYHUGEPAGE_1GB
	array = (double*) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0 );
#elif defined MYHUGEPAGE_THP
	//array = (double*) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0 );
	rc = posix_memalign((void **)&array, (size_t) 2097152, (size_t) len);
	if (rc != 0) {
		printf("ERROR: posix_memalign call failed with error code %d\n",rc);
		exit(3);
	}
#else
	array = (double*) mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0 );
#endif // MYHUGEPAGE_1GB
	if (array == (void *)(-1)) {
        perror("ERROR: mmap of array a failed! ");
        exit(1);
    }
	// initialize working array
	arraylen = NUMPAGES * MYPAGESIZE/sizeof(double);
// #pragma omp parallel for
	for (j=0; j<arraylen; j++) {
		array[j] = 1.0;
	}
	// initialize page_pointers to point to the beginning of each page in the array
	// then get and print physical addresses for each
#ifdef VERBOSE
	printf(" Page    ArrayIndex            VirtAddr        PagemapEntry         PFN           PhysAddr\n");
#endif // VERBOSE
	for (j=0; j<NUMPAGES; j++) {
		k = j*MYPAGESIZE/sizeof(double);
		page_pointers[j] = &array[k];
		pagemapentry = get_pagemap_entry(&array[k]);
		pageframenumber[j] = (pagemapentry & (unsigned long) 0x007FFFFFFFFFFFFF);
#ifdef VERBOSE
		printf(" %.5ld   %.10ld  %#18lx  %#18lx  %#18lx  %#18lx\n",j,k,&array[k],pagemapentry,pageframenumber[j],(pageframenumber[j]<<12));
#endif // VERBOSE
	}
	printf("PAGE_ADDRESSES ");
	for (j=0; j<NUMPAGES; j++) {
		basephysaddr = pageframenumber[j] << 12;
		paddr_by_page[j] = basephysaddr;
		printf("0x%.12lx ",paddr_by_page[j]);
        if (j%12 == 0) printf("\n");
	}
	printf("\n");


	// initialize arrays for counter data
	for (socket=0; socket<NUM_SOCKETS; socket++) {
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				imc_counts[socket][channel][counter][0] = 0;
				imc_counts[socket][channel][counter][1] = 0;
			}
		}
		for (tile=0; tile<NUM_CHA_USED; tile++) {
			lines_by_cha[tile] = 0;
			for (counter=0; counter<4; counter++) {
				cha_counts[socket][tile][counter][0] = 0;
				cha_counts[socket][tile][counter][1] = 0;
			}
		}
	}

	// get the host name, assume that it is of the TACC standard form, and use this as part
	// of the log file name....  Standard form is "c263-109.stampede2.tacc.utexas.edu", so
	// truncating at the first "." is done by writing \0 to character #8.
	len = 100;	
	rc = gethostname(description, len);
	if (rc != 0) {
		fprintf(stderr,"ERROR when trying to get hostname\n");
		exit(-1);
	}
	description[8] = 0;		// assume hostname of the form c263-109.stampede2.tacc.utexas.edu -- truncate after first period

	my_uid = getuid();
	my_gid = getgid();

#ifdef DEBUG
	sprintf(filename,"log.%s.perf_counters",description);
	// sprintf(filename,"log.perf_counters");
	log_file = fopen(filename,"w+");
	if (log_file == 0) {
		fprintf(stderr,"ERROR %s when trying to open log file %s\n",strerror(errno),filename);
		exit(-1);
	}

	fprintf(log_file,"DEBUG: my uid is %d, my gid is %d\n",my_uid,my_gid);

	rc = chown(filename,my_uid,my_gid);
	if (rc == 0) {
		fprintf(log_file,"DEBUG: Successfully changed ownership of log file to %d %d\n",my_uid,my_gid);
	} else {
		fprintf(stderr,"ERROR: Attempt to change ownership of log file failed -- bailing out\n");
		exit(-1);
	}
#endif // DEBUG

	//========================================================================================================================
	// initial checks
	// 		is this a supported core?  (CPUID Family/Model)
	//      Every processor that I am going to see will be Family 0x06 (no ExtFamily needed).
	//      The DisplayModel field is (ExtModel<<4)+Model and should be 0x3F for all Xeon E5 v3 systems
	int leaf = 1;
	int subleaf = 0;
	uint32_t eax, ebx, ecx, edx;
	__asm__ __volatile__ ("cpuid" : \
		  "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (leaf), "c" (subleaf));

	// Alternate form: 
	// 		The compiler cpuid intrinsics are not documented by Intel -- they use the Microsoft format
	// 			described at https://msdn.microsoft.com/en-us/library/hskdteyh.aspx
	// 			__cpuid(array to hold eax,ebx,ecx,edx outputs, initial eax value)
	// 			__cpuidex(array to hold eax,ebx,ecx,edx outputs, initial eax value, initial ecx value)
	//      CPUID function 0x01 returns the model info in eax.
	//      		27:20 ExtFamily	-- expect 0x00
	//      		19:16 ExtModel	-- expect 0x3 for HSW, 0x5 for SKX
	//      		11:8  Family	-- expect 0x6
	//      		7:4   Model		-- expect 0xf for HSW, 0x5 for SKX
	// __cpuid(&cpuid_return[0], 1);
	// uint32_t ModelInfo = cpuid_return[0] & 0x0fff0ff0;	// mask out the reserved and "stepping" fields, leaving only the based and extended Family/Model fields

	uint32_t ModelInfo = eax & 0x0fff0ff0;	// mask out the reserved and "stepping" fields, leaving only the based and extended Family/Model fields
    //uint32_t ExpectedModel = 0x00050650;    // SKX/CLX
    uint32_t ExpectedModel = 0x000606a0;     // ICX
	if (ModelInfo != ExpectedModel) {
		fprintf(stderr,"ERROR -- this does not appear to be the correct processor type!!!\n");
		fprintf(stderr,"ERROR -- Expected CPUID(0x01) Family/Model bits = 0x%x, but found 0x%x\n",ExpectedModel,ModelInfo);
		exit(1);
	}

#ifdef IMC_COUNTS
	// ===================================================================================================================
	// ------------------ REQUIRES ROOT PERMISSIONS ------------------
	// open /dev/mem for PCI device access and mmap() a pointer to the beginning
	// of the 256 MiB PCI Configuration Space.
	// 		check VID/DID for uncore bus:device:function combinations
	//   Note that using /dev/mem for PCI configuration space access is required for some devices on KNL.
	//   It is not required on other systems, but it is not particularly inconvenient either.
	sprintf(filename,"/dev/mem");
#ifdef DEBUG
	fprintf(log_file,"opening %s\n",filename);
#endif // DEBUG
	mem_fd = open(filename, O_RDWR);
	if (mem_fd == -1) {
		fprintf(stderr,"ERROR %s when trying to open %s\n",strerror(errno),filename);
		exit(-1);
	}
	int map_prot = PROT_READ | PROT_WRITE;
	mmconfig_ptr = mmap(NULL, mmconfig_size, map_prot, MAP_SHARED, mem_fd, mmconfig_base);
    if (mmconfig_ptr == MAP_FAILED) {
        fprintf(stderr,"cannot mmap base of PCI configuration space from /dev/mem: address %lx\n", mmconfig_base);
        exit(2);
#ifdef DEBUG
    } else {
		fprintf(log_file,"Successful mmap of base of PCI configuration space from /dev/mem at address %lx\n", mmconfig_base);
#endif // DEBUG
	}
    close(mem_fd);      // OK to close file after mmap() -- the mapping persists until unmap() or program exit

	// New simple test that does not need to know the uncore bus numbers here...
	// Skylake bus 0, Function 5, offset 0 -- Sky Lake-E MM/Vt-d Configuration Registers
	//
	// simple test -- should return "20248086" on Skylake Xeon EP -- DID 0x2024, VID 0x8086
	bus = 0x00;
	device = 0x5;
	function = 0x0;
	offset = 0x0;
	index = PCI_cfg_index(bus, device, function, offset);
    value = mmconfig_ptr[index];
	if (value != 0x20248086) {
		fprintf(stderr,"ERROR: Bus %x device %x function %x offset %x expected %x, found %x\n",bus,device,function,offset,0x20248086,value);
		exit(3);
#ifdef DEBUG
	} else {
		fprintf(log_file,"DEBUG: Well done! Bus %x device %x function %x offset %x returns expected value of %x\n",bus,device,function,offset,value);
#endif // DEBUG
	}
#endif // IMC_COUNTS

#ifdef CHA_COUNTS
	// ===================================================================================================================
	// open the MSR driver using one core in socket 0 and one core in socket 1
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    proc_in_pkg[0] = 0;                 // logical processor 0 is in socket 0 in all TACC systems
    proc_in_pkg[1] = nr_cpus-1;         // logical processor N-1 is in socket 1 in all TACC 2-socket systems
	for (pkg=0; pkg<2; pkg++) {
		sprintf(filename,"/dev/cpu/%d/msr",proc_in_pkg[pkg]);
		msr_fd[pkg] = open(filename, O_RDWR);
		if (msr_fd[pkg] == -1) {
			fprintf(stderr,"ERROR %s when trying to open %s\n",strerror(errno),filename);
			exit(-1);
		}
	}
	for (pkg=0; pkg<2; pkg++) {
		pread(msr_fd[pkg],&msr_val,sizeof(msr_val),IA32_TIME_STAMP_COUNTER);
		fprintf(stdout,"DEBUG: TSC on core %d socket %d is %ld\n",proc_in_pkg[pkg],pkg,msr_val);
	}

    int core_under_test, socket_under_test;
    tsc_start = full_rdtscp(&socket_under_test, &core_under_test);

	pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),0x186);
	printf("Core PerfEvtSel0 0x%lx\n",msr_val);
	pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),0x187);
	printf("Core PerfEvtSel1 0x%lx\n",msr_val);
	pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),0x188);
	printf("Core PerfEvtSel2 0x%lx\n",msr_val);
	pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),0x189);
	printf("Core PerfEvtSel3 0x%lx\n",msr_val);


	// Program the CHA mesh counters
	//
	// Updated for ICX -- several uglies here....
	//   Each CHA has a block of 14 MSRs reserved, of which 11 are used
	//   The base for the first 18 CHAs (0-17) is 0xE00 + 0x0E*CHA
	//          for CHAs 18-33 add 0x0E to the computed value
	//          for CHAs 23-39 subtract 1148 from the computed value
	//   Within each block:
	//   	Unit Control is at offset 0x00
	//   	CTL0, 1, 2, 3 are at offsets 0x01, 0x02, 0x03, 0x04
	//   	CTR0, 1, 2, 3 are at offsets 0x08, 0x09, 0x0a, 0x0b
	//
	//   For the moment I think I can ignore the filter registers at offsets 0x05 and 0x06
	//     and the status register at offset 0x07
	//  For ICX one of the filter registers was dropped and the remaining filter register is 
	//     interpreted differently depending on the event being measured.
	//  ICX uses high-order bits in the perfevtsel registers, so these must be 64-bit variables.
	//
	//   The control register needs bit 22 set to enabled, then bits 15:8 as Umask and 7:0 as EventSelect
	//   Mesh Events:
	//   	HORZ_RING_BL_IN_USE = 0xab
	//   		LEFT_EVEN = 0x01
	//   		LEFT_ODD = 0x02
	//   		RIGHT_EVEN = 0x04
	//   		RIGHT_ODD = 0x08
	//   	VERT_RING_BL_IN_USE = 0xaa
	//   		UP_EVEN = 0x01
	//   		UP_ODD = 0x02
	//   		DN_EVEN = 0x04
	//   		DN_ODD = 0x08
	//   For starters, I will combine even and odd and create 4 events
	//   	0x004003ab	HORZ_RING_BL_IN_USE.LEFT
	//   	0x00400cab	HORZ_RING_BL_IN_USE.RIGHT
	//   	0x004003aa	VERT_RING_BL_IN_USE.UP
	//   	0x00400caa	VERT_RING_BL_IN_USE.DN

	// first set to try....
//	cha_perfevtsel[0] = 0x004003ab;		// HORZ_RING_BL_IN_USE.LEFT
//	cha_perfevtsel[1] = 0x00400cab;		// HORZ_RING_BL_IN_USE.RIGHT
//	cha_perfevtsel[2] = 0x004003aa;		// VERT_RING_BL_IN_USE.UP
//	cha_perfevtsel[3] = 0x00400caa;		// VERT_RING_BL_IN_USE.DN

	// second set to try....
//	cha_perfevtsel[0] = 0x004001ab;		// HORZ_RING_BL_IN_USE.LEFT_EVEN
//	cha_perfevtsel[1] = 0x004002ab;		// HORZ_RING_BL_IN_USE.LEFT_ODD
//	cha_perfevtsel[2] = 0x004004ab;		// HORZ_RING_BL_IN_USE.RIGHT_EVEN
//	cha_perfevtsel[3] = 0x004008ab;		// HORZ_RING_BL_IN_USE.RIGHT_ODD
//
// ==================================================================================================================
// https://download.01.org/perfmon/ICX/icelakex_uncore_v1.06.json
// {
//    "Unit": "CHA",
//    "EventCode": "0x34",
//    "UMask": "0xFF",
//    "PortMask": "0x00",
//    "FCMask": "0x00",
//    "UMaskExt": "0x1BC1",
//    "EventName": "UNC_CHA_LLC_LOOKUP.DATA_READ",
//    "BriefDescription": "Cache and Snoop Filter Lookups; Data Read Request",
//    "PublicDescription": "Counts the number of times the LLC was accessed - this includes code, data, prefetches and hints coming from L2.  This has numerous filters available.  Note the non-standard filtering equation.  This event will count requests that lookup the cache multiple times with multiple increments.  One must ALWAYS set umask bit 0 and select a state or states to match.  Otherwise, the event will count nothing.   CHAFilter0[24:21,17] bits correspond to [FMESI] state. Read transactions",
//    "Counter": "0,1,2,3",
//    "MSRValue": "0x00",
//    "ELLC": "0",
//    "Filter": "na",
//    "ExtSel": "0",
//    "Deprecated": "0",
//    "FILTER_VALUE": "0",
//    "CounterType": "PGMABLE"
//  },
// ------------------------------------------------------------------------------------------------------------------
// https://download.01.org/perfmon/ICX/icelakex_uncore_v1.06_experimental.json
// Lists many versions of this event without the high-order bits
//   0x00400134 UNC_CHA_LLC_LOOKUP.I        LLC misses
//   0x00400234 UNC_CHA_LLC_LOOKUP.SF_S     SF hit S
//   0x00400434 UNC_CHA_LLC_LOOKUP.SF_E     SF hit E
//   0x00400834 UNC_CHA_LLC_LOOKUP.SF_H     SF hit H (HitMe state)
//   0x00401034 UNC_CHA_LLC_LOOKUP.S        LLC HitS
//   0x00402034 UNC_CHA_LLC_LOOKUP.E        LLC HitE
//   0x00404034 UNC_CHA_LLC_LOOKUP.M        LLC HitM
//   0x00408034 UNC_CHA_LLC_LOOKUP.F        LLC HitF
// And many more with the high-order bits
//   0x0000 1bc8 0040 ff34 UNC_CHA_LLC_LOOKUP.RFO
//   0x0000 1fff 0040 ff34 UNC_CHA_LLC_LOOKUP.ALL
//   0x0000 1bc1 0040 ff34 UNC_CHA_LLC_LOOKUP.DATA_READ
//   0x0000 1a44 0040 ff34 UNC_CHA_LLC_LOOKUP.FLUSH_INV
//   0x0000 1bd0 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ
//   0x0000 0bdf 0040 ff34 UNC_CHA_LLC_LOOKUP.LOC_HOM
//   0x0000 15df 0040 ff34 UNC_CHA_LLC_LOOKUP.REM_HOM
//   0x0000 1a04 0040 ff34 UNC_CHA_LLC_LOOKUP.FLUSH_INV_REMOTE
//   0x0000 1a01 0040 ff34 UNC_CHA_LLC_LOOKUP.DATA_READ_REMOTE
//   0x0000 1a08 0040 ff34 UNC_CHA_LLC_LOOKUP.RFO_REMOTE
//   0x0000 1a10 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ_REMOTE
//   0x0000 1c19 0040 ff34 UNC_CHA_LLC_LOOKUP.REMOTE_SNP
//   0x0000 1844 0040 ff34 UNC_CHA_LLC_LOOKUP.FLUSH_INV_LOCAL
//   0x0000 19c1 0040 ff34 UNC_CHA_LLC_LOOKUP.DATA_READ_LOCAL
//   0x0000 19c8 0040 ff34 UNC_CHA_LLC_LOOKUP.RFO_LOCAL
//   0x0000 19d0 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ_LOCAL
//   0x0000 189d 0040 ff34 UNC_CHA_LLC_LOOKUP.LLCPREF_LOCAL
//   0x0000 0008 0040 **34 UNC_CHA_LLC_LOOKUP.RFO_F
//   0x0000 0800 0040 **34 UNC_CHA_LLC_LOOKUP.LOCAL_F
//   0x0000 1000 0040 **34 UNC_CHA_LLC_LOOKUP.REMOTE_F
//   0x0000 0400 0040 **34 UNC_CHA_LLC_LOOKUP.REMOTE_SNOOP_F
//   0x0000 0020 0040 **34 UNC_CHA_LLC_LOOKUP.ANY_F
//   0x0000 0001 0040 **34 UNC_CHA_LLC_LOOKUP.DATA_READ_F
//   0x0000 0002 0040 **34 UNC_CHA_LLC_LOOKUP.OTHER_REQ_F       Writebacks to the LLC
//   0x0000 0004 0040 **34 UNC_CHA_LLC_LOOKUP.FLUSH_OR_INV_F
//   0x0000 0010 0040 **34 UNC_CHA_LLC_LOOKUP.CODE_READ_F
//   0x0000 0040 0040 **34 UNC_CHA_LLC_LOOKUP.COREPREF_OR_DMND_LOCAL_F
//   0x0000 0080 0040 **34 UNC_CHA_LLC_LOOKUP.LLCPREF_LOCAL_F
//   0x0000 0200 0040 **34 UNC_CHA_LLC_LOOKUP.PREF_OR_DMND_REMOTE_F
//   0x0000 1bc1 0040 0134 UNC_CHA_LLC_LOOKUP.DATA_READ_MISS
//   0x0000 1e20 0040 ff34 UNC_CHA_LLC_LOOKUP.ALL_REMOTE
//   0x0000 1bd0 0040 0134 UNC_CHA_LLC_LOOKUP.CODE_READ_MISS
//   0x0000 1bc8 0040 0134 UNC_CHA_LLC_LOOKUP.RFO_MISS
//   0x0000 1bc8 0040 0134 UNC_CHA_LLC_LOOKUP.RFO_MISS
//   0x0000 1bd9 0040 0134 UNC_CHA_LLC_LOOKUP.READ_MISS
//   0x0000 0bd9 0040 0134 UNC_CHA_LLC_LOOKUP.READ_MISS_LOC_HOM
//   0x0000 13d9 0040 0134 UNC_CHA_LLC_LOOKUP.READ_MISS_REM_HOM
//   0x0000 09d9 0040 ff34 UNC_CHA_LLC_LOOKUP.READ_LOCAL_LOC_HOM
//   0x0000 0a19 0040 ff34 UNC_CHA_LLC_LOOKUP.READ_REMOTE_LOC_HOM
//   0x0000 11d9 0040 ff34 UNC_CHA_LLC_LOOKUP.READ_LOCAL_REM_HOM
//   0x0000 1bd9 0040 0e34 UNC_CHA_LLC_LOOKUP.READ_SF_HIT
//   0x0000 1619 0040 0134 UNC_CHA_LLC_LOOKUP.READ_OR_SNOOP_REMOTE_MISS_REM_HOME
//   0x0000 1a42 0040 ff34 UNC_CHA_LLC_LOOKUP.WRITES_AND_OTHER
//   0x0000 0bdf 0040 ff34 UNC_CHA_LLC_LOOKUP.LOC_HOM
//   0x0000 15df 0040 ff34 UNC_CHA_LLC_LOOKUP.REM_HOM
//   0x0000 1a10 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ_REMOTE
//   0x0000 19d0 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ_LOCAL
//   0x0000 189d 0040 ff34 UNC_CHA_LLC_LOOKUP.LLCPREF_LOCAL
//   0x0000 1bd0 0040 ff34 UNC_CHA_LLC_LOOKUP.CODE_READ
// ==================================================================================================================

	// I DON'T UNDERSTAND INTEL'S DOCUMENTATION FOR THE LLC_LOOKUP COUNTER 
	// cha_perfevtsel[0] = 0x0040073d;		// SF_EVICTION S,E,M states
	// cha_perfevtsel[1] = 0x00001bc10040ff34;		// LLC_LOOKUP.DATA_READ
	// cha_perfevtsel[1] = 0x000019c10040ff34;		// LLC_LOOKUP.DATA_READ_LOCAL
	cha_perfevtsel[0] = 0x00400134;		// LLC_LOOKUP.MISS
	cha_perfevtsel[1] = 0x00400350;		// REQUESTS.READS
	cha_perfevtsel[2] = 0x00400353;	    // DIR_LOOKUP.ANY
	cha_perfevtsel[3] = 0x0040ff34;     // LLC_LOOKUP.ANY
//	uint64_t cha_filter0 = 0x01e20000;		// set bits 24,23,22,21,17 FMESI -- all LLC lookups, no SF lookups -- DO NOT USE ON ICX

// these events seem to work in the mesh traffic tracking code.....
//    cha_perfevtsel[0] = 0x004003b8;     // HORZ_RING_BL_IN_USE.LEFT
//    cha_perfevtsel[1] = 0x00400cb8;     // HORZ_RING_BL_IN_USE.RIGHT
//    cha_perfevtsel[2] = 0x004003b2;     // VERT_RING_BL_IN_USE.UP
//    cha_perfevtsel[3] = 0x00400cb2;     // VERT_RING_BL_IN_USE.DN


//	printf("CHA FILTER0 0x%lx\n",cha_filter0);          # DO NOT USE ON ICX

#ifdef VERBOSE
	printf("VERBOSE: programming CHA counters\n");
#endif // VERBOSE

    int msr_base;
    int msr_stride = 0x0e;
	for (pkg=0; pkg<2; pkg++) {
		for (tile=0; tile<NUM_CHA_USED; tile++) {
            if (tile >= 34) {
                msr_base = 0x0e00 - 0x47c;       // ICX MSRs skip backwards for CHAs 34-39
            } else if (tile >= 18) {              // ICX MSRs skil forward for CHAs 18-33
                msr_base = 0x0e00 + 0x0e;
            } else {
                msr_base = 0x0e00;
            }
			msr_num = msr_base + msr_stride*tile;		// unit control register -- write bit 2 to clear counters
			msr_val = 0x2;
			// printf("DEBUG: %d %lx %ld %lx\n",msr_fd[pkg],msr_val,sizeof(msr_val),msr_num);
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = msr_base + msr_stride*tile + 1;	// ctl0
			msr_val = cha_perfevtsel[0];
			// printf("DEBUG: %d %lx %ld %lx\n",msr_fd[pkg],msr_val,sizeof(msr_val),msr_num);
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = msr_base + msr_stride*tile + 2;	// ctl1
			msr_val = cha_perfevtsel[1];
			// printf("DEBUG: %d %lx %ld %lx\n",msr_fd[pkg],msr_val,sizeof(msr_val),msr_num);
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = msr_base + msr_stride*tile + 3;	// ctl2
			msr_val = cha_perfevtsel[2];
			// printf("DEBUG: %d %lx %ld %lx\n",msr_fd[pkg],msr_val,sizeof(msr_val),msr_num);
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
			msr_num = msr_base + msr_stride*tile + 4;	// ctl3
			msr_val = cha_perfevtsel[3];
			// printf("DEBUG: %d %lx %ld %lx\n",msr_fd[pkg],msr_val,sizeof(msr_val),msr_num);
			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
//			msr_num = msr_base + msr_stride*tile + 5;	// filter0              # NOT USED IN THE SAME WAY IN ICX
//			msr_val = cha_filter0;				// bits 24:21,17 FMESI -- all LLC lookups, not not SF lookups
//			pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
		}
	}
    // document CHA counter programming in output
    for (counter=0; counter<NUM_CHA_COUNTERS; counter++) {
        printf("INFO: CHA_PERFEVTSEL[%d] = 0x%lx\n",counter,cha_perfevtsel[counter]);
    }

#ifdef VERBOSE
	printf("VERBOSE: finished programming CHA counters\n");
#endif // VERBOSE

    // Hit the global "unfreeze counter" function in the uncore global control on each chip
    // Enable the uncore fixed clock while I am at it....
	for (pkg=0; pkg<2; pkg++) {
        msr_num = U_MSR_PMON_GLOBAL_CTL;
        msr_val = (1UL)<<61;
        pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);

        msr_num = U_MSR_PMON_FIXED_CTL;
        msr_val = 0x00400000UL;
        pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
    }
#ifdef VERBOSE
	printf("VERBOSE: Triggered UNFREEZE on all Uncore Counters, and enabled Uncore Clock Counter (MSR 0x704)\n");
#endif // VERBOSE
#endif // CHA_COUNTS

#ifdef IMC_COUNTS
	// ===================================================================================================================
	// Read the current programming of the IMC counters and look for the standard values (in this order)
	//     CAS_COUNT.READS		Event 0x04, Umask 0x03
	//     CAS_COUNT.WRITES		Event 0x04, Umask 0x0C
	//     ACT.ALL				Event 0x01, Umask 0x0B
	//     PRE_COUNT.MISS		Event 0x02, Umask 0x01
	//     DCLK

#ifdef VERBOSE
	printf("Preparing to program IMC counters\n");
#endif // VERBOSE
	// expected values of IMC performance counter event select control registers
	imc_perfevtsel[0] = 0x00400304;		// CAS_COUNT.READS
	imc_perfevtsel[1] = 0x00400C04;		// CAS_COUNT.WRITES
	imc_perfevtsel[2] = 0x00400B01;		// ACT_COUNT.ALL
	imc_perfevtsel[3] = 0x00400102;		// PRE_COUNT.MISS
	imc_perfevtsel[4] = 0x00400000;		// DCLK

	imc_vid_did[0] = 0x20428086;		// all channel 0 devices are 2042
	imc_vid_did[1] = 0x20468086;		// all channel 1 devices are 2046
	imc_vid_did[2] = 0x204a8086;		// all channel 2 devices are 204a

	printf("IMC PerfEvtSel0 0x%lx\n",imc_perfevtsel[0]);
	printf("IMC PerfEvtSel1 0x%lx\n",imc_perfevtsel[1]);
	printf("IMC PerfEvtSel2 0x%lx\n",imc_perfevtsel[2]);
	printf("IMC PerfEvtSel3 0x%lx\n",imc_perfevtsel[3]);
	printf("IMC PerfEvtSel4 0x%lx\n",imc_perfevtsel[4]);

	// print the full wall-clock time in seconds and microseconds
	// assume both components of tp struct are longs.
	fprintf(stdout,"# %s\n", rcsid);
    i = gettimeofday(&tp,&tzp);
	fprintf(stdout,"%ld %ld\n", tp.tv_sec,tp.tv_usec);

	for (socket=0; socket<NUM_SOCKETS; socket++) {
		bus = IMC_BUS_Socket[socket];
#ifdef VERBOSE
		printf("VERBOSE: socket %d bus %d\n",socket,bus);
#endif // VERBOSE
		for (channel=0; channel<NUM_IMC_CHANNELS; channel++) {
			device = IMC_Device_Channel[channel];
			function = IMC_Function_Channel[channel];
#ifdef VERBOSE
			printf("VERBOSE: channel %d device %d function %d\n",channel, device, function);
#endif // VERBOSE
			// check to make sure this is the correct device
			offset = 0x0;
			index = PCI_cfg_index(bus, device, function, offset);
			value = mmconfig_ptr[index];
			if ( value != imc_vid_did[channel%3]) {
				fprintf(stderr,"WARNING!!!! socket %d, channel %d has vid_did %x but should be %x\n",socket,channel,value,imc_vid_did[channel%3]);
			}
			for (counter=0; counter<NUM_IMC_COUNTERS; counter++) {
				// check to see if this unit is programmed correctly and reprogram if needed
				offset = IMC_PmonCtl_Offset[counter];
				index = PCI_cfg_index(bus, device, function, offset);
				value = mmconfig_ptr[index];
				if ( value != imc_perfevtsel[counter]) {
					fprintf(stderr,"WARNING!!!! socket %d, channel %d has perfevtsel %x but should be %x -- reprogramming\n",socket,channel,value,imc_perfevtsel[counter]);
					mmconfig_ptr[index] = imc_perfevtsel[counter];
				}

			}
		}
	}
#endif // IMC_COUNTS

// ========= END OF PERFORMANCE COUNTER SETUP ========================================================================

#ifdef MAP_L3
// ============== BEGIN L3 MAPPING TESTS ==============================
// For each of the NUMPAGES 2MiB pages:
//   1. Use "access()" to see if the mapping file already exists.
//		If exists:
//   		2. Use "stat()" to make sure the file is the correct size
//   		   If right size:
//   		   	3. Read the contents into the 32768-element int8_t array of L3 numbers.
//   		   Else (wrong size):
//   		   	4. Abort and tell the user to fix it manually.
//   	Else (not exists):
//   		4. Call the mapping function to re-compute the map
//   		5. Create mapping file
//   		6. Save data in mapping file
//   		7. Close output file

	FILE *ptr_mapping_file;
	int needs_mapping;
	int good, good_old, good_new, pass1, pass2, pass3, found, numtries;
    int backoffs;
	int min_count, max_count, sum_count, old_cha;
	double avg_count, goodness1, goodness2, goodness3;
	int globalsum = 0;
	long totaltries = 0;
	int NFLUSHES = 1000;
    int new_pages_mapped = 0;
    int primestride = 797;
	// for (page_number=0; page_number<PAGES_MAPPED; page_number++) {
	//for (page_number=0; page_number<NUMPAGES; page_number++) {
	for (int iii=0; iii<NUMPAGES; iii++) {
        page_number = (primestride * iii) % NUMPAGES;
		needs_mapping=0;
		sprintf(filename,"PADDR_0x%.12lx.map",paddr_by_page[page_number]);
		i = access(filename, F_OK);
		if (i == -1) {								// file does not exist
			printf("DEBUG: Mapping file %s does not exist -- will create file after mapping cache lines\n",filename);
			needs_mapping = 1;
		} else {									// file exists
			i = access(filename, R_OK);
			if (i == -1) {							// file exists without read permissions
				printf("ERROR: Mapping file %s exists, but without read permission\n",filename);
				exit(1);
			} else {								// file exists with read permissions
				ptr_mapping_file = fopen(filename,"r");
				if (!ptr_mapping_file) {
					printf("ERROR: Failed to open Mapping File %s, should not happen\n",filename);
					exit(2);
				}
				k = fread(&cha_by_page[page_number][0],(size_t) 32768,(size_t) 1,ptr_mapping_file);
				if (k != 1) {					// incorrect read length
					printf("ERROR: Read from Mapping File %s, returned the wrong record count %ld expected 1\n",filename,k);
					exit(3);
				} else {							// correct read length
					printf("DEBUG: Mapping File read for %s succeeded -- skipping mapping for this page\n",filename);
					needs_mapping = 0;
				}
			}
		}
		if (needs_mapping == 1) {
			// code imported from SystemMirrors/Hikari/MemSuite/InterventionLatency/L3_mapping.c
#ifdef VERBOSE
			printf("DEBUG: here I need to perform the mapping for paddr 0x%.12lx, and then save the file\n",paddr_by_page[page_number]);
#endif // VERBOSE
			page_base_index = page_number*262144;		// index of element at beginning of current 2MiB page
			for (line_number=0; line_number<32768; line_number++) {
				good = 0;
				good_old = 0;
				good_new = 0;
				numtries = 0;
                backoffs = 0;
#ifdef VERBOSE
				if (line_number%64 == 0) {
					pagemapentry = get_pagemap_entry(&array[page_base_index+line_number*8]);
					printf("DEBUG: page_base_index %ld line_number %ld index %ld pagemapentry 0x%lx\n",page_base_index,line_number,page_base_index+line_number*8,pagemapentry);
				}
#endif // VERBOSE
				do  {               // -------------- Inner Repeat Loop until results pass "goodness" tests --------------
					numtries++;
					if (numtries > 100) {
                        backoffs += 1;
                        sleep(1);
                        if ( backoffs > 10 ) {
                            printf("ERROR: No good results for line %d after %d tries and %d backoffs\n",line_number,numtries,backoffs);
                            exit(101);
                        }
					}
					totaltries++;
				// 1. read L3 counters before starting test
				for (tile=0; tile<NUM_CHA_USED; tile++) {
                    if (tile >= 34) {
                        msr_base = 0x0e00 - 0x47c;       // ICX MSRs skip backwards for CHAs 34-39
                    } else if (tile >= 18) {              // ICX MSRs skil forward for CHAs 18-33
                        msr_base = 0x0e00 + 0x0e;
                    } else {
                        msr_base = 0x0e00;
                    }
					msr_num = msr_base + msr_stride*tile + 0x8 + 1;				// counter 1 is the LLC_LOOKUPS.READ event
					pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),msr_num);
					cha_counts[socket_under_test][tile][1][0] = msr_val;					//  use the array I have already declared for cha counts
					// printf("DEBUG: page %ld line %ld msr_num 0x%x msr_val %ld cha_counter1 %lu\n",
					//		page_number,line_number,msr_num,msr_val,cha_counts[0][tile][1][0]);
				}

				// 2. Access the line many times
				sum = 0;
				for (i=0; i<NFLUSHES; i++) {
					sum += array[page_base_index+line_number*8];
					_mm_mfence();
					_mm_lfence();
					_mm_clflush(&array[page_base_index+line_number*8]);
					_mm_mfence();
					_mm_lfence();
				}
				globalsum += sum;

				// 3. read L3 counters after loads are done
				for (tile=0; tile<NUM_CHA_USED; tile++) {
                    if (tile >= 34) {
                        msr_base = 0x0e00 - 0x47c;       // ICX MSRs skip backwards for CHAs 34-39
                    } else if (tile >= 18) {              // ICX MSRs skil forward for CHAs 18-33
                        msr_base = 0x0e00 + 0x0e;
                    } else {
                        msr_base = 0x0e00;
                    }
					msr_num = msr_base + msr_stride*tile + 0x8 + 1;				// counter 1 is the LLC_LOOKUPS.READ event
					pread(msr_fd[socket_under_test],&msr_val,sizeof(msr_val),msr_num);
					cha_counts[socket_under_test][tile][1][1] = msr_val;					//  use the array I have already declared for cha counts
				}

#ifdef VERBOSE
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					printf("DEBUG: page %ld line %ld cha_counter1_after %lu cha_counter1 before %lu delta %lu\n",
							page_number,line_number,cha_counts[socket_under_test][tile][1][1],cha_counts[socket_under_test][tile][1][0],cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0]);
				}
#endif // VERBOSE

				//   CHA counter 1 set to LLC_LOOKUP.READ
				//
				//  4. Determine which L3 slice owns the cache line and
				//  5. Save the CHA number in the cha_by_page[page][line] array

				// first do a rough quantitative checks of the "goodness" of the data
				//		goodness1 = max/NFLUSHES (pass if >95%)
				// 		goodness2 = min/NFLUSHES (pass if <20%)
				//		goodness3 = avg/NFLUSHES (pass if <40%)
				max_count = 0;
				min_count = 1<<30;
				sum_count = 0;
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					max_count = MAX(max_count, cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0]);
					min_count = MIN(min_count, cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0]);
					sum_count += cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0];
				}
				avg_count = (double)(sum_count - max_count) / (double)(NUM_CHA_USED);
				goodness1 = (double) max_count / (double) NFLUSHES;
				goodness2 = (double) min_count / (double) NFLUSHES;
				goodness3 =          avg_count / (double) NFLUSHES;
				// compare the goodness parameters with manually chosen limits & combine into a single pass (good=1) or fail (good=0)
				pass1 = 0;
				pass2 = 0;
				pass3 = 0;
				if ( goodness1 > 0.95 ) pass1 = 1;
				if ( goodness2 < 0.20 ) pass2 = 1;
				if ( goodness3 < 0.40 ) pass3 = 1;
				good_new = pass1 * pass2 * pass3;
#ifdef VERBOSE
				printf("GOODNESS: line_number %ld max_count %d min_count %d sum_count %d avg_count %f goodness1 %f goodness2 %f goodness3 %f pass123 %d %d %d\n",
								  line_number, max_count, min_count, sum_count, avg_count, goodness1, goodness2, goodness3, pass1, pass2, pass3);
				if (good_new == 0) printf("DEBUG: one or more of the sanity checks failed for line=%ld: %d %d %d goodness values %f %f %f\n",
					line_number,pass1,pass2,pass3,goodness1,goodness2,goodness3);
#endif // VERBOSE

				// test to see if more than one CHA reports > 0.95*NFLUSHES events
				found = 0;
				old_cha = -1;
				int min_counts = (NFLUSHES*19)/20;
				for (tile=0; tile<NUM_CHA_USED; tile++) {
					if (cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0] >= min_counts) {
						old_cha = cha_by_page[page_number][line_number];
						cha_by_page[page_number][line_number] = tile;
						found++;
#ifdef VERBOSE
						if (found > 1) {
							printf("WARNING: Multiple (%d) CHAs found using counter 1 for cache line %ld, index %ld: old_cha %d new_cha %d\n",found,line_number,page_base_index+line_number*8,old_cha,cha_by_page[page_number][line_number]);
						}
#endif // VERBOSE
					}
				}
				if (found == 0) {
					good_old = 0;
#ifdef VERBOSE
					printf("WARNING: no CHA entry has been found for line %ld!\n",line_number);
					printf("DEBUG dump for no CHA found\n");
					for (tile=0; tile<NUM_CHA_USED; tile++) {
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,(cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0]));
					}
#endif // VERBOSE
				} else if (found == 1) {
					good_old = 1;
				} else {
					good_old = 0;
#ifdef VERBOSE
					printf("DEBUG dump for multiple CHAs found\n");
					for (tile=0; tile<NUM_CHA_USED; tile++) {
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,(cha_counts[socket_under_test][tile][1][1]-cha_counts[socket_under_test][tile][1][0]));
					}
#endif // VERBOSE
				}
				good = good_new * good_old;         // trigger a repeat if either the old or new tests failed
				}
				while (good == 0);
#if 0
				// 6. save the cache line number in the appropriate the cbo_indices[cbo][#lines] array
				// 7. increment the corresponding cbo_num_lines[cbo] array entry
				this_cbo = cha_by_page[page_number][line_number];
				if (this_cbo == -1) {
					printf("ERROR: cha_by_page[%ld][%ld] has not been set!\n",page_number,line_number);
					exit(80);
				}
				cbo_indices[this_cbo][cbo_num_lines[this_cbo]] = line_number;
				cbo_num_lines[this_cbo]++;
#endif // 0
			}
			// I have not overwritten the filename, but I will rebuild it here just in case I add something stupid in between....
			sprintf(filename,"PADDR_0x%.12lx.map",paddr_by_page[page_number]);
			ptr_mapping_file = fopen(filename,"w");
			if (!ptr_mapping_file) {
				printf("ERROR: Failed to open Mapping File %s for writing -- aborting\n",filename);
				exit(4);
			}
			// first try -- write one record of 32768 bytes
			rc64 = fwrite(&cha_by_page[page_number][0],(size_t) 32768, (size_t) 1, ptr_mapping_file);
			if (rc64 != 1) {
				printf("ERROR: failed to write one 32768 Byte record to  %s -- return code %ld\n",filename,rc64);
				exit(5);
			} else {
				printf("SUCCESS: wrote mapping file %d %s\n",new_pages_mapped,filename);
			}
        new_pages_mapped += 1;
        if (new_pages_mapped >= PAGES_MAPPED) break;
		}
	}
    printf("INFO: %d new 2MiB pages have been mapped\n",new_pages_mapped);
	printf("DUMMY: globalsum %d\n",globalsum);
	printf("VERBOSE: L3 Mapping Complete in %ld tries for %d cache lines ratio %f\n",totaltries,32768*PAGES_MAPPED,(double)totaltries/(double)(32768*PAGES_MAPPED));

#ifndef MYHUGEPAGE_1GB
	// now that the mapping is complete, I can add up the number of lines mapped to each CHA
	// be careful to count only the lines that are used, not the full 24MiB
	// 3 million elements is ~11.44 2MiB pages, so count all lines in each of the first 11 pages
	// If I did the arithmetic correctly, the 3 million elements uses 931328 Bytes of the 12th 2MiB page
	// which is 116416 elements or 14552 cache lines.

	// first accumulate the first 11 full pages
	for (page_number=0; page_number<11; page_number++) {
		for (line_number=0; line_number<32768; line_number++) {
			lines_by_cha[cha_by_page[page_number][line_number]]++;
		}
	}
	// then accumulate the partial 12th page
	for (line_number=0; line_number<14552; line_number++) {
		lines_by_cha[cha_by_page[11][line_number]]++;
	}
	// output
	long lines_accounted = 0;
	printf("LINES_BY_CHA");
	for (i=0; i<NUM_CHA_USED; i++) {
		printf(" %ld",lines_by_cha[i]);
		lines_accounted += lines_by_cha[i];
	}
	printf("\n");
	printf("ACCCOUNTED FOR %ld lines expected %ld lines\n",lines_accounted,l2_contained_size/8);
#endif // MYHUGEPAGE_1GB
// ============== END L3 MAPPING TESTS ==============================
#endif // MAP_L3


    exit(0);
}
