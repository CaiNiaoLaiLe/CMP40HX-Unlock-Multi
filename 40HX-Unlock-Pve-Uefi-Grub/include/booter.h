/* booter.h - DIRECT_SEC2 booter port, public helper API.
 *
 * Tranche 1 exposes only the pure Falcon primitives shared by the two
 * loaders (booter_load_v67 / booter_load_tu102_direct) and fwsec_boot_gsp.
 * They take the host gpu_dev so they can run in the main EFI image (where the
 * embedded blobs live) and use only the existing callbacks: g->mmio (RD/WR),
 * g->log, g->delay_ms. Nothing here touches hardware until a loader calls it.
 */
#ifndef CMP40HX_BOOTER_H
#define CMP40HX_BOOTER_H

#include "uefi_min.h"

struct gpu_dev;

/* v70 falcon_wait_reset_ready: poll HWCFG2[31] RESET_READY (bounded). */
void booter_falcon_wait_reset_ready(struct gpu_dev *g, UINT32 hwcfg2Reg);

/* v70 falcon_wait_scrub_done: DMACTL[2:1]==0 && HWCFG2[12]==0.
 * Returns 0 on done, -1 on timeout (tag is only for the log line). */
int booter_falcon_wait_scrub_done(struct gpu_dev *g, UINT32 dmactlReg,
                                  UINT32 hwcfg2Reg, const char *tag);

/* v70 falcon_dma_transfer: 256B-block DMA through the SEC2 DMATRF regs. */
void booter_falcon_dma_transfer(struct gpu_dev *g, UINT32 dest, UINT32 memOff,
                                UINT64 srcPhys, UINT32 size, UINT32 dmaCmd);

/* v70 u40x_imem_write: IMEM port load, AINCW + IMEMT tag every 64 words.
 * sec != 0 selects the SECURE (bit28) window, matching SEC=1 DMA. */
void booter_u40x_imem_write(struct gpu_dev *g, UINT32 dst, UINT32 sec,
                            const UINT32 *src, UINT32 sizeBytes,
                            UINT32 virtAddr);

/* v70 u40x_dmem_write: DMEM port load, AINCW sequential. */
void booter_u40x_dmem_write(struct gpu_dev *g, UINT32 dst, const UINT32 *src,
                            UINT32 sizeBytes);

/* v70 gsp_engine_reset (L1841): kill the GFW instance booted by VBIOS at POST
 * so SEC2 stops answering 0xBADF5620 (priv lockdown). Flow step [8]. */
void booter_gsp_engine_reset(struct gpu_dev *g);

/* v70 gsp_dma_transfer (L1886): 256B block DMA through the GSP DMATRF regs
 * (used by fwsec_boot_gsp_40hx). Same shape as the SEC2 transfer. */
void booter_gsp_dma_transfer(struct gpu_dev *g, UINT32 dest, UINT32 memOff,
                             UINT64 srcPhys, UINT32 size, UINT32 dmaCmd);

/* v70 booter_load_tu102_direct (L2980): the PRIMARY DIRECT_SEC2 loader.
 * Port-loads IMEM (NS+SEC) and DMEM from ucodePhys (image already copied and
 * the 16-byte SIG_PROD patched at image+0x8700 by the caller), sets BOOTVEC=0,
 * mailbox = wprMetaPhys (64-bit direct, no low copy), STARTCPU, then holds 3 s
 * WITHOUT register reads (v57 lesson) before a few sparse reads. Returns 0 on
 * booter OK (mbox0==0), -1 otherwise. Diagnostic STAGE/SELECTOR branches are
 * dropped; the v70 breadcrumb logs are kept for trace-based debugging. */
int booter_load_tu102_direct(struct gpu_dev *g, UINT64 wprMetaPhys,
                             UINT64 ucodePhys);

/* v70 fwsec_boot_gsp_40hx (L2186): the WITH_LOADER HS-FWSEC boot on the GSP
 * falcon that latches WPR2 (the privileged aperture the SEC2 booter needs).
 * Flow step [8b]: kflcnReset(GSP) -> patch DMEM data iface (mapper.init_cmd =
 * FRTS 0x15, readVbiosDesc + frtsRegionDesc) -> FBIF/TRANSCFG=0x5 -> build the
 * RM_FLCN_BL_DMEM_DESC into DMEM 0x0 and port-load the generic BL (768B) to
 * the top of GSP IMEM (BOOTVEC = 0xfd00) -> STARTCPU -> poll WPR2 up ~5s. The
 * FWSEC image must already be copied to fwsecPhys (host phys, identity
 * mapped) by the caller. Diagnostic STAGE/SB branches dropped. Returns 0 when
 * WPR2 latches, -1 otherwise (GSP locked 0xBADF, or poll timeout). */
int booter_fwsec_boot_gsp_40hx(struct gpu_dev *g, UINT64 fwsecPhys);

/* booter_alloc_high - >4GiB identity-mapped reserved allocator, a port of the
 * v70 helper alloc_fwsec_buffer (L1245): BS->AllocatePages(AllocateAddress,
 * EfiReservedMemoryType) walking 16MiB-aligned slots up from 0x110000000, with
 * static fallback candidates then a below-4G retry. It lives in main.c because
 * only there is g_boot_services in scope; booter.c calls it as a plain global
 * (same PE image). Returns the physical address, which equals the CPU virtual
 * address because UEFI identity-maps all DRAM, or 0 when every slot failed.
 * The TU102 booter can only read meta/V67 from >4G (v70 L8531-8537), so the
 * buffers must come from here. */
UINT64 booter_alloc_high(UINTN pages);

/* v70 flow steps [6]-[7]: allocate and fill the host-side buffers the two
 * loaders consume, then build the 256-byte GspFwWprMeta. On success returns 0
 * and writes the three physical addresses the loaders need:
 *   wprMetaPhys - GspFwWprMeta block (tu102 mailbox + fwsec BL geometry)
 *   ucodePhys   - booter_ucode_prod, with the 16-byte SIG_PROD patched at +0x8700
 *   fwsecPhys   - fwsec_40hx_prod_bin (FW40_BLOB_SIZE) for the HS-FWSEC boot
 * Internally also allocates v67 / gsp_rm_boot / dummy-fw / radix buffers that
 * the meta references, but only these three are returned. No MMIO is touched:
 * this is pure memory preparation, safe to run before any engine reset. */
int booter_prepare(struct gpu_dev *g, UINT64 *wprMetaPhys,
                      UINT64 *ucodePhys, UINT64 *fwsecPhys);

/* booter_run - the single wired entry for the DIRECT_SEC2 compute
 * unlock (mode 7). It reproduces the v70 production efi_main flow in order:
 *   [6][7] booter_prepare  - allocate/fill the host buffers + WPR meta
 *   [8]    booter_gsp_engine_reset - kill the POST/VBIOS GFW
 *   [8b]   booter_fwsec_boot_gsp_40hx - latch WPR2 (non-fatal, observed)
 *   [9]    booter_load_tu102_direct   - port-load the booter
 *   [10]   read PLM/SS0/SS1 + WPR2 and log the verdict
 * The reset is issued after prepare (which touches no MMIO) and immediately
 * before the fwsec boot, matching the v70 ordering. Every risky MMIO read is
 * behind a breadcrumb so a hard hang on the blind PVE box is attributable in
 * BOOTER_TRACE.TXT. Returns 0 only when SS0==VAL_SS0_UNLOCKED and
 * SS1==VAL_SS1_UNLOCKED; -1 on any prepare/loader failure or gated verdict. */
int booter_run(struct gpu_dev *g);

#endif /* CMP40HX_BOOTER_H */
