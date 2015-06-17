#ifndef LIBXL_SR_STREAM_FORMAT_H
#define LIBXL_SR_STREAM_FORMAT_H

/*
 * C structures for the Migration v2 stream format.
 * See docs/specs/libxl-migration-stream.pandoc
 */

#include <stdint.h>

typedef struct libxl_sr_hdr
{
    uint64_t ident;
    uint32_t version;
    uint32_t options;
} libxl_sr_hdr;

#define RESTORE_STREAM_IDENT         0x4c6962786c466d74UL
#define RESTORE_STREAM_VERSION       0x00000002U

#define RESTORE_OPT_BIG_ENDIAN       (1 << 0)
#define RESTORE_OPT_LEGACY           (1 << 1)


typedef struct libxl_sr_rec_hdr
{
    uint32_t type;
    uint32_t length;
} libxl_sr_rec_hdr;

/* All records must be aligned up to an 8 octet boundary */
#define REC_ALIGN_ORDER              3U

#define REC_TYPE_END                 0x00000000U
#define REC_TYPE_LIBXC_CONTEXT       0x00000001U
#define REC_TYPE_XENSTORE_DATA       0x00000002U
#define REC_TYPE_EMULATOR_CONTEXT    0x00000003U
#define REC_TYPE_CHECKPOINT_END      0x00000004U
#define REC_TYPE_COLO_CONTEXT        0x00000005U

typedef struct libxl_sr_emulator_hdr
{
    uint32_t id;
    uint32_t index;
} libxl_sr_emulator_hdr;

#define EMULATOR_UNKNOWN             0x00000000U
#define EMULATOR_QEMU_TRADITIONAL    0x00000001U
#define EMULATOR_QEMU_UPSTREAM       0x00000002U

typedef struct libxl_sr_colo_context
{
    uint32_t id;
} libxl_sr_colo_context;

#define COLO_NEW_CHECKPOINT          0x00000000U
#define COLO_SVM_SUSPENDED           0x00000001U
#define COLO_SVM_READY               0x00000002U
#define COLO_SVM_RESUMED             0x00000003U

#endif /* LIBXL_SR_STREAM_FORMAT_H */

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
