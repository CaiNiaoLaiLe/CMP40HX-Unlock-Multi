/*
 * core_40hx.c — TU106 (CMP 40HX) permissive core for the UEFI unlock shell.
 *
 * Values are taken from the empirically verified 40HX unlock chain
 * (Linux SEC2-booter ROP, same register family as GA102 NVPermissive):
 *   XP3G_PLM  0x8E1B0  = 0xFFFFFFFF   (compute unlock, verified)
 *   XVE_OVR   0x8872C  = 0x00000006   (PCIe Gen2 unlock, verified)
 *   FECS_PLM  0x409650 = 0xFFFFFFFF   (FECS feature lock open, verified)
 *   SS0       0x409664 = 0x88888888   (SM_SPEED_SELECT full - tensor target)
 *   SS1       0x40966C = 0x00000008   (SM_SPEED_SELECT_1 full)
 *
 * Mode flow after the compute/full modes were retired: this core only serves
 * the read-only inspect dump and the ReBAR unlock that the booter one-shot
 * chains after compute. Compute unlock itself happens through the SEC2 booter
 * (booter.c), not by direct register override here. (SBR clears PCI endpoint
 * state, GPU internal security-domain state persists - verify via inspect.)
 */
#include "core_abi.h"
#include <stdint.h>

#define WR(g, off, val)                                                    \
    do {                                                                   \
        *(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)) = (uint32_t)(val); \
    } while (0)
#define RD(g, off) (*(volatile uint32_t *)((uintptr_t)(g)->mmio + (off)))

#define XP3G_PLM_OFF 0x008E1B0U
#define FECS_PLM_OFF 0x00409650U
#define FECS_SS0_OFF 0x00409664U
#define FECS_SS1_OFF 0x0040966CU
#define FECS_SS0_RD_OFF 0x00409668U
#define FUSE_DISP_OFF   0x00021C04U

/* v29.68: ReBAR 8 GiB unlock - XVE family, plain MMIO, same domain as the
 * verified Gen2 overrides (no Falcon privilege involved). Ported from
 * the 40HX-Unlock-Drive-Patch scheme's 0003-cmp40hx-rebar-unlock
 * (provenance only; this project builds self-contained):
 *   XVE_CYA 0x88724 = 0x30          override unlock
 *   XVE_CFG 0x88dcc  selector 7 + enable(bit31) -> offered cap 0x400->0x7fc00
 *   RBAR-CTL 0x88bc0 (cfg 0xBC0 mirror) bits[13:8] = log2(MiB), 13 = 8 GiB
 * Must run AFTER the last SBR (SBR clears endpoint PCI state). Success is
 * gated on the CFG readback ONLY (exactly like the driver path, which checks
 * read_cfg and never the CYA key): XVE_CYA is a write-only override key that
 * reads back a status byte, and XVE_RBAR_CTL is a firmware-owned capability
 * mirror, so neither is compared. On a genuine CFG mismatch the whole set
 * reverts, so the failure mode stays "ReBAR off" (BAR1 64 MiB, harmless). */
#define XVE_CYA_OFF         0x0088724U
#define XVE_CAP_OFF         0x0088BBCU
#define XVE_RBAR_CTL_OFF    0x0088BC0U
#define XVE_CFG_OFF         0x0088DCCU
#define XVE_CYA_UNLOCK      0x00000030U
#define XVE_CFG_SEL_MASK    0x0000000FU
#define XVE_CFG_ENABLE      0x80000000U
#define XVE_CFG_SELECTOR    7U           /* selector 7 = 8 GiB (2^(6+7) MiB) */
#define XVE_RBAR_SIZE_MASK  0x00003F00U  /* bits [13:8] */
#define XVE_RBAR_SIZE_SHIFT 8U
#define REBAR_LOG2_MIB      13U          /* 2^13 MiB = 8 GiB */
#define REBAR_CAP_8GIB_BIT  (1U << 13)

const char nvperm_core_version[] = "40hx core v29.69 (inspect+rebar)";

static int rebar_unlock(struct gpu_dev *g)
{
    uint32_t cya, cfg, ctl;
    uint32_t cap_before, cap_after;
    int fail = 0;

    cya = RD(g, XVE_CYA_OFF);
    cfg = RD(g, XVE_CFG_OFF);
    ctl = RD(g, XVE_RBAR_CTL_OFF);
    cap_before = RD(g, XVE_CAP_OFF);
    g->log(g, "rebar before: CYA=%08x CFG=%08x CAP=%08x CTL=%08x\n",
           cya, cfg, cap_before, ctl);

    WR(g, XVE_CYA_OFF, XVE_CYA_UNLOCK);
    WR(g, XVE_CFG_OFF,
       (cfg & ~XVE_CFG_SEL_MASK) | XVE_CFG_ENABLE | XVE_CFG_SELECTOR);
    WR(g, XVE_RBAR_CTL_OFF,
       (ctl & ~XVE_RBAR_SIZE_MASK) | (REBAR_LOG2_MIB << XVE_RBAR_SIZE_SHIFT));

    /* Gate success on the CFG readback ONLY - exactly what the verified
     * driver path (nv_cmp40_unlock_bar1_resize) validates; it checks
     * read_cfg, never the CYA key or the CTL size field.
     *   - XVE_CYA 0x88724 is a write-only override KEY: it reads back a
     *     status byte (observed 0x0d after writing 0x30), NOT the value we
     *     wrote, so comparing it is a guaranteed false MISMATCH.
     *   - XVE_RBAR_CTL 0x88BC0 is a PCIe-capability mirror the firmware
     *     owns; not a hard gate either.
     * The real proof of unlock is CFG=ENABLE|selector7 sticking plus the
     * capability mask gaining the 8 GiB bit (CAP 0x400 -> 0x3fc00), logged
     * informationally below. */
    if ((RD(g, XVE_CFG_OFF) & (XVE_CFG_ENABLE | XVE_CFG_SEL_MASK)) !=
        (XVE_CFG_ENABLE | XVE_CFG_SELECTOR))
        fail = 1;
    cap_after = RD(g, XVE_CAP_OFF);
    g->log(g, "rebar after:  CYA=%08x CFG=%08x CAP=%08x CTL=%08x%s\n",
           RD(g, XVE_CYA_OFF), RD(g, XVE_CFG_OFF), cap_after,
           RD(g, XVE_RBAR_CTL_OFF), fail ? " (MISMATCH)" : "");
    if (!(cap_after & REBAR_CAP_8GIB_BIT))
        g->log(g, "rebar note: CAP bit13 (8 GiB offered) not visible yet; "
                  "check lspci Resizable-BAR sizes after boot\n");
    if (fail) {
        WR(g, XVE_CFG_OFF, cfg);
        WR(g, XVE_RBAR_CTL_OFF, ctl);
        WR(g, XVE_CYA_OFF, cya);
        g->log(g, "rebar: readback mismatch, reverted to stock "
                  "(BAR1 stays 64 MiB; harmless)\n");
        return -1;
    }
    g->log(g, "rebar: XVE aperture unlocked to 8 GiB; OS resize follows\n");
    return 0;
}

int do_permissive(struct gpu_dev *g, int mode)
{
    if (!g || !g->mmio)
        return -1;

    switch (mode) {
    case NVPERM_MODE_GR_INSPECT: {
        uint32_t boot0 = RD(g, 0x00000000U);
        uint32_t xp3g = RD(g, XP3G_PLM_OFF);
        uint32_t fecs_plm = RD(g, FECS_PLM_OFF);
        uint32_t ss0 = RD(g, FECS_SS0_OFF);
        uint32_t ss0_rd = RD(g, FECS_SS0_RD_OFF);
        uint32_t ss1 = RD(g, FECS_SS1_OFF);
        uint32_t disp = RD(g, FUSE_DISP_OFF);
        /* v29.66: EFI-phase PDISP probe - never measured pre-GSP before */
        uint32_t pdisp0 = RD(g, 0x00600000U);
        uint32_t pdisp1 = RD(g, 0x00610000U);
        uint32_t pd_disp = RD(g, 0x00419B54U);
        g->log(g, "40HX inspect: BOOT0=0x%08x XP3G=0x%08x FECS_PLM=0x%08x\n",
               boot0, xp3g, fecs_plm);
        g->log(g, "40HX inspect: SS0=0x%08x SS0_RD=0x%08x SS1=0x%08x FUSE_DISP=0x%08x\n",
               ss0, ss0_rd, ss1, disp);
        g->log(g, "40HX PDISP-EFI: 0x600000=0x%08x 0x610000=0x%08x 0x419B54=0x%08x\n",
               pdisp0, pdisp1, pd_disp);
        return 0;
    }
    case NVPERM_MODE_REBAR:
        return rebar_unlock(g);
    default:
        return -1;
    }
}