/* booter_assets.h - externs for the firmware blobs objcopy-embedded into the
 * EFI image by the Makefile (see ASSET_* rules). Names match the
 * --redefine-sym targets in build_v70.sh so a line-for-line port of
 * unlock40x_v70.c compiles unchanged.
 *
 * The blobs are self-contained in this project's assets/ (copies of the
 * v70 .bin files from the 40HX-Unlock-Drive-Patch scheme; sha256 identical,
 * provenance only -- no build-time dependency on that directory).
 *
 * Length must be computed as (_end - _start): objcopy's generated *_size
 * symbol stores a big-endian byte count and is NOT a host integer.
 */
#ifndef CMP40HX_BOOTER_ASSETS_H
#define CMP40HX_BOOTER_ASSETS_H

#include "uefi_min.h"

/* SEC2 booter microcode image (TU102, production, sig 0x303c3b1f). */
extern const UINT8 booter_ucode_prod[];
extern const UINT8 booter_ucode_prod_end[];
#define BOOTER_UCODE_PROD_LEN ((UINTN)(booter_ucode_prod_end - booter_ucode_prod))

/* v67 privileged register-write payload (64000). */
extern const UINT8 v67_payload_bin[];
extern const UINT8 v67_payload_bin_end[];
#define V67_PAYLOAD_LEN ((UINTN)(v67_payload_bin_end - v67_payload_bin))

/* GSP firmware bootblock loader (bl_gsp_tu102, 768). */
extern const UINT8 gsp_bl_tu102[];
extern const UINT8 gsp_bl_tu102_end[];
#define GSP_BL_TU102_LEN ((UINTN)(gsp_bl_tu102_end - gsp_bl_tu102))

/* GSP RM boot (GspRmBoot) image. */
extern const UINT8 gsp_rm_boot_dbg[];
extern const UINT8 gsp_rm_boot_dbg_end[];
#define GSP_RM_BOOT_DBG_LEN ((UINTN)(gsp_rm_boot_dbg_end - gsp_rm_boot_dbg))

/* GA102 HS-FWSEC image (59904) + its detached signature (2248). */
extern const UINT8 fwsec_ga102_bin[];
extern const UINT8 fwsec_ga102_bin_end[];
#define FWSEC_GA102_LEN ((UINTN)(fwsec_ga102_bin_end - fwsec_ga102_bin))

extern const UINT8 fwsec_ga102_sig[];
extern const UINT8 fwsec_ga102_sig_end[];
#define FWSEC_GA102_SIG_LEN ((UINTN)(fwsec_ga102_sig_end - fwsec_ga102_sig))

/* 40HX-native HS-FWSEC image (40432), alternate to the GA102 one. */
extern const UINT8 fwsec_40hx_prod_bin[];
extern const UINT8 fwsec_40hx_prod_bin_end[];
#define FWSEC_40HX_PROD_LEN ((UINTN)(fwsec_40hx_prod_bin_end - fwsec_40hx_prod_bin))

#endif /* CMP40HX_BOOTER_ASSETS_H */
