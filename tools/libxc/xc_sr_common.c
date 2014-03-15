#include "xc_sr_common.h"

static const char *dhdr_types[] =
{
    [DHDR_TYPE_X86_PV]  = "x86 PV",
    [DHDR_TYPE_X86_HVM] = "x86 HVM",
    [DHDR_TYPE_X86_PVH] = "x86 PVH",
    [DHDR_TYPE_ARM]     = "ARM",
};

const char *dhdr_type_to_str(uint32_t type)
{
    if ( type < ARRAY_SIZE(dhdr_types) && dhdr_types[type] )
        return dhdr_types[type];

    return "Reserved";
}

static const char *mandatory_rec_types[] =
{
    [REC_TYPE_END]                  = "End",
    [REC_TYPE_PAGE_DATA]            = "Page data",
    [REC_TYPE_X86_PV_INFO]          = "x86 PV info",
    [REC_TYPE_X86_PV_P2M_FRAMES]    = "x86 PV P2M frames",
    [REC_TYPE_X86_PV_VCPU_BASIC]    = "x86 PV vcpu basic",
    [REC_TYPE_X86_PV_VCPU_EXTENDED] = "x86 PV vcpu extended",
    [REC_TYPE_X86_PV_VCPU_XSAVE]    = "x86 PV vcpu xsave",
    [REC_TYPE_SHARED_INFO]          = "Shared info",
    [REC_TYPE_TSC_INFO]             = "TSC info",
    [REC_TYPE_HVM_CONTEXT]          = "HVM context",
    [REC_TYPE_HVM_PARAMS]           = "HVM params",
    [REC_TYPE_TOOLSTACK]            = "Toolstack",
    [REC_TYPE_X86_PV_VCPU_MSRS]     = "x86 PV vcpu msrs",
    [REC_TYPE_VERIFY]               = "Verify",
};

const char *rec_type_to_str(uint32_t type)
{
    if ( !(type & REC_TYPE_OPTIONAL) )
    {
        if ( (type < ARRAY_SIZE(mandatory_rec_types)) &&
             (mandatory_rec_types[type]) )
            return mandatory_rec_types[type];
    }

    return "Reserved";
}

static void __attribute__((unused)) build_assertions(void)
{
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_ihdr) != 24);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_dhdr) != 16);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rhdr) != 8);

    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_page_data_header)  != 8);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_x86_pv_info)       != 8);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_x86_pv_p2m_frames) != 8);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_x86_pv_vcpu_hdr)   != 8);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_tsc_info)          != 24);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_hvm_params_entry)  != 16);
    XC_BUILD_BUG_ON(sizeof(struct xc_sr_rec_hvm_params)        != 8);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */