
// read_CHA_counter() encapsulates the MSR addressing patterns for various Intel processors and
// reads the specified performance counter number in the specified CHA of the specified socket.

uint64_t read_CHA_counter(uint32_t CurrentCPUIDSignature, int socket, int cha_number, int counter, int *msr_fd)
{
    uint64_t msr_val, msr_num, msr_base;
    uint64_t msr_stride;

    msr_val = 0;

    switch(CurrentCPUIDSignature) {
        case CPUID_SIGNATURE_HASWELL:
        // ------------ Haswell EP -- Xeon E5-2xxx v3 --------------
            // printf("CPUID Signature 0x%x identified as Haswell EP\n",CurrentCPUIDSignature);
            break;
        // ------------ Skylake Xeon and Cascade Lake Xeon -- 1st and 2nd generation Xeon Scalable Processors ------------
        case CPUID_SIGNATURE_SKX:
            // printf("CPUID Signature 0x%x identified as Skylake Xeon/Cascade Lake Xeon\n",CurrentCPUIDSignature);
            msr_base = 0xe00;
            msr_stride = 0x10;              // specific to SKX/CLX
            msr_num = msr_base + msr_stride*cha_number + counter + 8;     // compute MSR number for count register for counter
            // printf("DEBUG: socket %d cha_number %d counter %d msr_num 0x%lx msr_val 0x%lx\n",socket,cha_number,counter,msr_num,msr_val);
            pread(msr_fd[socket],&msr_val,sizeof(msr_val),msr_num);
            return (msr_val);
            break;
        // ------------- Ice Lake Xeon -- 3rd generation Xeon Scalable Processors ------------
        case CPUID_SIGNATURE_ICX:
            // printf("CPUID Signature 0x%x identified as Ice Lake Xeon\n",CurrentCPUIDSignature);
            msr_stride = 0x0e;              // ICX-specific

            if (cha_number >= 34) {
                msr_base = 0x0e00 - 0x47c;       // ICX MSRs skip backwards for CHAs 34-39
            } else if (cha_number >= 18) {
                msr_base = 0x0e00 + 0x0e;        // ICX MSRs skip forward for CHAs 18-33
            } else {
                msr_base = 0x0e00;               // MSRs for the first 18 CHAs
            }
            msr_num = msr_base + msr_stride*cha_number + counter + 8;     // compute MSR number for count register for counter 
            pread(msr_fd[socket],&msr_val,sizeof(msr_val),msr_num);
            return(msr_val); 
            break;
        // ------------------ Sapphire Rapids -- 4th generation Xeon Scalable Processors and Xeon CPU Max Processors ------------
        case CPUID_SIGNATURE_SPR:
            // printf("CPUID Signature 0x%x identified as Sapphire Rapids Xeon\n",CurrentCPUIDSignature);
            msr_base = 0x2000;
            msr_stride = 0x10;
            msr_num = msr_base + msr_stride*cha_number + 0x8 + counter;
            pread(msr_fd[socket],&msr_val,sizeof(msr_val),msr_num);
            return(msr_val); 
            break;
        default:
            fprintf(stderr,"CHA counters not yet supported for CPUID Signature 0x%x\n",CurrentCPUIDSignature);
            exit(1);
    }
}
