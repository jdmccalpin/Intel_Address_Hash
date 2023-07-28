
#define CPUID_SIGNATURE_HASWELL 0x000306f0U
#define CPUID_SIGNATURE_SKX 0x00050650U
#define CPUID_SIGNATURE_ICX 0x000606a0U
#define CPUID_SIGNATURE_SPR 0x000806f0U

uint32_t cpuid_signature() {
    int cpuid_return[4];

    __cpuid(&cpuid_return[0], 1);

    uint32_t ModelInfo = cpuid_return[0] & 0x0fff0ff0;  // mask out the reserved and "stepping" fields, leaving only the base and extended Family/Model fields

#ifdef DEBUG
    if (ModelInfo == CPUID_SIGNATURE_HASWELL) {                   // expected values for Haswell EP
        printf("Haswell EP\n");
    }
    else if (ModelInfo == CPUID_SIGNATURE_SKX) {              // expected values for SKX/CLX
        printf("SKX/CLX\n");
    }
    else if (ModelInfo == CPUID_SIGNATURE_ICX) {              // expected values for Ice Lake Xeon
        printf("ICX\n");
    }
    else if (ModelInfo == CPUID_SIGNATURE_SPR) {              // expected values for Sapphire Rapids Xeon
        printf("SPR\n");
    } else {
        printf("Unknown processor 0x%x\n",ModelInfo);
    }
#endif

    return ModelInfo;
}
