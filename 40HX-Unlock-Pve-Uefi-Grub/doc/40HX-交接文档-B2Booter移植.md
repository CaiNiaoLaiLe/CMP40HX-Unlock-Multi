# B2 DIRECT_SEC2 Booter 移植 —— 交接文档

> 用途：任务暂停、上下文可能被清空后，凭本文档无损续接。
> 最后更新：2026-11-09（tranche-4 编码已完成，待 PVE 实机验证）
> 蓝本（只读参考，勿改）：`40HX-Unlock-Drive-Patch/windows/tools/unlock40x/unlock40x_v70.c`（8882 行）
> 目标工程：`40HX-Unlock-Pve-Uefi-Grub`（手搓 UEFI GRUB 框架，PVE 实机 `~/CMP40HX-Unlock-Multi/40HX-Unlock-Pve-Uefi-Grub`）

---

## 0. 一句话现状

把 v70 生产 DIRECT_SEC2 算力解锁流程逐段移植进 UEFI 框架，**增量交付 tranche 1→2a→2b→3→4→5**。**tranche 1/2a/2b 已实机 `make release` 通过（FULL=396567）；tranche-3 + tranche-4 编码已全部完成、GetProblems 全绿，但 tranche-3/4 尚未在 PVE 上跑 `make release` 验证**。tranche-4 已把 booter 流程接成 mode 7（`booter`）：新增 `booter_run_b2`（编排 [6]→[7]→[8]→[8b]→[9]→[10]），main.c enum/parse_mode/menu/run_requested 全部接线，Makefile `release` 多出 `NVPermissiveEFI-BOOTER.efi`，install-efi.sh 增第 4 个 GRUB 菜单项。**tranche-5（已编码）**：`REQUEST_BOOTER` 分支在 B2 判定 UNLOCKED 后自动续跑 `apply_gen2`+`rebar`，单趟开机同时拿到算力解锁+Gen2 x16+8GiB BAR1（两步都不发 SBR，不会打回 SS0/SS1；未解锁则跳过）。**下一步只剩 §3.0 的 PVE `make release` 编译验证 + §3.2 的实机首跑定位**。除被 mode 7 显式触发外，其余 booter 函数仍是 dead code；未选 booter 菜单时运行行为不变、不触碰硬件。

---

## 1. 我在做什么（任务定义）

**B2 算力全量使能**：v70 在 Windows 侧用一段 UEFI 应用 `unlock40x_v70.c` 的 `efi_main`（生产是文件里**第二个** `efi_main`，约 L8156 起），在 40HX（TU106，PCIe 算力卡）上通过 SEC2/GSP Falcon 装载签名 booter，解锁 PLM/SS0/SS1 算力限制。本任务把这套流程移植进本框架的 GRUB 阶段 EFI stub。

**核心流程（v70 生产顺序，移植蓝本）**：
```
[6] 分配+填 v67Phys(0xFA00) / ucodePhys(0xE700) / blPhys(0x6000)   ← host sysmem, >4GB 身份映射
[7] u40x_build_dummy_fw(0x1bfadc8 全零) + u40x_build_radix(radix-3 页表)
    → alloc wprMetaPhys(256B) + build_wpr_meta(fbSize=8GiB)
[8] gsp_engine_reset(GSP)  —— 杀掉 POST/VBIOS 起的 GFW
[8b] alloc fwsecPhys(FW40_BLOB_SIZE=0x9DF0)+填 fwsec_40hx_prod
     → fwsec_boot_gsp_40hx(fwsecPhys)  —— WITH_LOADER HS-FWSEC，闩锁 WPR2（特权孔径）
[9] re-copy ucode + SIG_PROD(16B) patch @image+0x8700
     → booter_load_tu102_direct(wprMetaPhys, ucodePhys)  —— BOOT_DIRECT 端口装载 booter
[10] 读 REG_FEAT_OVR_PLM / SM_SPD / SM_SPD_1 判定是否解锁
```

**远程验证方式**：实机无屏幕，靠 tee 到 `/boot/efi/EFI/UNLOCK/LOGS/BOOTER_TRACE.TXT` 的日志远程定位。`check_unlock.sh` 会自动拉取覆盖。因此**每个 risky MMIO 读之前先打一条 breadcrumb 日志**（对齐 v70 的 "read#N" 模式），挂死时能定位到具体那一行。

---

## 2. 已实现的（tranche 1/2a/2b/3）

### 2.1 文件清单与职责

| 文件 | 状态 | 内容 |
|---|---|---|
| `include/booter_regs.h` | 完成 | SEC2/GSP Falcon + WPR/FEAT 全量寄存器偏移、几何常量。所有值逐字抄自磁盘 v70，带 L 行号注释。 |
| `include/booter_assets.h` | 完成 | 7 个固件 blob（objcopy 内嵌 `.assets` 段）的 extern 声明。长度用 `(_end - _start)` 算。 |
| `include/booter_wprmeta.h` | **本轮新增** | `GspFwWprMeta` 结构（逐字复制 v70 L664-716，256B）+ WPR/radix/dummy 几何常量。 |
| `include/booter.h` | 完成 | booter 公开 API 声明。 |
| `src/booter.c` | 主体 | 5 个 helper + tu102_direct + fwsec_boot_gsp_40hx + tranche-3 的 4 个 prep 函数 + tranche-4 的 `booter_run_b2` 编排。 |
| `src/main.c` | 框架 | `booter_alloc_high`；`#include "booter.h"`；tranche-4 接线（enum/parse_mode/menu/run_requested 的 `REQUEST_BOOTER`）；tranche-5 让 `REQUEST_BOOTER` 成功后串 `apply_gen2`+rebar 一条龙。 |

### 2.2 booter.c 已有公开函数（`src/booter.c`）

Tranche-1（纯 helper，用 `g->mmio`→BWR/BRD、`g->log`、`g->delay_ms`）：
- `booter_falcon_wait_reset_ready(g, hwcfg2Reg)` — 轮询 HWCFG2[31]。**已修正**：1000×`delay_ms(1)`≈1s（对齐 v70 10000×Stall(100us)）。
- `booter_falcon_wait_scrub_done(g, dmactlReg, hwcfg2Reg, tag)` — DMACTL[2:1]==0 && HWCFG2[12]==0。**已修正**：3000×`delay_ms(1)`≈3s（原先写成无延时快循环最坏仅~30ms→会撞 DEAD5EC1 race）。
- `booter_falcon_dma_transfer` / `booter_u40x_imem_write` / `booter_u40x_dmem_write`

Tranche-2a：
- `booter_gsp_engine_reset(g)`（v70 L1841）、`booter_gsp_dma_transfer`（v70 L1886）
- `booter_load_tu102_direct(g, wprMetaPhys, ucodePhys)`（v70 L2980）— 两次尝试（A 无 reset、B engine reset），port 写 IMEM(NS 0x100+SEC 0x8400)+DMEM(0x6200)，BOOTVEC=0，mailbox=wprMetaPhys 64 位直传，STARTCPU 后 **hold 3s 完全不读寄存器**（v57 教训），再 sparse 单读（每读前 breadcrumb）。

Tranche-2b：
- `booter_fwsec_boot_gsp_40hx(g, fwsecPhys)`（v70 L2186，WITH_LOADER 全 183 行）— kflcnReset(GSP)→DMEM iface patch(mapper.init_cmd=0x15 FRTS, readVbiosDesc+frtsRegionDesc)→FBIF bit7+TRANSCFG0/4=0x5→BL DESC(0x54B)经端口写 DMEM[0..0x54]→generic BL(gsp_bl_tu102 前 0x200)经端口写 IMEM 顶(BOOTVEC=0xFD00)→STARTCPU→poll WPR2 up 5000×delay_ms(1)=5s，500ms 打诊断。cpuctl 0xBADF 判 GSP 锁死→abort。

Tranche-3（本轮，`src/booter.c` 末尾 +241 行）：
- `static void booter_build_wpr_meta(...)`（v70 L4871）— 纯公式填 meta；`fbSize==0x280000000?0x7F00000:0x6900000` 分支（生产 8GiB 走后者）。**丢弃** stage-only 的 cmp40_stockSig / CPU_RM_HANDOFF 分支。
- `static int booter_build_dummy_fw(...)`（v70 L7410）— alloc `DUMMY_FW_SIZE=0x1bfadc8` 全零。
- `static int booter_build_radix(...)`（v70 L7434）— radix-3 root(tab[0])→L1(tab+0x1000)→L2(tab+0x2000)→data(tab+ptSize)，`nData=(sz+FFF)>>12, nL2=((nData-1)>>9)+1, ptSize=(2+nL2)<<12`。逐条对齐 v70 偏移。
- `int booter_prepare_b2(g, *wprMetaPhys, *ucodePhys, *fwsecPhys)` — 编排 [6]→[7]→[8b]，返回三个 loader 需要的物理地址；v67/ucode/bl 拷贝用 `min(资产LEN,目标SIZE)` 钳制+补零防越界；ucode 拷完打 16B SIG_PROD @+0x8700。

### 2.3 main.c 本轮新增

- `#include "booter.h"`（顶部）
- `UINT64 booter_alloc_high(UINTN pages)`（在 `dma_alloc_below_4g` 与 `dma_free` 之间，+51 行）— 移植 v70 `alloc_fwsec_buffer`（L1245）：`static uint64_t booter_high_cursor = 0x110000000`，游标 16MB 步进试 12 槽 → 静态候选 {0x1A0000000,0x1C0000000,0x1E0000000} → 最后才 <4G 回退；`AllocatePages(AllocateAddress, EfiReservedMemoryType, pages, &addr)`；返回物理地址（身份映射可直接解引用）或失败 0。**放 main.c 是因为 `g_boot_services` 是 main.c 的 static**。

### 2.4 关键常量（磁盘核实值，勿信摘要）

```
V67_SIZE=0xFA00(63488, 非 64000!)  BOOTER_UCODE_SIZE=0xE700  BOOTER_SIG_PATCH_LOC=0x8700
GSP_RM_BOOT_SIZE=0x6000  FW40_BLOB_SIZE=0x9DF0  WPR_META_SIZE=256
GSP_FW_WPR_META_MAGIC=0xdc3aae21371a60b3  REVISION=1
DUMMY_FW_SIZE=0x1bfadc8  B2_FB_SIZE=0x200000000  HIGH_SLOT_BASE=0x110000000
SIG_PROD[16] = 32 60 43 67 98 89 7b 06 e8 5f 1a a8 71 43 5d cb  (v70 L8710-8713)
SEC2_RM_CHIPID0=0x166000a1   WPR2_RAISE lo=0x01ffe000 hi=0x01ffee00
FW40_*: DATA_OFF=0x9A00 MAPPER=0x360 CMDIN=0x3B0 IFACE=0xE0 DMEM_SIZE=0x3F0
        FRTS_OFFSET=0x1FFE00000 NS_SIZE=0x400 SEC_BASE=0x400 SEC_SIZE=0x9600 CMD_FRTS=0x15
BL_DESC: SIZE=0x54 CTXDMA=0x20 CODEDMA_LO=0x24 HI=0x28 NSOFF=0x2C NSSIZE=0x30
         SECOFF=0x34 SECSIZE=0x38 ENTRY=0x3C DATADMA_LO=0x40 HI=0x44 DATASIZE=0x48
GSP_BL_TU102_CODE_SIZE=0x200 START_TAG=0xFD   WPR2_UP lo=0x01FFE000 hi=0x01FFEE00
```

### 2.5 架构约束（务必遵守）

- **`struct gpu_dev` ABI 冻结**（`core_abi.h`：`platform_data`@112、`sizeof`==120 有 `_Static_assert`）。**绝不能加字段**（会破坏独立编译的 core ELF `core_40hx.c` 的 ABI）。
- **booter.c 与 main.c 链进同一个 PE 镜像**（main.o/elf_loader.o/booter.o 一起 link），所以 booter.c 可直接调 main.c 导出的全局函数（如 `booter_alloc_high`），无需经 gpu_dev。core_40hx.c 才是独立 ELF blob。
- booter.c 用 `BWR(g,off,val)`/`BRD(g,off)`（基于 `g->mmio`）、`g->log(g,...)`（SYSV 变参，同时 tee 到 trace）、`g->delay_ms(ms)`、`WBINVD()` 内联汇编。
- memcpy/memset 是 main.c 里逐字节实现（`SYSVABI`），booter.c 顶部用 `extern void *SYSVABI memcpy(...)` 声明（与 elf_loader.c 同款）。
- `SYSVABI=__attribute__((sysv_abi))`，`EFIAPI=__attribute__((ms_abi))`。UEFI 调用在本框架里**直调**（`g_boot_services->AllocatePages(...)`），不用 `uefi_call_wrapper`。
- CFLAGS：`-std=gnu11 -O2 -Wall -extra -Werror -ffreestanding -fno-builtin -mcmodel=large -mno-red-zone -maccumulate-outgoing-args`。
- 新函数若非 static 则不受 `-Werror=unused-function`；静态辅助只要被 prepare 调用即"已使用"。当前 prep 链无调用者→dead code 但编译通过。

---

## 3. 还需要做什么

### 3.0 ⚠️ 立刻要做的第一步：验证 tranche-3 能否编译（用户尚未跑）

```bash
cd ~/CMP40HX-Unlock-Multi/40HX-Unlock-Pve-Uefi-Grub
make clean && make release
ls -l NVPermissiveEFI-FULL.efi
```
预期：`cc -c src/booter.c`、`src/main.c` 无 error/warning；三镜像 `EFI OK`；FULL 较 396567 增约 +3~6KB。若报 `-Wcomment`/类型/`_Static_assert` 问题，按下面坑清单排查。

### 3.1 tranche-4：模式接线 + 实机首跑（✅ 编码已完成，仅待实机首跑）

> 下列 1~3 已落盘并 GetProblems 全绿；只剩第 4 步（首次实机）要做。

1. ✅ **mode 7**：`core_abi.h` 已加 `#define NVPERM_MODE_BOOTER 7`（纯宏，不动 `gpu_dev` ABI）；`main.c` 的 `enum requested_mode` 增 `REQUEST_BOOTER`、`parse_mode` 识别 `booter` 词、`interactive_menu` 增 `[4]`、`run_requested` 增 `REQUEST_BOOTER` 分支直调 `booter_run_b2(gpu)`（不经 `core->do_permissive`）。
2. ✅ **接线顺序**（已在新函数 `int booter_run_b2(struct gpu_dev *g)`（`src/booter.c` 末尾 +~70 行，`booter.h` 已声明）里实现，因 `[10]` 的 BWR/BRD 宏只在 booter.c 可见）：
   ```
   [6][7] booter_prepare_b2(g, &wprMetaPhys, &ucodePhys, &fwsecPhys);   // 纯内存，产出三缓冲（含 SIG_PROD）
   [8]    booter_gsp_engine_reset(g);                                   // 紧跟 [8b] 之前，对齐 v70
   [8b]   booter_fwsec_boot_gsp_40hx(g, fwsecPhys);   // 闩 WPR2（失败不阻断，只 log）
   [9]    booter_load_tu102_direct(g, wprMetaPhys, ucodePhys);
   [10]   delay_ms(500) 后逐个 breadcrumb 读 PLM/SS0/SS1 + WPR2 hi/lo；SS0==0x88888888 && SS1==0x8 则 UNLOCKED 返回 0
   ```
   ⚠️ SIG_PROD 由 prepare 内部已打（tranche-3），`[9]` 前无需重拷；`[10]` 判定值与 v70 `is_unlocked()`（L1307-1310）逐字对齐。
3. ✅ **GRUB 菜单入口**：`Makefile` `release` 已增 `AUTO=REQUEST_BOOTER` 一步（产出 `NVPermissiveEFI-BOOTER.efi`）；`install-efi.sh` 已增 `T_BOOTER` 标题、`for M in ... BOOTER`（MFI=NVPermissiveEFI-BOOTER ARG=booter）、产物检查/ESP 拷贝/sha256/已安装探测/expand_title 均含 BOOTER。
4. ✅ **一条龙（tranche-5）**：`run_requested` 的 `REQUEST_BOOTER` 分支在 `booter_run_b2` 返回 0（真·UNLOCKED）后自动续跑 `apply_gen2(gpu)` + `run_one(core, gpu, NVPERM_MODE_REBAR, "rebar", 0)`，单趟开机同时拿到**算力解锁 + Gen2 x16 + 8GiB BAR1**；若 B2 未达 UNLOCKED 则跳过 Gen2/ReBAR、链路和 BAR 维持原状。顺序与 compute 路径一致（gen2→rebar，rebar 排最后），且 `apply_gen2` 只做链路重训练、rebar 走 `reset_after=0`，**两者都不发 SBR**，不会把 B2 刚建立的 SS0/SS1 打回。`interactive_menu` `[4]` 与 `install-efi.sh` 的 `T_BOOTER` 文案已同步为“一条龙”。
5. ⏳ **首次实机**：跑一次→拉 `BOOTER_TRACE.TXT`→定位（见 §3.2）。首跑成功后 `check_unlock.sh` 的 gen2/rebar 两项应从 FAIL 翻成 PASS（同一趟 booter 已顺手做完），无需再单独跑 compute。

### 3.2 tranche-4 高风险点（预期在实机暴露，按概率排序）

1. **>4GB 身份映射能否直接解引用**——本框架/固件页表若未把 >4GB 恒等地映射进 EFI app 地址空间，`(void*)(UINTN)phys` 会 #GP/挂死。**这是 tranche-4 最可能翻车处**。对策：prepare 里对每个高缓冲写读 canary 并 log 物理地址；若首跑挂在 prepare 阶段即坐实此假设，需改为先 `SetVirtualAddressMap`/加映射或用 `EFI_MEMORY_TYPE` 可映射区。
2. **分配总量/游标越界**：dummy(28.9MB)+radix(≈28.9MB+68KB)+若干，游标 16MB 步进，累计到 >0x130000000；确认机器内存足够且 AllocateAddress 成功（log 每个 alloc 返回地址，0=失败）。
3. **scrub-wait / STARTCPU hold 时序**：务必保持 delay_ms 间隔版本，勿退回快循环。
4. **WPR2 主机侧写可能被锁**（tu102 里 raise WPR2 那步仅观测，不阻断）。
5. **gsp_engine_reset 与 fwsec 的顺序**：reset 必须在 fwsec_boot 之前。

### 3.3 安装 + 首跑 + 验收命令（PVE 实机，tranche-4 后续照此走）

> 均在 PVE 实机仓库目录 `~/CMP40HX-Unlock-Multi/40HX-Unlock-Pve-Uefi-Grub` 下执行。脚本经 git 检出为 LF；若从 Windows 用 scp/WinSCP 二进制直传，先 `sed -i 's/\r$//' install-efi.sh uninstall-efi.sh tools/check_unlock.sh`。

```bash
# A) 编译验证（§3.0，产出含 NVPermissiveEFI-BOOTER.efi 的四镜像）
make clean && make release
ls -l NVPermissiveEFI-BOOTER.efi

# B) 安装（写 ESP + 生成 /etc/grub.d/40-cmp40hx + update-grub）
#    --prebuilt 用刚编好的产物；--force 覆盖旧装；--default=booter 把开机默认项设成 booter
sudo sh install-efi.sh --prebuilt --force --default=booter --timeout=5
#   （交互式：直接 `sudo sh install-efi.sh`，选 1 编译安装 / 2 预编译安装）

# C) 首跑：reboot → GRUB 选 "... (booter, ...)" 项（或等默认 5s 自动进）
#    EFI 阶段 tee 日志到 /boot/efi/EFI/UNLOCK/LOGS/BOOTER_TRACE.TXT；跑完自动接回 PVE 内核

# D) 进系统后验收（pull_trace 会自动拉 BOOTER_TRACE.TXT 到 logs/）
bash tools/check_unlock.sh          # 快检：新增 booter 项 = trace 里程碑判定
sudo bash tools/check_unlock.sh     # 附加 SS0/SS1 实读值（解锁应为 0x88888888/0x8）
bash tools/check_unlock.sh --diag   # 选 6) booter 看完整 trace 断点定位

# E) 应急：设坏了 / 首跑挂死 → 三级回退
#    1) 改 GRUB_DEFAULT 回普通项   2) sh uninstall-efi.sh   3) 断电
```

**booter 项判读口径**（`check_unlock.sh` 已内置）：`b2: prepare FAILED` → §3.2 第1条（>4G 映射翻车）；`fwsec -> -1` → WPR2 未闩（观测不阻断）；`mbox0!=0` → booter 未 OK；无 `b2: RESULT` 且尾部停在某条 `b2: [10] read ...` → 硬挂在那个寄存器读，对照 breadcrumb 定位。

### 3.4 收尾（可选，实机成功后）

- 把 tranche-2/3/4 的 on-hardware 结果补记到 `40HX-破解-UEFI GRUB方案-总结.md`。
- 更新 `CHECKSUMS.sha256`（`make release` 已自动 `sha256sum >`）。

---

## 4. 血泪坑清单（每条都真实踩过，续接前先读）

1. **摘要会杜撰符号名**：lossy summary 里的 `gsp_falcon_dma_write`/`NV_GSP_FALCON2_SEC_CTRL`/`haltdone`/`reset_halt`/`falcon_start` 磁盘上**根本不存在**，照抄必编译失败。**移植前一律用磁盘字节核对**。
2. **中文路径下 Grep/findstr 不可靠**：用 PowerShell `Get-Content -LiteralPath $p` 读全量 + `($c)[a..b]` 切片 dump 行范围 + `Select-String -SimpleMatch` 验 token 存在性。行号从 1 起，切片用 0 基（`$c[8486..8600]` = L8487..8601）。
3. **注释里别出现 `/*` 序列**（如 `unlock40x/*.bin`）→ `-Werror=comment` 编译失败。booter_assets.h L7 已因此改为 `unlock40x  (v70 .bin files)`。写路径/通配符注释时注意。
4. **scrub-wait/reset_ready 必须带延时间隔**（见 §2.2），否则最坏几十 ms 就放弃→引擎撞 scrubber→DEAD5EC1 全覆写。
5. **risky MMIO 读前先打 breadcrumb**，挂死才能在 trace 里定位到具体寄存器读。

---

## 5. 磁盘参考行号速查（v70 `unlock40x_v70.c`）

| 目标 | v70 行号 |
|---|---|
| `GspFwWprMeta` 结构 | L659-716 |
| radix/WPR_META/FW40/BL_DESC 常量 | L642-654, L719-722, L136-165(见 booter_regs 注释) |
| `alloc_fwsec_buffer` / `alloc_below_4g` | L1245 / L1231 |
| `falcon_wait_scrub_done` / `wait_reset_ready` | L1739 / L1758 |
| `gsp_engine_reset` | L1841 |
| `gsp_dma_transfer` | L1886 |
| `fwsec_boot_gsp_40hx` | L2186 |
| `booter_load_tu102_direct` | L2980 |
| `build_wpr_meta` | L4871 |
| `u40x_build_dummy_fw` / `u40x_build_radix` | L7410 / L7434 |
| 生产 `efi_main` 第二份（[6]-[9] 编排 + SIG_PROD） | L8156 起；[6]L8488 [7]L8555 [8b]L8657 [9]L8704-8719 |

---

## 6. 续接清单（下次开工照这个走）

1. 读本文档 §2.5 架构约束 + §4 坑清单。
2. 跑 §3.0 验证 tranche-3 **与 tranche-4** 编译（`make clean && make release` 现在会多出 `NVPermissiveEFI-BOOTER.efi`）；有错按 §4 修。
3. ✅ tranche-4 接线已完成（见 §3.1 1~3）；无需再写代码，除非编译报错。
4. 编译过 → 按 §3.3 命令链走：`install-efi.sh --default=booter` 安装 → 首跑 → `check_unlock.sh`（已内置 booter trace 判读项）。
5. 实机首跑盯 §3.2 第 1 条（>4GB 身份映射）；挂 prepare 就查映射（check_unlock booter 项会直接报 `prepare FAILED`），挂 loader 就看 breadcrumb。
6. 全程用磁盘字节核对，不信摘要/文档估计。
