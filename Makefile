# Initial testing using the Intel icc compiler -- probably not necessary

CC=icc
CFLAGS=-sox -g -O0
CDEFINES=-DMAP_L3 -DMYHUGEPAGE_THP -DCHA_COUNTS

HELPERS=cpuid_check_inline.c low_overhead_timers.c program_CHA_counters.c read_CHA_counter.c

default: Map_Addresses_to_L3_Slices.c va2pa_lib.c $(HELPERS)
	$(CC) $(CFLAGS) $(CDEFINES) Map_Addresses_to_L3_Slices.c va2pa_lib.c -o Map_Addresses_to_L3_Slices.exe
