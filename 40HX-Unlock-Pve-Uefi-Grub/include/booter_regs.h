/* booter_regs.h - SEC2 / GSP Falcon + WPR/FEAT register map for the
 * DIRECT_SEC2 booter port.
 *
 * Every offset and size below is copied VERBATIM from the working reference
 * implementation 40HX-Unlock-Drive-Patch/windows/tools/unlock40x/unlock40x_v70.c
 * (line refs in comments), NOT inferred. Do not "clean up" or reorder any
 * value without re-checking the source - a single wrong field means a hard
 * Falcon hang, not a graceful error.
 *
 * Offsets are BAR0-relative MMIO addresses (added to g->mmio by RD/WR).
 */
#ifndef CMP40HX_BOOTER_REGS_H
#define CMP40HX_BOOTER_REGS_H

/* ---- Feature-override / selector block (TU106) : v70 L551-556 ---- */
#define REG_FEAT_OVR_PLM        0x00409650UL   /* PLM: 0xffffffff = open           */
#define REG_FEAT_OVR_SM_SPD     0x00409664UL   /* SS0: 0x88888888 = full           */
#define REG_FEAT_OVR_SM_SPD_1   0x0040966CUL   /* SS1: 0x00000008 = full           */
#define REG_PFB_MMU_WPR2_LO     0x001FA824UL
#define REG_PFB_MMU_WPR2_HI     0x001FA828UL

/* ---- SEC2 Falcon engine base : v70 L567-593, 1737 ---- */
#define NV_PSEC_BASE            0x00840000UL
#define NV_PSEC_FBIF_BASE       0x00840600UL
#define NV_FALCON2_SEC_BASE     0x00841000UL

#define SEC2_IRQSTAT            (NV_PSEC_BASE + 0x008)
#define SEC2_MAILBOX0           (NV_PSEC_BASE + 0x040)
#define SEC2_MAILBOX1           (NV_PSEC_BASE + 0x044)
#define SEC2_RM                 (NV_PSEC_BASE + 0x084)   /* chipId0 */
#define SEC2_DEBUGINFO          (NV_PSEC_BASE + 0x094)
#define SEC2_HWCFG2             (NV_PSEC_BASE + 0x0F4)   /* bit31 reset_ready, bit12 mem_scrub */
#define SEC2_CPUCTL             (NV_PSEC_BASE + 0x100)   /* bit1 STARTCPU, bit4 HALTED */
#define SEC2_BOOTVEC            (NV_PSEC_BASE + 0x104)
#define SEC2_DMACTL             (NV_PSEC_BASE + 0x10C)   /* bits[2:1] IMEM/DMEM scrub */
#define SEC2_DMATRFBASE         (NV_PSEC_BASE + 0x110)
#define SEC2_DMATRFMOFFS        (NV_PSEC_BASE + 0x114)
#define SEC2_DMATRFCMD          (NV_PSEC_BASE + 0x118)   /* bit0 full, bit1 idle */
#define SEC2_DMATRFFBOFFS       (NV_PSEC_BASE + 0x11C)
#define SEC2_DMATRFBASE1        (NV_PSEC_BASE + 0x128)   /* high 9 bits of (phys>>8) */
#define SEC2_IMEMC0             (NV_PSEC_BASE + 0x180)
#define SEC2_IMEMD0             (NV_PSEC_BASE + 0x184)
#define SEC2_IMEMT0             (NV_PSEC_BASE + 0x188)   /* IMEM block tag port */
#define SEC2_DMEMC0             (NV_PSEC_BASE + 0x1C0)
#define SEC2_DMEMD0             (NV_PSEC_BASE + 0x1C4)
#define SEC2_ENGINE             (NV_PSEC_BASE + 0x3C0)   /* _RESET toggle */
#define SEC2_RESET_PLM_REG      (NV_PSEC_BASE + 0x3C4)   /* factory 0x8f / 0xff after */
#define SEC2_FBIF_CTL           (NV_PSEC_FBIF_BASE + 0x24)  /* bit7 ALLOW_PHYS_NO_CTX */
#define SEC2_BCR_CTRL           (NV_FALCON2_SEC_BASE + 0x668) /* 0=FALCON 1=RISCV */

/* ---- GSP Falcon engine base : v70 L601-785, 1737 ---- */
#define GSP_BASE                0x00110000UL
#define GSP_FALCON2_BASE        0x00111000UL   /* NV_FALCON2_GSP_BASE (GA102) */

#define GSP_MAILBOX0            (GSP_BASE + 0x040)
#define GSP_MAILBOX1            (GSP_BASE + 0x044)
#define GSP_RM                  (GSP_BASE + 0x084)
#define GSP_HWCFG2              (GSP_BASE + 0x0F4)
#define GSP_CPUCTL              (GSP_BASE + 0x100)
#define GSP_BOOTVEC             (GSP_BASE + 0x104)
#define GSP_DMACTL              (GSP_BASE + 0x10C)
#define GSP_ENGINE              (GSP_BASE + 0x3C0)
#define GSP_BCR                 (GSP_FALCON2_BASE + 0x668)
#define GSP_BROM_CURR_UCODE_ID  (GSP_FALCON2_BASE + 0x198)
#define GSP_BROM_ENGIDMASK      (GSP_FALCON2_BASE + 0x19C)
#define GSP_BROM_PARAADDR0      (GSP_FALCON2_BASE + 0x210)
#define GSP_MOD_SEL             (GSP_FALCON2_BASE + 0x180)
/* GSP DMA-tran / FBIF offsets are confirmed only when each function is
 * ported; left out here to avoid guessing (v70 fwsec_boot_gsp). */

/* ---- Falcon CPUCTL bits (referenced in v70 booter core) ---- */
#define FLCN_CPUCTL_STARTCPU    (1u << 1)
#define FLCN_CPUCTL_HALTED      (1u << 4)
#define FLCN_BLK_ALIGNMENT      256u

/* ---- WPR2 "raise" values decoded from live 8-GiB dmesg (v70 L3012-3014) ----
 * The direct flow first pokes WPR2 from host MMIO before the booter; on the
 * locked TU106 this write is the same privileged aperture the booter needs. */
#define WPR2_RAISE_LO           0x01ffe000UL
#define WPR2_RAISE_HI           0x01ffee00UL

/* ---- chipId0 for SEC2 RM : = PMC_BOOT_0(40HX) read at runtime, v70 L3050 ---- */
#define SEC2_RM_CHIPID0         0x166000a1UL
/* GSP BL RM id (GA102 bootblock), v70 L3218 ---- */
#define GSP_RM_CHIPID0          0xb72000a1UL

/* ---- Asset geometry : v70 L642-785 (byte offsets into the blobs) ---- */
#define BOOTER_UCODE_SIZE       0x0000E700UL   /* 59136 - booter_ucode_prod blob   */
#define BOOTER_SIG_PATCH_LOC    0x00008700UL   /* DMEM sig landing (0x200 in DMEM) */
#define FWSEC_SIZE              0x0000EA00UL   /* 59904 - fwsec_ga102 blob         */
#define GSP_RM_BOOT_SIZE        0x00006000UL

/* ---- Falcon CPUCTL/ENGINE field TRUE values : v70 L610-612 ---- */
#define NV_FALCON_CPUCTL_STARTCPU_TRUE  0x2u    /* bit1 */
#define NV_FALCON_CPUCTL_HALTED_TRUE    0x10u   /* bit4 */
#define NV_FALCON_ENGINE_RESET_TRUE     0x1u    /* bit0 */
#define VAL_PLM_OPEN                    0xFFFFFFFFu  /* v70 L560: PLM open verdict */
#define VAL_SS0_UNLOCKED                0x88888888u  /* v70 L561: SS0 full compute   */
#define VAL_SS1_UNLOCKED                0x00000008u  /* v70 L562: SS1 full compute  */

/* ---- Booter ucode geometry (TU102) : v70 L643-650 ----
 * tu102_direct hardcodes the same 0x100/0x8400/0x8500/0x6200 inline; these
 * named forms are what the v67 DMA fallback loader uses. */
#define BOOTER_APP_CODE_OFFSET  0x00000100UL
#define BOOTER_APP_CODE_SIZE    0x00008400UL
#define BOOTER_OS_DATA_OFFSET   0x00008500UL
#define BOOTER_OS_DATA_SIZE     0x00006200UL
#define BOOTER_HS_SIG_DMEM_ADDR 0x00000200UL   /* = BOOTER_SIG_PATCH_LOC - 0x8500 */
#define BOOTER_UCODE_ID         13             /* PATCH_META.ucodeId (TU102) */
#define BOOTER_ENGINE_ID_MASK   1              /* PATCH_META.engineId */

/* ---- SEC2 BROM / FBIF / RISC-V trace (v67 DMA fallback loader) : v70 L586-608 ---- */
#define SEC2_FBIF_TRANSCFG0     (NV_PSEC_FBIF_BASE + 0x00)
#define SEC2_MOD_SEL            (NV_FALCON2_SEC_BASE + 0x180)  /* RSA3K: |0x1 */
#define SEC2_BROM_CURR_UCODE_ID (NV_FALCON2_SEC_BASE + 0x198)
#define SEC2_BROM_ENGIDMASK     (NV_FALCON2_SEC_BASE + 0x19C)
#define SEC2_BROM_PARAADDR0     (NV_FALCON2_SEC_BASE + 0x210)
#define SEC2_TRACE_WTIDX        (NV_FALCON2_SEC_BASE + 0x408)  /* exec-progress poll */

/* ---- GSP falcon DMA-tran / FBIF / HWCFG / dbg (fwsec loaders) : v70 L775-783,825-826 ---- */
#define GSP_DEBUGINFO           (GSP_BASE + 0x094)
#define GSP_HWCFG               (GSP_BASE + 0x108)   /* IMEM_SIZE[8:0] blocks */
#define GSP_DMATRFBASE          (GSP_BASE + 0x110)
#define GSP_DMATRFMOFFS         (GSP_BASE + 0x114)
#define GSP_DMATRFCMD           (GSP_BASE + 0x118)   /* bit0 full, bit1 idle */
#define GSP_DMATRFFBOFFS        (GSP_BASE + 0x11C)
#define GSP_DMATRFBASE1         (GSP_BASE + 0x128)
#define GSP_FBIF_TRANSCFG0      (GSP_BASE + 0x600)
#define GSP_FBIF_TRANSCFG4      (GSP_BASE + 0x610)   /* TRANSCFG(dmaIdx=4), BL ctxDma */
#define GSP_FBIF_CTL            (GSP_BASE + 0x624)   /* bit7 ALLOW_PHYS_NO_CTX */

/* ---- 40HX FWSEC-HS-boot-on-GSP geometry (fwsec_boot_gsp_40hx) ----
 * BL descriptor fields (RM_FLCN_BL_DMEM_DESC), the FWSEC data-interface
 * offsets, the FRTS command, the generic-BL start tag and the WPR2 "up"
 * signature. All values copied from unlock40x_v70.c #defines and re-checked
 * against the disk bytes (a wrong descriptor field makes the BL DMA pull the
 * wrong image -> silent WPR2 never-up, not a clean error). */
#define FW40_DATA_OFF           0x00009A00UL   /* FWSEC blob data start      */
#define FW40_IFACE_OFF          0x000000E0UL   /* data interface header      */
#define FW40_MAPPER_OFF         0x00000360UL   /* DMEM mapper v3             */
#define FW40_CMDIN_OFF          0x000003B0UL   /* cmd_in_buffer              */
#define FW40_DMEM_SIZE          0x000003F0UL   /* dmemLoad                   */
#define FW40_FRTS_OFFSET        0x1FFE00000ULL /* 8 GiB FB frtsOffset        */
#define FW40_NS_SIZE            0x00000400UL   /* image[0..0x400) NS segment  */
#define FW40_SEC_BASE           0x00000400UL   /* secure IMEM/image start    */
#define FW40_SEC_SIZE           0x00009600UL   /* image[0x400..0x9a00) SEC   */
#define FWSEC_CMD_FRTS          0x15u          /* DMEM_MAPPER_V3_CMD_FRTS    */

#define BL_DESC_SIZE            0x54u          /* sizeof(RM_FLCN_BL_DMEM_DESC) */
#define BL_DESC_CTXDMA          0x20u          /* also the DMEM[0x20] readback */
#define BL_DESC_CODEDMA_LO      0x24u
#define BL_DESC_CODEDMA_HI      0x28u
#define BL_DESC_NSOFF           0x2Cu
#define BL_DESC_NSSIZE          0x30u
#define BL_DESC_SECOFF          0x34u
#define BL_DESC_SECSIZE         0x38u
#define BL_DESC_ENTRY           0x3Cu
#define BL_DESC_DATADMA_LO      0x40u
#define BL_DESC_DATADMA_HI      0x44u
#define BL_DESC_DATASIZE        0x48u
#define BL_DESC_DMEM_LOAD_OFF   0x00000000UL   /* desc -> DMEM offset (0)    */

#define GSP_BL_TU102_CODE_SIZE  0x200u         /* generic BL code (code only)*/
#define GSP_BL_TU102_START_TAG  0xFDu          /* BOOTVEC = tag<<8 = 0xfd00  */

#define FW40_WPR2_LO_UP         0x01FFE000UL   /* WPR2 lo when FRTS latched  */
#define FW40_WPR2_HI_UP         0x01FFEE00UL   /* WPR2 hi when FRTS latched  */

#endif /* CMP40HX_BOOTER_REGS_H */
