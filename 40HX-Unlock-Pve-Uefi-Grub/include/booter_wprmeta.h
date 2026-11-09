/* booter_wprmeta.h - GspFwWprMeta layout + geometry constants for the
 * DIRECT_SEC2 WPR-metadata builder (port of unlock40x_v70.c L659-722).
 *
 * Every field and value below was copied VERBATIM from the on-disk v70
 * reference; do not "tidy" the struct. The booter executed by SEC2/GSP parses
 * this block by byte offset, so a wrong field order silently boots garbage.
 *
 * The whole block lives in a >4GiB, identity-mapped reserved buffer whose
 * physical address is handed to the booter through SEC2/GSP mailbox0/1 as a
 * 64-bit LO32/HI32 pair (v70 L8531-8537: reading meta/V67 from <4GiB is
 * blocked on TU102 with exit 0x91, so the buffers MUST be allocated high).
 */
#ifndef CMP40HX_BOOTER_WPRMETA_H
#define CMP40HX_BOOTER_WPRMETA_H

#include "uefi_min.h"

/* ---- Buffer sizes / signatures (v70 L642-722) ---- */
#define V67_SIZE                0x0000FA00UL   /* L654  payload bytes actually used */
#define FW40_BLOB_SIZE          0x00009DF0UL   /* L801  40HX FWSEC code+data        */
#define WPR_META_SIZE           256u           /* L659  == sizeof(GspFwWprMeta)     */
#define GSP_FW_WPR_META_MAGIC   0xdc3aae21371a60b3ULL   /* L660 */
#define GSP_FW_WPR_META_REVISION 1ULL                    /* L661 */

/* Production frame-buffer size used by the second (real) efi_main: 8 GiB.
 * build_wpr_meta derives the entire WPR/heap layout from this value. */
#define BOOTER_FB_SIZE          0x200000000ULL

/* Real GSP-RM .fwimage size (log53 / gsp_tu10x.bin). The dummy firmware is a
 * zero image of exactly this length; gspFwOffset = bootBinOffset - elfSize so
 * this size fixes the whole WPR layout (v70 L7419). */
#define DUMMY_FW_SIZE           0x01bfadc8ULL

/* radix3 page-table geometry (v70 L719-722). */
#define RADIX_PAGE_LOG2         12
#define RADIX_PAGE_SIZE         (1ULL << RADIX_PAGE_LOG2)
#define RADIX_ENTRIES_LOG2      (RADIX_PAGE_LOG2 - 3)
#define RADIX_ENTRIES           (1ULL << RADIX_ENTRIES_LOG2)  /* 512 */

/* flags bit0 = GSP_FW_FLAGS_CLOCK_BOOST (v70 L4947). */
#define GSP_FW_FLAGS_CLOCK_BOOST 0x1u

/* High-memory allocation cursor base (v70 cmp40_next_high_slot init 0x110000000). */
#define BOOTER_HIGH_SLOT_BASE   0x110000000ULL

/* Field-for-field replica of v70 GspFwWprMeta (L664-716). */
typedef struct {
    UINT64 magic;
    UINT64 revision;
    UINT64 sysmemAddrOfRadix3Elf;
    UINT64 sizeOfRadix3Elf;
    UINT64 sysmemAddrOfBootloader;
    UINT64 sizeOfBootloader;
    UINT64 bootloaderCodeOffset;
    UINT64 bootloaderDataOffset;
    UINT64 bootloaderManifestOffset;
    union {
        struct { UINT64 sysmemAddrOfSignature; UINT64 sizeOfSignature; };
        struct { UINT32 gspFwHeapFreeListWprOffset; UINT32 unused0; UINT64 unused1; };
    };
    UINT64 gspFwRsvdStart;
    UINT64 nonWprHeapOffset;
    UINT64 nonWprHeapSize;
    UINT64 gspFwWprStart;
    UINT64 gspFwHeapOffset;
    UINT64 gspFwHeapSize;
    UINT64 gspFwOffset;
    UINT64 bootBinOffset;
    UINT64 frtsOffset;
    UINT64 frtsSize;
    UINT64 gspFwWprEnd;
    UINT64 fbSize;
    UINT64 vgaWorkspaceOffset;
    UINT64 vgaWorkspaceSize;
    UINT64 bootCount;
    union {
        struct {
            UINT64 partitionRpcAddr;
            UINT16 partitionRpcRequestOffset;
            UINT16 partitionRpcReplyOffset;
            UINT32 elfCodeOffset;
            UINT32 elfDataOffset;
            UINT32 elfCodeSize;
            UINT32 elfDataSize;
            UINT32 lsUcodeVersion;
        };
        struct {
            UINT32 partitionRpcPadding[4];
            UINT64 sysmemAddrOfCrashReportQueue;
            UINT32 sizeOfCrashReportQueue;
            UINT32 lsUcodeVersionPadding;
        };
    };
    UINT8  gspFwHeapVfPartitionCount;
    UINT8  flags;
    UINT8  padding[2];
    UINT32 pmuReservedSize;
    UINT64 verified;
} GspFwWprMeta;

#endif /* CMP40HX_BOOTER_WPRMETA_H */
