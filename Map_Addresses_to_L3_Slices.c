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

#define MYPAGESIZE 2097152L
#define NUMPAGES 2048L			// 40960L (80 GiB) for big production runs
#define PAGES_MAPPED 16L		// 128L or 256L for production runs


// interfaces for va2pa_lib.c
void print_pagemap_entry(unsigned long long pagemap_entry);
unsigned long long get_pagemap_entry( void * va );

double *array;					// array pointer to mmap on 1GiB pages
double *page_pointers[NUMPAGES];		// one pointer for each page allocated
uint64_t pageframenumber[NUMPAGES];	// one PFN entry for each page allocated

// constant value defines for pre-allocated arrays
# define NUM_SOCKETS 2
# define NUM_CHA_BOXES 60               // largest number of CHAs per socket in current product line (2023-07-30)
# define NUM_CHA_COUNTERS 4

long cha_counts[NUM_SOCKETS][NUM_CHA_BOXES][NUM_CHA_COUNTERS][2];		// 2 sockets, 28 tiles per socket, 4 counters per tile, 2 times (before and after)
uint64_t cha_perfevtsel[NUM_CHA_COUNTERS];
long cha_pkg_sums[NUM_SOCKETS][NUM_CHA_COUNTERS];

int8_t cha_by_page[NUMPAGES][32768];				// L3 numbers for each of the 32,768 cache lines in each of the first PAGES_MAPPED 2MiB pages
uint64_t paddr_by_page[NUMPAGES];					// physical addresses of the base of each of the first PAGES_MAPPED 2MiB pages used
long lines_by_cha[NUM_CHA_BOXES];			// bulk count of lines assigned to each CHA

# ifndef MIN
# define MIN(x,y) ((x)<(y)?(x):(y))
# endif
# ifndef MAX
# define MAX(x,y) ((x)>(y)?(x):(y))
# endif

#include "MSR_defs.h"                   // includes MSR_Architectural.h and MSR_ArchPerfMon_v3.h -- very few of these defines are used here 
#include "low_overhead_timers.c"        // probably need to link this to my official github version
#include "cpuid_check_inline.c"         // CPUID "signatures" (CPUID/leaf 0x01, return value in eax with stepping masked out)
// #include "program_CHA_PMC_ICX.c"        // off-loading code with details of CHA PMON MSR indexing
#include "program_CHA_counters.c"       // program all CHA counters -- contains model-specific code
#include "read_CHA_counter.c"           // read one CHA counter from one CHA in one socket -- contains model-specific code

// ===========================================================================================================================================================================
int main(int argc, char *argv[])
{
	// local declarations
	// int cpuid_return[4];
	int i;
	int tag;
	int rc;
	ssize_t rc64;
	size_t len;
	unsigned long pagemapentry;
	unsigned long paddr, basephysaddr;
	uint32_t socket, counter;
	long count,delta;
	long j,k,page_number,page_base_index,line_number;
	uint32_t low_0, high_0, low_1, high_1;
	char filename[100];
	int pkg, tile;
	int nr_cpus;
    int CHA_per_socket;
	uint64_t msr_val, msr_num;
	int mem_fd;
	int msr_fd[2];				// one for each socket
	int proc_in_pkg[2];			// one Logical Processor number for each socket
	uid_t my_uid;
	gid_t my_gid;
	double sum;
	unsigned long tsc_start;

    uint32_t CurrentCPUIDSignature;     // CPUID Signature for the current system -- save for later processor-dependent conditionals

    // ===============================================================================================================================
	// allocate working array on a huge pages -- either 1GiB or 2MiB
	len = NUMPAGES * MYPAGESIZE;        // Bytes
	rc = posix_memalign((void **)&array, (size_t) 2097152, (size_t) len);
	if (rc != 0) {
		printf("ERROR: posix_memalign call failed with error code %d\n",rc);
		exit(3);
	}
	if (array == (void *)(-1)) {
        perror("ERROR: mmap of array a failed! ");
        exit(1);
    }
	// initialize working array
	for (j=0; j<len/sizeof(double); j++) {
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
	printf("PAGE_ADDRESSES\n");
	for (j=0; j<NUMPAGES; j++) {
		basephysaddr = pageframenumber[j] << 12;
		paddr_by_page[j] = basephysaddr;
		printf("0x%.12lx ",paddr_by_page[j]);
        if ((j+1)%8 == 0) printf("\n");
	}
	printf("\n");
	for (j=0; j<NUMPAGES; j++) {
		if ( (paddr_by_page[j] & 0x1fffffUL) != 0 ) {
            printf("WARNING: page %d basephysaddr %p is not 2MiB-aligned\n",j,paddr_by_page[j]);
        }
    }

    // ===============================================================================================================================
	// initialize arrays for CHA counter data (only partially used in this MAP_L3 version, but not big enough to be a problem)
	for (socket=0; socket<NUM_SOCKETS; socket++) {
		for (tile=0; tile<NUM_CHA_BOXES; tile++) {
			lines_by_cha[tile] = 0;
			for (counter=0; counter<NUM_CHA_COUNTERS; counter++) {
				cha_counts[socket][tile][counter][0] = 0;
				cha_counts[socket][tile][counter][1] = 0;
			}
		}
	}
	// initialize the array that will hold the L3 slice numbers for each cache line for each of the first NUMPAGES 2MiB pages
	for (i=0; i<NUMPAGES; i++) {
		for (line_number=0; line_number<32768; line_number++) {
			cha_by_page[i][line_number] = 0;
		}
	}


	//========================================================================================================================
	// Identify the processor by CPUID signature (CPUID leaf 0x01, return value in %eax)
	// If not supported, Don't abort yet but save the CurrentCPUIDSignature for later processor-dependent conditionals
    CurrentCPUIDSignature = cpuid_signature();

    switch(CurrentCPUIDSignature) {
        case CPUID_SIGNATURE_HASWELL:
            printf("CPUID Signature 0x%x identified as Haswell EP\n",CurrentCPUIDSignature);
            break;
        case CPUID_SIGNATURE_SKX:
            printf("CPUID Signature 0x%x identified as Skylake Xeon/Cascade Lake Xeon\n",CurrentCPUIDSignature);
            break;
        case CPUID_SIGNATURE_ICX:
            printf("CPUID Signature 0x%x identified as Ice Lake Xeon\n",CurrentCPUIDSignature);
            break;
        case CPUID_SIGNATURE_SPR:
            printf("CPUID Signature 0x%x identified as Sapphire Rapids Xeon\n",CurrentCPUIDSignature);
            break;
        default:
            printf("CPUID Signature 0x%x not a supported value\n",CurrentCPUIDSignature);
    }

	// ===================================================================================================================
	// open the MSR driver using one core in socket 0 and one core in socket 1
	nr_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    proc_in_pkg[0] = 0;                 // logical processor 0 is in socket 0 in all TACC systems
    proc_in_pkg[1] = nr_cpus-1;         // logical processor N-1 is in socket 1 in all TACC 2-socket systems
	for (pkg=0; pkg<NUM_SOCKETS; pkg++) {
		sprintf(filename,"/dev/cpu/%d/msr",proc_in_pkg[pkg]);
		msr_fd[pkg] = open(filename, O_RDWR);
		if (msr_fd[pkg] == -1) {
			fprintf(stderr,"ERROR %s when trying to open %s\n",strerror(errno),filename);
			exit(-1);
		}
	}
	for (pkg=0; pkg<NUM_SOCKETS; pkg++) {
		pread(msr_fd[pkg],&msr_val,sizeof(msr_val),IA32_TIME_STAMP_COUNTER);
		fprintf(stdout,"DEBUG: TSC on core %d socket %d is %ld\n",proc_in_pkg[pkg],pkg,msr_val);
	}

    int core_under_test, socket_under_test;
    tsc_start = full_rdtscp(&socket_under_test, &core_under_test);

#ifdef VERBOSE
	printf("VERBOSE: programming CHA counters\n");
#endif // VERBOSE

    // Model-specific CHA performance counter events
    switch(CurrentCPUIDSignature) {
        case CPUID_SIGNATURE_HASWELL:
            printf("CPUID Signature 0x%x identified as Haswell EP\n",CurrentCPUIDSignature);
            printf("--- not yet supported\n");
            exit(1);
            break;
        case CPUID_SIGNATURE_SKX:
            printf("CPUID Signature 0x%x identified as Skylake Xeon/Cascade Lake Xeon\n",CurrentCPUIDSignature);
            CHA_per_socket = 28;
            cha_perfevtsel[0] = 0x00400334;		// LLC_LOOKUP.DATA_READ -- requires CHA_FILTER0 bits 26:17
            cha_perfevtsel[1] = 0x00400334;		// LLC_LOOKUP.DATA_READ -- requires CHA_FILTER0 bits 26:17
            cha_perfevtsel[2] = 0x00400334;		// LLC_LOOKUP.DATA_READ -- requires CHA_FILTER0 bits 26:17
            cha_perfevtsel[3] = 0x00400334;		// LLC_LOOKUP.DATA_READ -- requires CHA_FILTER0 bits 26:17
            break;
        case CPUID_SIGNATURE_ICX:
            printf("CPUID Signature 0x%x identified as Ice Lake Xeon\n",CurrentCPUIDSignature);
            CHA_per_socket = 40;
            cha_perfevtsel[0] = 0x00400350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[1] = 0x00400350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[2] = 0x00400350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[3] = 0x00400350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            // program_CHA_PMC_ICX(NUM_CHA_USED, cha_perfevtsel, 4, msr_fd, NUM_SOCKETS);
            break;
        case CPUID_SIGNATURE_SPR:
            printf("CPUID Signature 0x%x identified as Sapphire Rapids Xeon\n",CurrentCPUIDSignature);
            CHA_per_socket = 60;
            // Note that SPR does not use the "enable" bit (bit 22), and reserves it -- do not write!
            cha_perfevtsel[0] = 0x00000350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[1] = 0x00000350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[2] = 0x00000350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            cha_perfevtsel[3] = 0x00000350;		// REQUESTS.READS -- local read requests that miss the SF & LLC and are sent to the HA
            break;
        default:
            printf("CPUID Signature 0x%x not a supported value\n",CurrentCPUIDSignature);
            exit(1);
    }
    program_CHA_counters(CurrentCPUIDSignature,CHA_per_socket, cha_perfevtsel, 4, msr_fd, NUM_SOCKETS);
    // document CHA counter programming in output
    for (counter=0; counter<NUM_CHA_COUNTERS; counter++) {
        printf("INFO: CHA_PERFEVTSEL[%d] = 0x%lx\n",counter,cha_perfevtsel[counter]);
    }

#ifdef VERBOSE
	printf("VERBOSE: finished programming CHA counters\n");
#endif // VERBOSE

    // Hit the global "unfreeze counter" function in the uncore global control on each chip
    // Enable the uncore fixed clock while I am at it....
	for (pkg=0; pkg<NUM_SOCKETS; pkg++) {
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
    long page_numbers_mapped[PAGES_MAPPED];
    for (i=0; i<PAGES_MAPPED; i++) page_numbers_mapped[i] = 0;

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
				for (tile=0; tile<CHA_per_socket; tile++) {
                    cha_counts[socket_under_test][tile][0][0] = read_CHA_counter(CurrentCPUIDSignature, socket_under_test, tile, 0, msr_fd);
                }

#if 0
				// 1. read L3 counters before starting test
				for (tile=0; tile<CHA_per_socket; tile++) {
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
#endif

				// 2. Access the line NFLUSHES times
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
				for (tile=0; tile<CHA_per_socket; tile++) {
                    cha_counts[socket_under_test][tile][0][1] = read_CHA_counter(CurrentCPUIDSignature, socket_under_test, tile, 0, msr_fd);
                }


#if 0
				// 3. read L3 counters after loads are done
				for (tile=0; tile<CHA_per_socket; tile++) {
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
#endif




#ifdef VERBOSE
				for (tile=0; tile<CHA_per_socket; tile++) {
					printf("DEBUG: page %ld line %ld cha_counter0_after %lu cha_counter0 before %lu delta %lu\n",
							page_number,line_number,cha_counts[socket_under_test][tile][0][1],cha_counts[socket_under_test][tile][0][0],cha_counts[socket_under_test][tile][0][1]-cha_counts[socket_under_test][tile][0][0]);
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
				for (tile=0; tile<CHA_per_socket; tile++) {
					delta = corrected_pmc_delta(cha_counts[socket_under_test][tile][0][1],cha_counts[socket_under_test][tile][0][0],48);
					max_count = MAX(max_count, delta);
					min_count = MIN(min_count, delta);
					sum_count += delta;
				}
				avg_count = (double)(sum_count - max_count) / (double)(CHA_per_socket);
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
				for (tile=0; tile<CHA_per_socket; tile++) {
					delta = corrected_pmc_delta(cha_counts[socket_under_test][tile][0][1],cha_counts[socket_under_test][tile][0][0],48);
					if (delta >= min_counts) {
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
					for (tile=0; tile<CHA_per_socket; tile++) {
                        delta = corrected_pmc_delta(cha_counts[socket_under_test][tile][0][1],cha_counts[socket_under_test][tile][0][0],48);
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,delta);
					}
#endif // VERBOSE
				} else if (found == 1) {
					good_old = 1;
				} else {
					good_old = 0;
#ifdef VERBOSE
					printf("DEBUG dump for multiple CHAs found\n");
					for (tile=0; tile<CHA_per_socket; tile++) {
                        delta = corrected_pmc_delta(cha_counts[socket_under_test][tile][0][1],cha_counts[socket_under_test][tile][0][0],48);
						printf("CHA %d LLC_LOOKUP.READ          delta %ld\n",tile,delta);
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
        page_numbers_mapped[new_pages_mapped] = page_number;
        new_pages_mapped += 1;
        if (new_pages_mapped >= PAGES_MAPPED) break;
		}
	}
    printf("INFO: %d new 2MiB pages have been mapped\n",new_pages_mapped);
	printf("DUMMY: globalsum %d\n",globalsum);
	printf("VERBOSE: L3 Mapping Complete in %ld tries for %d cache lines ratio %f\n",totaltries,32768*PAGES_MAPPED,(double)totaltries/(double)(32768*PAGES_MAPPED));

    // Accumulate the number of lines mapped to each CHA slice in each of the new pages mapped
	for (i=0; i<new_pages_mapped; i++) {
        page_number = page_numbers_mapped[i];
		for (line_number=0; line_number<32768; line_number++) {
			lines_by_cha[cha_by_page[page_number][line_number]]++;
		}
	}
    // Report the lines mapped to each CHA and the total number of lines mapped
	long lines_accounted = 0;
	printf("------------\n");
	printf("LINES_BY_CHA\n");
	for (i=0; i<CHA_per_socket; i++) {
		printf("%d %ld\n",i,lines_by_cha[i]);
		lines_accounted += lines_by_cha[i];
	}
	printf("ACCCOUNTED FOR %ld lines expected %ld lines\n",lines_accounted,32768*new_pages_mapped);
// ============== END L3 MAPPING TESTS ==============================
#endif // MAP_L3

    exit(0);
}
