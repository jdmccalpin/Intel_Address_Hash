
// program_CHA_counters() encapsulates the MSR addressing patterns for various Intel 
// processors and programs the counters in each CHA box with the provided PerfEvtSel values.

int program_CHA_counters(uint32_t CurrentCPUIDSignature, int num_chas, uint64_t *cha_perfevtsel, int num_counters, int *msr_fd, int num_sockets)
{
    int pkg,tile,counter;
    uint64_t msr_val, msr_num, msr_base;
    uint64_t msr_stride;

    switch(CurrentCPUIDSignature) {
        case CPUID_SIGNATURE_HASWELL:
        // ------------ Haswell EP -- Xeon E5-2xxx v3 --------------
            printf("CPUID Signature 0x%x identified as Haswell EP\n",CurrentCPUIDSignature);
            break;
        // ------------ Skylake Xeon and Cascade Lake Xeon -- 1st and 2nd generation Xeon Scalable Processors ------------
        case CPUID_SIGNATURE_SKX:
            printf("CPUID Signature 0x%x identified as Skylake Xeon/Cascade Lake Xeon\n",CurrentCPUIDSignature);
            msr_base = 0xe00;
            msr_stride = 0x10;              // specific to SKX/CLX
            for (pkg=0; pkg<num_sockets; pkg++) {
                for (tile=0; tile<num_chas; tile++) {
                    for (counter=0; counter<num_counters; counter++) {
                        msr_num = msr_base + msr_stride*tile + counter + 1;     // ctl register for counter
                        msr_val = cha_perfevtsel[counter];
                        // printf("DEBUG: pkg %d tile %d counter %d msr_num 0x%lx msr_val 0x%lx\n",pkg,tile,counter,msr_num,msr_val);
                        pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
                    }
                    // The CHA performance counters on SKX/CLX have two filter registers that are required for some events.
                    // PMON_BOX_FILTER0 (offset 0x5) controls LLC_LOOKUP state (bits 26:17) and optional TID (bits 8:0)
                    msr_num = msr_base + msr_stride*tile + 5;    // filter0
                    msr_val = 0x01e20000;              // bits 24:21,17 FMESI -- all LLC lookups, not not SF lookups
                    // printf("DEBUG: pkg %d tile %d counter %d msr_num 0x%lx msr_val 0x%lx\n",pkg,tile,counter,msr_num,msr_val);
                    pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
                    // PMON_BOX_FILTER1 (offset 0x6) allows opcode filtering and local/remote filtering.
                    //    FILTER1 should be set to 0x03B for no filtering (all memory, all local/remote, all opcodes)
                    msr_num = msr_base + msr_stride*tile + 6;    // filter1
                    msr_val = 0x03b;                    // near&non-near memory, local&remote, all opcodes
                    // printf("DEBUG: pkg %d tile %d counter %d msr_num 0x%lx msr_val 0x%lx\n",pkg,tile,counter,msr_num,msr_val);
                    pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
                }
            }
            break;
        // ------------- Ice Lake Xeon -- 3rd generation Xeon Scalable Processors ------------
        case CPUID_SIGNATURE_ICX:
            printf("CPUID Signature 0x%x identified as Ice Lake Xeon\n",CurrentCPUIDSignature);
            msr_stride = 0x0e;              // ICX-specific
            for (pkg=0; pkg<num_sockets; pkg++) {
                for (tile=0; tile<num_chas; tile++) {
                    if (tile >= 34) {
                        msr_base = 0x0e00 - 0x47c;       // ICX MSRs skip backwards for CHAs 34-39
                    } else if (tile >= 18) {
                        msr_base = 0x0e00 + 0x0e;        // ICX MSRs skip forward for CHAs 18-33
                    } else {
                        msr_base = 0x0e00;               // MSRs for the first 18 CHAs
                    }

                    // unit control register -- optional write bit 1 (value 0x2) to clear counters
                    msr_num = msr_base + msr_stride*tile;
                    msr_val = 0x2;
                    pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);

                    // program the control registers for counters 0..num_counters-1
                    for (counter=0; counter<num_counters; counter++) {
                        msr_num = msr_base + msr_stride*tile + counter + 1;     // ctl register for counter
                        msr_val = cha_perfevtsel[counter];
                        pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
                    }
                }
            }
            return(0); // no error checking yet -- if it dies, it dies....
            break;
        // ------------------ Sapphire Rapids -- 4th generation Xeon Scalable Processors and Xeon CPU Max Processors ------------
        case CPUID_SIGNATURE_SPR:
            printf("CPUID Signature 0x%x identified as Sapphire Rapids Xeon\n",CurrentCPUIDSignature);
            msr_base = 0x2000;
            msr_stride = 0x10;
            for (pkg=0; pkg<num_sockets; pkg++) {
                for (tile=0; tile<num_chas; tile++) {
                    for (counter=0; counter<num_counters; counter++) {
                        msr_num = msr_base + msr_stride*tile + counter + 2;     // compute MSR number of control register for counter
                        msr_val = cha_perfevtsel[counter];
                        // printf("DEBUG: pkg %d tile %d counter %d msr_num 0x%lx msr_val 0x%lx\n",pkg,tile,counter,msr_num,msr_val);
                        pwrite(msr_fd[pkg],&msr_val,sizeof(msr_val),msr_num);
                    }
                }
            }
            return(0); // no error checking yet -- if it dies, it dies....
            break;
        default:
            fprintf(stderr,"CHA counters not yet supported for CPUID Signature 0x%x\n",CurrentCPUIDSignature);
            exit(1);
    }
}
