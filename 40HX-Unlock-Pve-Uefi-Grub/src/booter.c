/* booter.c - DIRECT_SEC2 booter port, tranche 1: shared Falcon helpers.
 *
 * Ported line-for-line from the empirically verified reference
 * 40HX-Unlock-Drive-Patch/windows/tools/unlock40x/unlock40x_v70.c
 * (v70 source line refs in the comments). Register map: include/booter_regs.h.
 *
 * These run in the main EFI image (compiled into the PE, not the separately
 * loaded core ELF) because the loaders that use them also need the embedded
 * firmware blobs, which link only on this side. They use ONLY the gpu_dev
 * callbacks the framework already provides:
 *   g->mmio      BAR0 window -> BWR/BRD (same macro shape as core_40hx.c)
 *   g->log       tee'd trace logger (also lands in BOOTER_TRACE.TXT)
 * No booter code below is wired to a mode yet, so this tranche changes no
 * runtime behaviour; it exists so tranche 2 (the loaders) can call it.
 */
#include "booter.h"
#include "core_abi.h"
#include "booter_regs.h"
#include "booter_assets.h"
#include "booter_wprmeta.h"

#define BWR(g, off, val)                                                       \
    do {                                                                       \
        *(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)) =                 \
            (uint32_t)(val);                                                   \
    } while (0)
#define BRD(g, off) (*(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)))

/* CPU cache flush before the Falcon DMA/ports read host memory. v70 issues
 * this inline everywhere a buffer must be coherent for the engine. */
#define WBINVD() __asm__ volatile("wbinvd" ::: "memory")

/* Byte-wise memcpy/memset live in main.c (same PE image); declared here the
 * same way elf_loader.c declares them, so the tranche-3 buffer copies have a
 * prototype. They resolve to the image's own definitions, not a libc. */
extern void *SYSVABI memcpy(void *destination, const void *source, size_t size);
extern void *SYSVABI memset(void *destination, int value, size_t size);

/* v70 L1758 falcon_wait_reset_ready - wait HWCFG2[31] RESET_READY.
 * v70 budget is 10000 x Stall(100us) = ~1s wall. The framework timer is ms
 * grained, so 1000 x delay_ms(1) reproduces that ~1s bound; a bare register
 * spin (no delay) would give up in microseconds and let the caller act on a
 * not-yet-ready engine. */
void booter_falcon_wait_reset_ready(struct gpu_dev *g, UINT32 hwcfg2Reg)
{
    UINTN i;
    for (i = 0; i < 1000; i++) {
        if (BRD(g, hwcfg2Reg) & (1u << 31))
            return;
        g->delay_ms(1);
    }
}

/* v70 L1739 falcon_wait_scrub_done - post-reset IMEM/DMEM scrub wait.
 * Skipping this is what gave "DEAD5EC1 everywhere": a reset Falcon scrubs
 * IMEM/DMEM, and any load issued mid-scrub is erased. Wait until DMACTL[2:1]
 * (IMEM+DMEM scrubbing) and HWCFG2[12] (MEM_SCRUBBING) are both clear.
 * v70 budget is 30000 x Stall(100us) = ~3s; polling it as a fast spin
 * (~30ms) would exit before the scrubber finishes and re-trigger the exact
 * DEAD5EC1 race, so we space the polls with delay_ms(1) over 3000 iters for
 * the same ~3s wall bound. */
int booter_falcon_wait_scrub_done(struct gpu_dev *g, UINT32 dmactlReg,
                                  UINT32 hwcfg2Reg, const char *tag)
{
    UINTN i;
    UINT32 dct = 0xFFFFFFFFu, hcfg = 0xFFFFFFFFu;
    for (i = 0; i < 3000; i++) {
        dct = BRD(g, dmactlReg);
        if ((dct & 0x6u) == 0) {
            hcfg = BRD(g, hwcfg2Reg);
            if (!(hcfg & (1u << 12)))
                return 0;
        }
        g->delay_ms(1);
    }
    g->log(g, "scrub-wait TIMEOUT (%s) dmactl=%08x hwcfg2=%08x\n",
           tag, dct, hcfg);
    return -1;
}

/* v70 L1707 / L1716 SEC2 DMA queue throttles (DMATRFCMD bit0 FULL / bit1 IDLE). */
static void booter_dma_wait_not_full(struct gpu_dev *g)
{
    UINTN i;
    for (i = 0; i < 1000000; i++)
        if (!(BRD(g, SEC2_DMATRFCMD) & 0x1u))
            return;
}

static void booter_dma_wait_idle(struct gpu_dev *g)
{
    UINTN i;
    for (i = 0; i < 1000000; i++)
        if (BRD(g, SEC2_DMATRFCMD) & 0x2u)
            return;
}

/* v70 L1769 falcon_dma_transfer - 256B block upload through SEC2 DMATRF regs. */
void booter_falcon_dma_transfer(struct gpu_dev *g, UINT32 dest, UINT32 memOff,
                                UINT64 srcPhys, UINT32 size, UINT32 dmaCmd)
{
    UINT32 bytesXfered = 0;

    booter_dma_wait_not_full(g);
    BWR(g, SEC2_DMATRFBASE, (UINT32)(srcPhys >> 8));
    BWR(g, SEC2_DMATRFBASE1, (UINT32)(((srcPhys >> 8) >> 32)) & 0x1FFu);

    while (bytesXfered < size) {
        booter_dma_wait_not_full(g);
        BWR(g, SEC2_DMATRFMOFFS, dest);
        BWR(g, SEC2_DMATRFFBOFFS, memOff);
        BWR(g, SEC2_DMATRFCMD, dmaCmd);
        bytesXfered += FLCN_BLK_ALIGNMENT;
        dest        += FLCN_BLK_ALIGNMENT;
        memOff      += FLCN_BLK_ALIGNMENT;
    }
    booter_dma_wait_idle(g);
}

/* v70 L2937 u40x_imem_write - IMEM port load: AINCW (bit24) plus an IMEMT
 * block tag every 64 words (256B). sec!=0 sets IMEMC_SECURE (bit28), the same
 * secure window SEC=1 DMA writes; non-secure reads cannot see it. */
void booter_u40x_imem_write(struct gpu_dev *g, UINT32 dst, UINT32 sec,
                            const UINT32 *src, UINT32 sizeBytes,
                            UINT32 virtAddr)
{
    UINT32 words = sizeBytes >> 2;
    UINT32 tag   = virtAddr >> 8;
    UINT32 i;

    BWR(g, SEC2_IMEMC0,
        (dst & 0xFFFFFCu) | (1u << 24) | (sec ? (1u << 28) : 0u));
    for (i = 0; i < words; i++) {
        if ((i & 63u) == 0u) {
            BWR(g, SEC2_IMEMT0, tag & 0xFFFFu);
            tag++;
        }
        BWR(g, SEC2_IMEMD0, src[i]);
    }
}

/* v70 L2957 u40x_dmem_write - DMEM port load: AINCW, sequential DMEMD. */
void booter_u40x_dmem_write(struct gpu_dev *g, UINT32 dst, const UINT32 *src,
                            UINT32 sizeBytes)
{
    UINT32 words = sizeBytes >> 2;
    UINT32 i;

    BWR(g, SEC2_DMEMC0, (dst & 0xFFFFFCu) | (1u << 24));
    for (i = 0; i < words; i++)
        BWR(g, SEC2_DMEMD0, src[i]);
}

/* ====================================================================
 * Tranche 2: GSP primitives + the primary DIRECT_SEC2 port loader.
 * Still un-wired (no mode calls these) -> no runtime effect.
 * ==================================================================== */

/* v70 L1841 gsp_engine_reset - kill the VBIOS-booted GFW so SEC2 unlocks.
 * kgspResetHw_TU102 replica: ENGINE._RESET=TRUE -> reads -> FALSE. */
void booter_gsp_engine_reset(struct gpu_dev *g)
{
    UINTN i;
    g->log(g, "gsp: ENGINE reset (stop GFW from POST)\n");
    BWR(g, GSP_ENGINE, NV_FALCON_ENGINE_RESET_TRUE);
    for (i = 0; i < 16; i++) BRD(g, GSP_ENGINE);
    BWR(g, GSP_ENGINE, 0);
    for (i = 0; i < 16; i++) BRD(g, GSP_ENGINE);
}

/* v70 L1866/1876 GSP DMA queue throttles (bounded, no stall). */
static void booter_gsp_dma_wait_not_full(struct gpu_dev *g)
{
    UINTN i;
    for (i = 0; i < 20000; i++)
        if (!(BRD(g, GSP_DMATRFCMD) & 0x1u))
            return;
}

static void booter_gsp_dma_wait_idle(struct gpu_dev *g)
{
    UINTN i;
    for (i = 0; i < 20000; i++)
        if (BRD(g, GSP_DMATRFCMD) & 0x2u)
            return;
}

/* v70 L1886 gsp_dma_transfer - 256B block upload through GSP DMATRF regs. */
void booter_gsp_dma_transfer(struct gpu_dev *g, UINT32 dest, UINT32 memOff,
                             UINT64 srcPhys, UINT32 size, UINT32 dmaCmd)
{
    UINT32 bytesXfered = 0;

    booter_gsp_dma_wait_not_full(g);
    BWR(g, GSP_DMATRFBASE, (UINT32)(srcPhys >> 8));
    BWR(g, GSP_DMATRFBASE1, (UINT32)(((srcPhys >> 8) >> 32)) & 0x1FFu);

    while (bytesXfered < size) {
        booter_gsp_dma_wait_not_full(g);
        BWR(g, GSP_DMATRFMOFFS, dest);
        BWR(g, GSP_DMATRFFBOFFS, memOff);
        BWR(g, GSP_DMATRFCMD, dmaCmd);
        bytesXfered += FLCN_BLK_ALIGNMENT;
        dest        += FLCN_BLK_ALIGNMENT;
        memOff      += FLCN_BLK_ALIGNMENT;
    }
    booter_gsp_dma_wait_idle(g);
}

/* v70 L2980 booter_load_tu102_direct - PRIMARY DIRECT_SEC2 port loader.
 * TU106 SEC2 is BOOT_DIRECT (kgspExecuteHsFalcon_TU102 asserts !bBootFromHs):
 * the image goes in through IMEMC/IMEMD+IMEMT and DMEMC/DMEMD ports, NOT the
 * DMATRF DMA engine. BOOTVEC=0, RM=chipId0(0x166000a1), mailbox = WPR meta
 * phys (64-bit direct, no <4G copy). Two attempts: A no-reset (driver-style,
 * exploit runs on the FWSEC-halted engine), B engine reset. After STARTCPU we
 * hold 3 s touching NO register (v57: a read during booter wedges the block). */
int booter_load_tu102_direct(struct gpu_dev *g, UINT64 wprMetaPhys,
                             UINT64 ucodePhys)
{
    UINT8 *img = (UINT8 *)(UINTN)ucodePhys;
    UINT64 metaLow = wprMetaPhys;
    UINT32 data;
    UINT32 rbNs, rbSec;
    UINT32 cpu = 0, dbg = 0, irq = 0, m0 = 0, m1 = 0;
    UINTN attempt;
    UINTN i;

    g->log(g, "\n=== tu102_direct: BOOT_DIRECT port loader ===\n");

    /* pre-probe: single-shot reads of known-safe registers only. RESET_PLM is
     * read AFTER the WPR2 poke here (v62); on a bare locked SEC2 it can hang,
     * so keep it last among the pre reads. */
    data = BRD(g, SEC2_HWCFG2);
    g->log(g, "tu102 pre: hwcfg2=%08x\n", data);
    data = BRD(g, SEC2_CPUCTL);
    g->log(g, "tu102 pre: cpuctl=%08x\n", data);
    data = BRD(g, SEC2_DMATRFCMD);
    g->log(g, "tu102 pre: dmatrf=%08x\n", data);
    g->log(g, "tu102 pre: wpr2 hi=%08x lo=%08x\n",
           BRD(g, REG_PFB_MMU_WPR2_HI), BRD(g, REG_PFB_MMU_WPR2_LO));

    /* v62: try to raise WPR2 (40HX 8 GiB) from host; may be gated - read back. */
    g->log(g, "tu102: raise WPR2 lo=%08x hi=%08x\n", WPR2_RAISE_LO, WPR2_RAISE_HI);
    BWR(g, REG_PFB_MMU_WPR2_LO, WPR2_RAISE_LO);
    BWR(g, REG_PFB_MMU_WPR2_HI, WPR2_RAISE_HI);
    WBINVD();
    g->delay_ms(5);
    g->log(g, "tu102: WPR2 rb hi=%08x lo=%08x\n",
           BRD(g, REG_PFB_MMU_WPR2_HI), BRD(g, REG_PFB_MMU_WPR2_LO));
    g->log(g, "tu102: read reset_plm...\n");
    g->log(g, "tu102: reset_plm=%08x (cmpunlocker exp 0xff)\n",
           BRD(g, SEC2_RESET_PLM_REG));

    for (attempt = 0; attempt < 2; attempt++) {
        int bReset = (attempt == 1);

        g->log(g, "\n--- tu102 attempt %d/2: %s ---\n", (int)attempt + 1,
               bReset ? "engine-reset" : "no-reset (driver-style)");

        if (bReset) {
            for (i = 0; i < 100000; i++)
                if (BRD(g, SEC2_HWCFG2) & 0x80000000u)
                    break;
            BWR(g, SEC2_ENGINE, 0x1);
            for (i = 0; i < 16; i++) BRD(g, SEC2_ENGINE);
            BWR(g, SEC2_ENGINE, 0x0);
            for (i = 0; i < 16; i++) BRD(g, SEC2_ENGINE);
            booter_falcon_wait_scrub_done(g, SEC2_DMACTL, SEC2_HWCFG2,
                                          "tu102-reset");
            g->delay_ms(50);
            BWR(g, SEC2_BCR_CTRL, 0x0);
            for (i = 0; i < 16; i++) BRD(g, SEC2_BCR_CTRL);
            g->delay_ms(10);
            BWR(g, SEC2_RM, SEC2_RM_CHIPID0);
            cpu = BRD(g, SEC2_CPUCTL);
            g->log(g, "tu102: reset done RM=%08x cpuctl=%08x\n",
                   SEC2_RM_CHIPID0, cpu);
        } else {
            cpu = BRD(g, SEC2_CPUCTL);
            g->log(g, "tu102: cpuctl=%08x halted=%d\n", cpu,
                   (cpu & NV_FALCON_CPUCTL_HALTED_TRUE) ? 1 : 0);
        }

        /* kflcnDisableCtxReq: FBIF_CTL bit7 + DMACTL=0 */
        data = BRD(g, SEC2_FBIF_CTL);
        data |= (1u << 7);
        BWR(g, SEC2_FBIF_CTL, data);
        BWR(g, SEC2_DMACTL, 0);

        /* port load: NS IMEM[0..0x100), SEC IMEM[0x100..0x8500), DMEM 0x6200
         * (signature already patched by caller at image+0x8700 = DMEM+0x200) */
        g->log(g, "tu102: port load IMEM ns+sec + DMEM\n");
        booter_u40x_imem_write(g, 0x000u, 0, (const UINT32 *)(img + 0x0u),
                               0x100u, 0x0u);
        booter_u40x_imem_write(g, 0x100u, 1, (const UINT32 *)(img + 0x100u),
                               0x8400u, 0x100u);
        booter_u40x_dmem_write(g, 0x0u, (const UINT32 *)(img + 0x8500u),
                               0x6200u);
        WBINVD();

        /* readback (pre-STARTCPU, safe) */
        BWR(g, SEC2_IMEMC0, 0x0u);
        rbNs = BRD(g, SEC2_IMEMD0);
        BWR(g, SEC2_IMEMC0, 0x100u | (1u << 28));
        rbSec = BRD(g, SEC2_IMEMD0);
        BWR(g, SEC2_DMEMC0, 0x200u);
        g->log(g, "tu102: rb IMEM[0]=%08x(exp %08x) IMEM_S=%08x "
                  "DMEM[0x200]=%08x(exp sig %08x)\n",
               rbNs, *(UINT32 *)(img + 0x0u), rbSec, BRD(g, SEC2_DMEMD0),
               *(UINT32 *)(img + BOOTER_SIG_PATCH_LOC));

        /* BOOTVEC=0, mailbox = WPR meta phys (direct, no low copy), STARTCPU */
        BWR(g, SEC2_BOOTVEC, 0x0u);
        BWR(g, SEC2_MAILBOX0, (UINT32)(metaLow & 0xFFFFFFFFu));
        BWR(g, SEC2_MAILBOX1, (UINT32)(metaLow >> 32));
        WBINVD();
        g->log(g, "tu102: BOOTVEC=0 mbox0=%08x mbox1=%08x STARTCPU\n",
               (UINT32)(metaLow & 0xFFFFFFFFu), (UINT32)(metaLow >> 32));
        BWR(g, SEC2_CPUCTL, NV_FALCON_CPUCTL_STARTCPU_TRUE);
        g->log(g, "tu102: started; hold 3s WITHOUT reg reads (v57 lesson)\n");
        g->delay_ms(3000);

        /* sparse single reads, each labelled BEFORE the read (v70 read#N
         * breadcrumb) so a hung register read is attributable in the trace. */
        g->log(g, "tu102: read#1 cpu...\n");
        cpu = BRD(g, SEC2_CPUCTL);
        g->log(g, "tu102:   cpu=%08x\n", cpu);
        g->delay_ms(1000);
        g->log(g, "tu102: read#2 irq...\n");
        irq = BRD(g, SEC2_IRQSTAT);
        g->log(g, "tu102:   irq=%08x\n", irq);
        g->log(g, "tu102: read#3 dbg...\n");
        dbg = BRD(g, SEC2_DEBUGINFO);
        g->log(g, "tu102:   dbg=%08x\n", dbg);
        g->log(g, "tu102: read#4 mbox...\n");
        m0 = BRD(g, SEC2_MAILBOX0);
        m1 = BRD(g, SEC2_MAILBOX1);
        g->log(g, "tu102:   m0=%08x m1=%08x\n", m0, m1);
        g->log(g, "tu102 RESULT: PLM=%08x SS0=%08x SS1=%08x WPR2=%08x:%08x\n",
               BRD(g, REG_FEAT_OVR_PLM), BRD(g, REG_FEAT_OVR_SM_SPD),
               BRD(g, REG_FEAT_OVR_SM_SPD_1), BRD(g, REG_PFB_MMU_WPR2_HI),
               BRD(g, REG_PFB_MMU_WPR2_LO));

        if (m0 == 0u) {
            g->log(g, "tu102: *** mbox0=0 -> booter OK (attempt %d) ***\n",
                   (int)attempt + 1);
            return 0;
        }
        g->log(g, "tu102: attempt %d not-OK (m0=%08x halted=%d)\n",
               (int)attempt + 1, m0,
               (cpu & NV_FALCON_CPUCTL_HALTED_TRUE) ? 1 : 0);
        if (attempt == 0)
            g->log(g, "tu102: -> retry with engine reset (attempt B)\n");
    }
    return -1;
}

/* v70 L2186 fwsec_boot_gsp_40hx - WITH_LOADER HS-FWSEC boot on the GSP falcon.
 * This is what latches WPR2 (the privileged aperture the SEC2 booter later
 * loads into). It mirrors kgspExecuteHsFalcon_TU102 + s_prepareHsFalconWith-
 * Loader: reset GSP, patch the FWSEC data iface for the FRTS command, publish
 * the BL descriptor + generic BL into GSP DMEM/IMEM through the debug ports
 * (host cannot write secure IMEM directly, so a tiny 768B BL in non-secure
 * IMEM @0xfd00 does the secure DMA), STARTCPU, then poll WPR2 for ~5s. The
 * STAGE_CONTROL and FWSEC_SB_RESTORE diagnostic branches are dropped. */
int booter_fwsec_boot_gsp_40hx(struct gpu_dev *g, UINT64 fwsecPhys)
{
    UINTN i;
    UINT32 data;
    UINT8 *blob = (UINT8 *)(UINTN)fwsecPhys;
    UINT8 *dmem = blob + FW40_DATA_OFF;
    UINT32 *mapper;
    UINT32 *c;

    g->log(g, "\n=== 40HX FWSEC HS-boot on GSP (blob 0x9a00+0x3f0) ===\n");

    /* 1. kflcnReset(GSP): IRQMSET=0, wait reset-ready, ENGINE reset, wait
     * scrub, BCR=FALCON(0), RM=chipId0(40HX). Turing has no hard core switch,
     * so BCR=0 readback 0x0 is normal. */
    g->log(g, "fwsec40: kflcnReset(GSP)...\n");
    BWR(g, GSP_BASE + 0x80, 0x0);            /* IRQMSET=0 (E1 trace off) */
    booter_falcon_wait_reset_ready(g, GSP_HWCFG2);
    BWR(g, GSP_ENGINE, 0x1);
    for (i = 0; i < 16; i++) BRD(g, GSP_ENGINE);
    BWR(g, GSP_ENGINE, 0x0);
    for (i = 0; i < 16; i++) BRD(g, GSP_ENGINE);
    booter_falcon_wait_scrub_done(g, GSP_DMACTL, GSP_HWCFG2, "gsp40-reset");
    g->delay_ms(50);
    BWR(g, GSP_BCR, 0x0);                    /* CORE_SELECT=FALCON (TU102) */
    for (i = 0; i < 16; i++) BRD(g, GSP_BCR);
    BWR(g, GSP_RM, SEC2_RM_CHIPID0);         /* chipId0 40HX (TU106) */
    for (i = 0; i < 16; i++) BRD(g, GSP_RM);
    g->log(g, "fwsec40: GSP reset cpuctl=%08x bcr=%08x hwcfg2=%08x\n",
           BRD(g, GSP_CPUCTL), BRD(g, GSP_BCR), BRD(g, GSP_HWCFG2));
    if ((BRD(g, GSP_CPUCTL) & 0xBADF0000u) == 0xBADF0000u) {
        g->log(g, "fwsec40: GSP locked (0xBADF) - abort\n");
        return -1;
    }

    /* 2. patch DMEM data iface: mapper.init_cmd=FRTS(0x15), cmd_in holds a
     * readVbiosDesc (24B) + frtsRegionDesc (20B) that point FRTS at the 8 GiB
     * FB region, exactly as the driver s_vbiosPatchInterfaceData does. */
    mapper = (UINT32 *)(dmem + FW40_MAPPER_OFF);
    c = (UINT32 *)(dmem + FW40_CMDIN_OFF);
    g->log(g, "fwsec40: iface hdr={%d,%d,%d,%d} mapper sig=%08x ver=%d "
              "cmd_in_off=%x size=%x\n",
           dmem[FW40_IFACE_OFF], dmem[FW40_IFACE_OFF + 1],
           dmem[FW40_IFACE_OFF + 2], dmem[FW40_IFACE_OFF + 3],
           mapper[0], (UINT16)((mapper[0] >> 16) & 0xFFFFu), mapper[2],
           mapper[3]);
    if (mapper[2] < FW40_DMEM_SIZE && mapper[2] != FW40_CMDIN_OFF)
        g->log(g, "fwsec40: mapper.cmd_in_off=%x (!=0x3b0) - using mapper value\n",
               mapper[2]);
    c[0] = 1; c[1] = 24; c[2] = 0; c[3] = 0; c[4] = 0; c[5] = 2;   /* readVbiosDesc */
    c[6] = 1; c[7] = 20;                                            /* frtsRegionDesc */
    c[8] = (UINT32)(FW40_FRTS_OFFSET >> 12);
    c[9] = 0x100;
    c[10] = 2;
    mapper[11] = FWSEC_CMD_FRTS;             /* init_cmd = 0x15 (FRTS) */
    WBINVD();
    g->log(g, "fwsec40: mapper.init_cmd=%x frts4k=%x (frts=%llx)\n",
           mapper[11], c[8], (unsigned long long)FW40_FRTS_OFFSET);

    /* 3. kflcnDisableCtxReq + TRANSCFG: host DMA into the falcon needs the
     * phys-no-ctx aperture, and the BL uses ctxDma=4 (COHERENT|PHYS). */
    data = BRD(g, GSP_FBIF_CTL);
    data |= (1u << 7);
    BWR(g, GSP_FBIF_CTL, data);
    BWR(g, GSP_DMACTL, 0);
    BWR(g, GSP_FBIF_TRANSCFG0, 0x5);
    BWR(g, GSP_FBIF_TRANSCFG4, 0x5);
    g->log(g, "fwsec40: FBIF bit7 + TRANSCFG0/4=5 DMACTL=0\n");

    /* 4. WITH_LOADER: (a) BL descriptor -> DMEM 0x0 (AINCW ports),
     *    (b) generic BL code -> top of GSP IMEM (dst = IMEM_SIZE - code),
     *    (c) STARTCPU makes the BL DMA the FWSEC code/data into secure IMEM. */
    {
        UINT8   desc[BL_DESC_SIZE];
        UINT32 *d = (UINT32 *)desc;
        const UINT32 *bl = (const UINT32 *)gsp_bl_tu102;
        UINT32 imemSizeBlk, imemDstBlk, blDstAddr, v;
        UINT32 codePaLo = (UINT32)(fwsecPhys & 0xFFFFFFFF);
        UINT32 codePaHi = (UINT32)(fwsecPhys >> 32);
        UINT64 dataPhys = fwsecPhys + FW40_DATA_OFF;

        for (i = 0; i < BL_DESC_SIZE / 4; i++) d[i] = 0;
        d[BL_DESC_CTXDMA / 4]     = 4;
        d[BL_DESC_CODEDMA_LO / 4] = codePaLo;
        d[BL_DESC_CODEDMA_HI / 4] = codePaHi;
        d[BL_DESC_NSOFF / 4]      = 0;
        d[BL_DESC_NSSIZE / 4]     = FW40_NS_SIZE;
        d[BL_DESC_SECOFF / 4]     = FW40_SEC_BASE;
        d[BL_DESC_SECSIZE / 4]    = FW40_SEC_SIZE;
        d[BL_DESC_ENTRY / 4]      = 0;
        d[BL_DESC_DATADMA_LO / 4] = (UINT32)(dataPhys & 0xFFFFFFFF);
        d[BL_DESC_DATADMA_HI / 4] = (UINT32)(dataPhys >> 32);
        d[BL_DESC_DATASIZE / 4]   = FW40_DMEM_SIZE;
        g->log(g, "fwsec40: BL DESC ctxDma=4 code=%llx (ns%x@0 sec%x@0x400) "
                  "data=%llx(%x)\n",
               (unsigned long long)fwsecPhys, FW40_NS_SIZE, FW40_SEC_SIZE,
               (unsigned long long)dataPhys, FW40_DMEM_SIZE);

        /* (a) descriptor -> DMEM 0x0 */
        BWR(g, GSP_BASE + 0x1C0, BL_DESC_DMEM_LOAD_OFF | (1u << 24));
        for (i = 0; i < BL_DESC_SIZE / 4; i++)
            BWR(g, GSP_BASE + 0x1C4, d[i]);
        WBINVD();
        BWR(g, GSP_BASE + 0x1C0, 0);

        /* (b) generic BL code -> IMEM top (driver s_setupLoader formula) */
        imemSizeBlk = BRD(g, GSP_HWCFG) & 0x1FF;
        imemDstBlk  = imemSizeBlk - (GSP_BL_TU102_CODE_SIZE / 256);
        blDstAddr   = imemDstBlk << 8;
        g->log(g, "fwsec40: BL IMEM top hwcfg.IMEM=%d dst=%x (BOOTVEC=%x)%s\n",
               imemSizeBlk, blDstAddr, (UINT32)(GSP_BL_TU102_START_TAG << 8),
               (blDstAddr == (GSP_BL_TU102_START_TAG << 8)) ? "" :
               "  <== WARN dst!=0xfd00");
        BWR(g, GSP_BASE + 0x180, blDstAddr | (1u << 24));   /* IMEMC0 AINCW */
        {
            UINT32 tag = GSP_BL_TU102_START_TAG;
            for (i = 0; i < GSP_BL_TU102_CODE_SIZE / 4; i++) {
                if ((i & 63u) == 0u) { BWR(g, GSP_BASE + 0x188, tag); tag++; }
                BWR(g, GSP_BASE + 0x184, bl[i]);
            }
        }
        WBINVD();
        BWR(g, GSP_BASE + 0x180, 0);

        /* (c) readback: BL[0] landed + desc.ctxDma landed (logs only) */
        BWR(g, GSP_BASE + 0x180, blDstAddr);
        v = BRD(g, GSP_BASE + 0x184);
        g->log(g, "fwsec40: rb IMEM[%x]=%08x (exp BL[0]=%08x)%s\n",
               blDstAddr, v, bl[0], (v == bl[0]) ? "  <== OK" : "  <== MISMATCH");
        BWR(g, GSP_BASE + 0x1C0, BL_DESC_CTXDMA);
        g->log(g, "fwsec40: rb DMEM[0x20]=%08x (exp ctxDma 4)%s\n",
               BRD(g, GSP_BASE + 0x1C4),
               (BRD(g, GSP_BASE + 0x1C4) == 4) ? "" : "  <== MISMATCH");
        BWR(g, GSP_BASE + 0x1C0, 0);
    }

    /* 5. BOOTVEC = blStartTag<<8 (BL self-locates) + STARTCPU. No BROM params:
     * kgspExecuteHsFalcon_TU102 WITH_LOADER only prepares + starts. */
    BWR(g, GSP_BOOTVEC, GSP_BL_TU102_START_TAG << 8);
    WBINVD();
    BWR(g, GSP_CPUCTL, NV_FALCON_CPUCTL_STARTCPU_TRUE);
    g->log(g, "fwsec40: STARTCPU (BL@0xfd00 -> DMA FWSEC), poll WPR2 up 5s...\n");

    /* 6. poll WPR2 -> FRTS success sets it up; every 500ms dump why not. */
    for (i = 0; i < 5000; i++) {
        UINT32 lo = BRD(g, REG_PFB_MMU_WPR2_LO);
        UINT32 hi = BRD(g, REG_PFB_MMU_WPR2_HI);
        if ((lo & 0xFFFFFFF0u) == FW40_WPR2_LO_UP &&
            (hi & 0xFFFFFFF0u) == FW40_WPR2_HI_UP) {
            g->log(g, "fwsec40: *** WPR2 UP lo=%08x hi=%08x after %d ms ***\n",
                   lo, hi, (int)i);
            return 0;
        }
        if ((i % 500) == 0) {
            UINT32 vIM, vD;
            BWR(g, GSP_BASE + 0x180, 0);
            vIM = BRD(g, GSP_BASE + 0x184);
            BWR(g, GSP_BASE + 0x1C0, 0x10);
            vD = BRD(g, GSP_BASE + 0x1C4);
            BWR(g, GSP_BASE + 0x1C0, 0);
            BWR(g, GSP_BASE + 0x180, 0);
            g->log(g, "fwsec40: t=%d wpr2lo=%08x wpr2hi=%08x gspcpu=%x dbg=%x "
                      "mbox0=%x IMEM[0]=%08x(exp %08x) DMEM[0x10]=%08x(exp %08x)\n",
                   (int)i, lo, hi, BRD(g, GSP_CPUCTL), BRD(g, GSP_DEBUGINFO),
                   BRD(g, GSP_MAILBOX0), vIM, *(UINT32 *)(UINTN)fwsecPhys, vD,
                   *(UINT32 *)(UINTN)(fwsecPhys + FW40_DATA_OFF + 0x10));
        }
        g->delay_ms(1);
    }
    g->log(g, "fwsec40: WPR2 NOT up lo=%08x hi=%08x gspcpu=%x dbg=%x mbox0=%x\n",
           BRD(g, REG_PFB_MMU_WPR2_LO), BRD(g, REG_PFB_MMU_WPR2_HI),
           BRD(g, GSP_CPUCTL), BRD(g, GSP_DEBUGINFO), BRD(g, GSP_MAILBOX0));
    return -1;
}

/* ====================================================================
 * Tranche 3: host-side buffer preparation the loaders consume (v70 flow
 * steps [6]-[7] + [8b] fwsec alloc + [9] sig patch). This is pure memory
 * work - no MMIO, no engine touched - so it stays dead code until tranche 4
 * wires it. All addresses come from booter_alloc_high (identity-mapped
 * physical == virtual, >4 GiB as the TU102 booter requires).
 * ==================================================================== */

_Static_assert(WPR_META_SIZE == sizeof(GspFwWprMeta),
               "GspFwWprMeta size != WPR_META_SIZE");

/* v70 L4871 build_wpr_meta - fill the 256B metadata the booter parses.
 * The entire FB/heap/WPR layout is a pure function of fbSize; production
 * passes 8 GiB (BOOTER_FB_SIZE). Two stage-only branches are dropped: the
 * cmp40_stockSig signature override (flag defaults 0 -> V67_SIZE) and the
 * CPU_RM_HANDOFF 960KiB heap override (not defined in the production build,
 * so the fbSize-driven heap formula is the real path). BL offsets 0x1800/0x800
 * are the live-driver values confirmed in the v70 dump comment. */
static void booter_build_wpr_meta(GspFwWprMeta *m, UINT64 elfPhys,
                                  UINT64 elfSize, UINT64 sigPhys,
                                  UINT64 fbSize, UINT64 blPhys, UINT64 blSize)
{
    UINT64 wprEnd;
    const UINT64 MB = 0x100000ULL;

    memset(m, 0, sizeof(*m));
    m->magic    = GSP_FW_WPR_META_MAGIC;
    m->revision = GSP_FW_WPR_META_REVISION;

    m->sysmemAddrOfRadix3Elf = elfPhys;
    m->sizeOfRadix3Elf       = elfSize;

    m->sysmemAddrOfSignature = sigPhys;
    m->sizeOfSignature       = V67_SIZE;

    m->sysmemAddrOfBootloader   = blPhys;
    m->sizeOfBootloader         = blSize;
    m->bootloaderCodeOffset     = 0x1800;
    m->bootloaderDataOffset     = 0x800;
    m->bootloaderManifestOffset = 0x0;

    m->fbSize = fbSize;

    /* No display fuse on CMP 40HX: vgaWorkspace = fbSize - PRAMIN(1MB). */
    m->vgaWorkspaceOffset = fbSize - 0x100000ULL;
    m->vgaWorkspaceSize   = fbSize - m->vgaWorkspaceOffset;

    wprEnd = m->vgaWorkspaceOffset & ~0x1FFFFULL;   /* 128KB aligned */
    m->gspFwWprEnd = wprEnd;

    m->frtsSize   = 0x100000ULL;
    m->frtsOffset = m->gspFwWprEnd - m->frtsSize;

    m->bootBinOffset = (m->frtsOffset - blSize) & ~0xFFFULL;   /* ALIGN_DOWN 4K */
    m->gspFwOffset   = (m->bootBinOffset - elfSize) & ~0xFFFFULL;  /* 64K align */

    m->gspFwHeapSize   = (fbSize == 0x280000000ULL) ? 0x7F00000ULL
                                                    : 0x6900000ULL;
    m->gspFwHeapOffset = (m->gspFwOffset - m->gspFwHeapSize) & ~(MB - 1);
    m->gspFwWprStart   = m->gspFwHeapOffset - MB;
    m->nonWprHeapSize  = MB;
    m->nonWprHeapOffset = m->gspFwWprStart - MB;
    m->gspFwRsvdStart  = m->nonWprHeapOffset;

    m->bootCount = 0;
    m->verified  = 0;
    m->pmuReservedSize = 0;
    m->gspFwHeapVfPartitionCount = 0;
    m->flags = GSP_FW_FLAGS_CLOCK_BOOST;
}

/* v70 L7410 u40x_build_dummy_fw - an all-zero "firmware" image whose size
 * (0x1bfadc8 = the real gsp_tu10x .fwimage) drives the WPR layout. No disk
 * dependency: we just reserve and zero the pages high. */
static int booter_build_dummy_fw(struct gpu_dev *g, UINT64 *dataPhys,
                                 UINT64 *dataSize)
{
    UINT64 sz = DUMMY_FW_SIZE;
    UINT64 phys = booter_alloc_high((UINTN)((sz + 0xFFFULL) >> 12));

    if (!phys) {
        g->log(g, "prep: alloc dummy fw (0x%lx) FAILED\n", sz);
        return -1;
    }
    memset((void *)(UINTN)phys, 0, (UINTN)sz);
    *dataPhys = phys;
    *dataSize = sz;
    g->log(g, "prep: dummy fw %dMB @0x%lx (ELF sz=0x%lx)\n",
           (int)(sz >> 20), phys, sz);
    return 0;
}

/* v70 L7434 u40x_build_radix - build the real radix-3 page table
 * (root -> L1 -> L2 -> data) the booter wants in place of the raw ELF image
 * (v2.86 lesson). ptSize = (2 + nL2) pages: page0 root, page1 L1, nL2 L2
 * pages, then the image data at tab+ptSize. */
static int booter_build_radix(struct gpu_dev *g, UINT64 dataPhys,
                              UINT64 dataSize, UINT64 *tabPhysOut)
{
    UINT64 nData  = (dataSize + 0xFFFULL) >> RADIX_PAGE_LOG2;
    UINT64 nL2    = ((nData - 1) >> RADIX_ENTRIES_LOG2) + 1;
    UINT64 ptSize = (2 + nL2) << RADIX_PAGE_LOG2;
    UINT64 dataOff = ptSize;
    UINT64 allocSz = dataOff + dataSize;
    UINT64 tab = booter_alloc_high((UINTN)((allocSz + 0xFFFULL) >> 12));
    UINT64 *root, *l1, *l2;
    UINTN i;

    if (!tab) {
        g->log(g, "prep: alloc radix table FAILED\n");
        return -1;
    }
    memset((void *)(UINTN)tab, 0, (UINTN)ptSize);
    memcpy((void *)(UINTN)(tab + dataOff), (const void *)(UINTN)dataPhys,
           (UINTN)dataSize);

    l2 = (UINT64 *)(UINTN)(tab + 0x2000);            /* L2 -> data pages */
    for (i = 0; i < nData; i++)
        l2[i] = tab + dataOff + ((UINT64)i << RADIX_PAGE_LOG2);
    l1 = (UINT64 *)(UINTN)(tab + 0x1000);            /* L1 -> L2 pages    */
    for (i = 0; i < nL2; i++)
        l1[i] = tab + 0x2000 + ((UINT64)i << RADIX_PAGE_LOG2);
    root = (UINT64 *)(UINTN)tab;                     /* root -> L1        */
    root[0] = tab + 0x1000;

    WBINVD();
    g->log(g, "prep: radix-3 @0x%lx (nData=%d nL2=%d, data@+0x%lx)\n",
           tab, (int)nData, (int)nL2, dataOff);
    *tabPhysOut = tab;
    return 0;
}

/* v70 flow [6]-[7] + [8b] + [9]: allocate & fill every host buffer the two
 * loaders read, build the WPR meta, then return the three addresses they
 * need. Buffers the meta merely references (v67/bl/dummy/radix) are internal.
 * Returns 0 with all three output pointers (*wprMetaPhys, *ucodePhys and
 * *fwsecPhys) set, or -1 on any alloc failure (all-zero outputs). */
int booter_prepare(struct gpu_dev *g, UINT64 *wprMetaPhys,
                      UINT64 *ucodePhys, UINT64 *fwsecPhys)
{
    /* SIG_PROD (16B AES) the driver writes into the booter ucode DMEM slot
     * before load, so BROM signature verify passes (v70 L8710). */
    static const UINT8 sig_prod[16] = {
        0x32, 0x60, 0x43, 0x67, 0x98, 0x89, 0x7b, 0x06,
        0xe8, 0x5f, 0x1a, 0xa8, 0x71, 0x43, 0x5d, 0xcb
    };
    UINT64 v67Phys = 0, blPhys = 0, fwPhys = 0, fwSize = 0;
    UINT64 radixPhys = 0, meta = 0, ucode = 0, fwsec = 0;
    GspFwWprMeta *wprMeta;
    UINTN len;

    if (!g || !wprMetaPhys || !ucodePhys || !fwsecPhys)
        return -1;
    *wprMetaPhys = *ucodePhys = *fwsecPhys = 0;

    g->log(g, "\n=== prep: host buffers (v70 [6]-[7]) ===\n");

    /* [6a] V67 signature payload: only V67_SIZE bytes are used even though
     * the embedded blob is larger. */
    v67Phys = booter_alloc_high((UINTN)((V67_SIZE + 0xFFFu) >> 12));
    if (!v67Phys) { g->log(g, "prep: alloc v67 FAILED\n"); return -1; }
    len = (V67_PAYLOAD_LEN < V67_SIZE) ? V67_PAYLOAD_LEN : V67_SIZE;
    memcpy((void *)(UINTN)v67Phys, v67_payload_bin, len);
    if (len < V67_SIZE)
        memset((void *)(UINTN)(v67Phys + len), 0, V67_SIZE - len);
    g->log(g, "prep: v67 @0x%lx (0x%x B, >4G)\n", v67Phys, V67_SIZE);

    /* [6b] SEC2 booter microcode + the 16B SIG_PROD patch at image+0x8700
     * (tu102_direct expects the caller to have done this). */
    ucode = booter_alloc_high((UINTN)((BOOTER_UCODE_SIZE + 0xFFFu) >> 12));
    if (!ucode) { g->log(g, "prep: alloc ucode FAILED\n"); return -1; }
    len = (BOOTER_UCODE_PROD_LEN < BOOTER_UCODE_SIZE) ? BOOTER_UCODE_PROD_LEN
                                                      : BOOTER_UCODE_SIZE;
    memcpy((void *)(UINTN)ucode, booter_ucode_prod, len);
    if (len < BOOTER_UCODE_SIZE)
        memset((void *)(UINTN)(ucode + len), 0, BOOTER_UCODE_SIZE - len);
    memcpy((void *)(UINTN)(ucode + BOOTER_SIG_PATCH_LOC), sig_prod,
           sizeof(sig_prod));
    WBINVD();
    g->log(g, "prep: ucode @0x%lx (0x%x B) sig@+0x%x=%08x\n", ucode,
           BOOTER_UCODE_SIZE, BOOTER_SIG_PATCH_LOC,
           *(UINT32 *)(UINTN)(ucode + BOOTER_SIG_PATCH_LOC));

    /* [6c] GspRmBoot (BL) image referenced by the meta. */
    blPhys = booter_alloc_high((UINTN)((GSP_RM_BOOT_SIZE + 0xFFFu) >> 12));
    if (!blPhys) { g->log(g, "prep: alloc bl FAILED\n"); return -1; }
    len = (GSP_RM_BOOT_DBG_LEN < GSP_RM_BOOT_SIZE) ? GSP_RM_BOOT_DBG_LEN
                                                   : GSP_RM_BOOT_SIZE;
    memcpy((void *)(UINTN)blPhys, gsp_rm_boot_dbg, len);
    if (len < GSP_RM_BOOT_SIZE)
        memset((void *)(UINTN)(blPhys + len), 0, GSP_RM_BOOT_SIZE - len);
    g->log(g, "prep: bl @0x%lx (0x%x B)\n", blPhys, GSP_RM_BOOT_SIZE);

    /* [7a] dummy fw + [7b] radix-3 page table. */
    if (booter_build_dummy_fw(g, &fwPhys, &fwSize))
        return -1;
    if (booter_build_radix(g, fwPhys, fwSize, &radixPhys))
        return -1;

    /* [7c] WPR metadata block, fbSize = 8 GiB (production). */
    meta = booter_alloc_high((UINTN)((WPR_META_SIZE + 0xFFFu) >> 12));
    if (!meta) { g->log(g, "prep: alloc meta FAILED\n"); return -1; }
    wprMeta = (GspFwWprMeta *)(UINTN)meta;
    booter_build_wpr_meta(wprMeta, radixPhys, fwSize, v67Phys,
                          BOOTER_FB_SIZE, blPhys, GSP_RM_BOOT_SIZE);
    WBINVD();
    g->log(g, "prep: meta @0x%lx wpr=%llx:%llx heap=%llx fb=%llx\n",
           meta, (unsigned long long)wprMeta->gspFwWprStart,
           (unsigned long long)wprMeta->gspFwWprEnd,
           (unsigned long long)wprMeta->gspFwHeapOffset,
           (unsigned long long)wprMeta->fbSize);

    /* [8b] 40HX-native FWSEC blob for the HS-FWSEC-on-GSP boot (WPR2 latch).
     * The BL descriptor in fwsec_boot_gsp_40hx points code at fwsecPhys and
     * data at fwsecPhys + FW40_DATA_OFF, so the whole 0x9df0 must be present. */
    fwsec = booter_alloc_high((UINTN)((FW40_BLOB_SIZE + 0xFFFu) >> 12));
    if (!fwsec) { g->log(g, "prep: alloc fwsec FAILED\n"); return -1; }
    len = (FWSEC_40HX_PROD_LEN < FW40_BLOB_SIZE) ? FWSEC_40HX_PROD_LEN
                                                 : FW40_BLOB_SIZE;
    memcpy((void *)(UINTN)fwsec, fwsec_40hx_prod_bin, len);
    if (len < FW40_BLOB_SIZE)
        memset((void *)(UINTN)(fwsec + len), 0, FW40_BLOB_SIZE - len);
    WBINVD();
    g->log(g, "prep: fwsec @0x%lx (0x%x B, 40HX native)\n", fwsec,
           FW40_BLOB_SIZE);

    *wprMetaPhys = meta;
    *ucodePhys   = ucode;
    *fwsecPhys   = fwsec;
    g->log(g, "prep: DONE meta=0x%lx ucode=0x%lx fwsec=0x%lx\n",
           meta, ucode, fwsec);
    return 0;
}

/* ====================================================================
 * Tranche 4: the wired DIRECT_SEC2 flow. booter_run is the only booter entry a
 * mode calls (main.c mode 7 / "booter"). It sequences the v70 production
 * steps exactly - prepare (pure memory) -> engine reset -> HS-FWSEC WPR2
 * latch -> DIRECT_SEC2 port load -> verdict - and mirrors every risky MMIO
 * read behind a breadcrumb line so a hard hang on the blind (remote,
 * no-screen) PVE box is attributable in BOOTER_TRACE.TXT.
 * ==================================================================== */
int booter_run(struct gpu_dev *g)
{
    UINT64 wprMetaPhys = 0, ucodePhys = 0, fwsecPhys = 0;
    UINT32 plm, ss0, ss1;
    int rc;

    if (!g)
        return -1;

    g->log(g, "\n=== DIRECT_SEC2 compute unlock (v70 [6]-[10]) ===\n");

    /* [6][7] host buffers + WPR meta. No MMIO; if a high (>4 GiB) alloc or
     * identity-mapped deref fails we abort here with the physical addresses
     * already logged by prepare, so the trace pins the failure. */
    g->log(g, "booter: [6][7] prepare host buffers\n");
    if (booter_prepare(g, &wprMetaPhys, &ucodePhys, &fwsecPhys)) {
        g->log(g, "booter: prepare FAILED (alloc / high-memory mapping)\n");
        return -1;
    }

    /* [pre] Read-only pre-reset state snapshot, logged before the normalize
     * so BOOTER_TRACE.TXT can later tell how often the reset is actually
     * needed (WPR2 hi>>4 != 0 marks a stale half-latched fwsec from the last
     * failed attempt). Pure observability: it does not gate anything yet. The
     * breadcrumb is emitted first because the reads happen while the next
     * line's arguments are evaluated (risky-MMIO-read rule). */
    g->log(g, "booter: [pre] reading pre-reset WPR2/SS0...\n");
    g->log(g, "booter: [pre] WPR2 hi=%08x lo=%08x SS0=%08x\n",
           BRD(g, REG_PFB_MMU_WPR2_HI), BRD(g, REG_PFB_MMU_WPR2_LO),
           BRD(g, REG_FEAT_OVR_SM_SPD));

    /* [pre] Normalize the endpoint to a clean power-on state with one SBR
     * before anything is latched. On a single-40HX host (no other display
     * adapter) firmware can leave WPR2/SEC2 in a state the DIRECT_SEC2
     * port-load cannot take over (SEC2 mailbox exit 0x91), and any earlier
     * failed attempt leaves WPR2 latched - which per v70 only an FLR or a full
     * power-off clears. A secondary bus reset is the in-band equivalent: it
     * resets the GPU function and drops WPR2 back to 0, so every attempt starts
     * from the same known-good base the v70 flow assumes. It sits here after
     * the pure-memory prepare (which touches no MMIO) exactly as the compute
     * path issues its SBR after register setup. Non-fatal: if the reset itself
     * fails we continue and let the loader verdict stand. */
    g->log(g, "booter: [pre] normalize SBR (clear stale WPR2/SEC2 state)\n");
    if (g->secondary_bus_reset(g))
        g->log(g, "booter: [pre] normalize SBR failed; continuing anyway\n");

    /* [8] kill the POST/VBIOS GFW so SEC2 stops answering 0xBADF. Reset
     * sits immediately before the fwsec boot (v70 order, §3.2.5). */
    g->log(g, "booter: [8] gsp_engine_reset\n");
    booter_gsp_engine_reset(g);

    /* [8b] HS-FWSEC-on-GSP latches WPR2. Non-fatal on first bring-up: log
     * the outcome and the WPR2 state, then continue to [9] regardless. */
    g->log(g, "booter: [8b] fwsec_boot_gsp_40hx (WPR2 latch)\n");
    rc = booter_fwsec_boot_gsp_40hx(g, fwsecPhys);
    g->log(g, "booter: fwsec -> %d; WPR2 hi=%08x lo=%08x\n", rc,
           BRD(g, REG_PFB_MMU_WPR2_HI), BRD(g, REG_PFB_MMU_WPR2_LO));

    /* [9] DIRECT_SEC2 port load; SIG_PROD already patched at ucode+0x8700
     * by prepare, so the booter BROM signature verify can pass. */
    g->log(g, "booter: [9] booter_load_tu102_direct\n");
    rc = booter_load_tu102_direct(g, wprMetaPhys, ucodePhys);
    g->log(g, "booter: load_tu102_direct -> %d\n", rc);

    /* [10] verdict. Settle first (the booter writes the selectors from its
     * RISC-V payload), then read PLM/SS0/SS1 - each behind a breadcrumb. */
    g->delay_ms(500);
    g->log(g, "booter: [10] read PLM...\n");
    plm = BRD(g, REG_FEAT_OVR_PLM);
    g->log(g, "booter: [10] read SS0...\n");
    ss0 = BRD(g, REG_FEAT_OVR_SM_SPD);
    g->log(g, "booter: [10] read SS1...\n");
    ss1 = BRD(g, REG_FEAT_OVR_SM_SPD_1);
    g->log(g, "booter: RESULT PLM=%08x SS0=%08x SS1=%08x WPR2=%08x:%08x\n",
           plm, ss0, ss1, BRD(g, REG_PFB_MMU_WPR2_HI),
           BRD(g, REG_PFB_MMU_WPR2_LO));
    if (ss0 == VAL_SS0_UNLOCKED && ss1 == VAL_SS1_UNLOCKED) {
        g->log(g, "booter: *** UNLOCKED (SS0=0x88888888 SS1=0x8) ***\n");
        return 0;
    }
    if (plm == VAL_PLM_OPEN || plm == 0xFFFFFF8Fu)
        g->log(g, "booter: PLM %s - SS0 still gated\n",
               plm == VAL_PLM_OPEN ? "OPEN" : "STAGED");
    else
        g->log(g, "booter: PLM closed - no booter effect observed\n");
    return -1;
}
