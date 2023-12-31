# Summary address hash information from a variety of Intel processors.
- John D. McCalpin
- mccalpin@tacc.utexas.edu
- Created 2021-09-09, Revised to 2023-07-14

## Context 
Nothing in this directory will make sense unless you are familiar with the material presented in the technical report [Mapping Addresses to L3/CHA Slices in Intel Processors](https://dx.doi.org/10.26153/tsw/14539) (more info in "Reference and Citation" section at the bottom of this page).

## File format information for Intel_Address_Hash/Results/ files

### Base Sequence files
Each file named "BaseSequence_\<proc\>_\<nn\>-slice.tbl" contains the "base sequence" of
L3/CHA numbers (as reported by the hardware performance counter in the processor
"uncore") for one of the processor configurations tested.  The "base sequence"
in each case is the set of L3/CHA numbers for cache line addresses starting at
physical address zero in the processor's memory space.

- "\<proc\>" values are
-- "KNL"	Xeon Phi x200 Processor "Knights Landing"
-- "SKX"	Xeon Scalable Processor (1st or 2nd gen) "Skylake Xeon" or "Cascade Lake Xeon"
-- "ICX"	Xeon Scalable Processor (3rd gen) "Ice Lake Xeon"
-- "SPR"	Xeon Scalable Processor (4th gen) "Sapphire Rapids Xeon" or Xeon CPU Max Processor ("Sapphire Rapids with HBM).
	
- "\<nn\>" values are the number of L3/CHA "slices" for the processor (or the number of CHA slices for the KNL processor, which has no L3)
The number of L3/CHA slices is always at least as large as the number of cores.

Each of the "BaseSequence" files is a text file containing one decimal number per line
and one line for each element of the base sequence.  The length of the base sequence
varies from 16 to 16384 lines for these processors.

**FileNames and Lengths**
- BaseSequence_ICX_28-slice.tbl 16384
- BaseSequence_ICX_40-slice.tbl 512
- BaseSequence_KNL_38-slice.tbl 4096
- BaseSequence_SKX_14-slice.tbl 16384
- BaseSequence_SKX_16-slice.tbl 16
- BaseSequence_SKX_18-slice.tbl 4096
- BaseSequence_SKX_20-slice.tbl 256
- BaseSequence_SKX_22-slice.tbl 16384
- BaseSequence_SKX_24-slice.tbl 512
- BaseSequence_SKX_26-slice.tbl 16384
- BaseSequence_SKX_28-slice.tbl 4096
- BaseSequence_SPR_56-slice.tbl 16384
- BaseSequence_SPR_60-slice.tbl 16384

### Permutation Select Mask files
Each file named "PermSelectMasks_\<proc\>_\<nn\>-slice.txt" contains two lines related to the "permutation selector masks".
- The first line contains two decimal numbers, the lowest address bit to which the permutation selector masks apply, and the highest address bit to which the permutation selector masks apply.
  - The lowest address bit is the first physical address bit above the top of the base sequence and should be 6+log2(BaseSequenceLength).
  - The highest address bit indicates the address range over which the mask setprovides correct answers.  E.g., the value "37" indicates that the masks are valid for all physical addresses for which the highest bit set is 37 or lower -- addresses below 2^38 = 256 GiB.
- The second line contains 14 hexadecimal numbers.
  - These are the permutation selector masks for permutation bit 0 (on the left) to permutation bit 13 (on the right).
  - All of the files contain 14 mask values, even if fewer are used.  (A permutation selector mask of zero corresponds to the identity permutation, so results are identical if permutation selector masks of 0x0 are used or ignored.)

## Reference and Citation:

John D. McCalpin, "Mapping Addresses to L3/CHA Slices in Intel Processors", Texas Advanced
Computing Center, University of Texas at Austin, Austin, TX, USA, ACELab TR-2021-03,
September 10, 2021, doi: https://dx.doi.org/10.26153/tsw/14539

(Note that this doi contains links to both the technical report describing the methodology and results and to the data files
that were completed as of the original publication date of 2021-09-10.)
