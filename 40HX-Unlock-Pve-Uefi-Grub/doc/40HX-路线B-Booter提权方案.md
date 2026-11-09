# 路线 B：UEFI 阶段 Booter 提权解锁算力 —— 方案执行文档

> 状态：规划完成，待分步实现。
> 目标读者：本项目维护者。本文是"怎么做"的单一权威来源；背景与踩坑见
> `40HX-破解-UEFI GRUB方案-总结.md` 与 `40HX-Gen2链路问题说明.md`。

---

## 0. 一句话结论

现有 `NVPermissiveEFI.efi` 只做普通 MMIO 写，**写不进 `SS0/SS1`（算力速度选择子）**，
因为这两个寄存器受 SEC2/Falcon 特权保护。路线 B = 把上游 `unlock40x_v70.c`
里那套**久经考验的 Booter 提权流程**（SEC2 booter V67 / FWSEC / WPR2 / RISC-V）
在 PVE 上用 gnu-efi 原生编成一个 `.efi`，接到现有 GRUB `chainloader` 菜单里，
用它的 `--return-to-grub` handoff 交回 PVE 引导。**不重新实现 Booter。**

---

## 1. 为什么必须走 Booter（已用实测钉死）

同一台机器、同一次开机，UEFI 交棒值与进系统值逐字节对比：

| 寄存器 | UEFI 交棒（屏幕 selector probe） | 进系统（`tools/read_bar0.py`） | 代码想写的值 | 判定 |
|---|---|---|---|---|
| `SS0` (0x409664) | `0x00110000` | `0x00110000` | `0x88888888` | ❌ 写被丢弃 |
| `SS1` (0x40966C) | `0x00000006` | `0x00000006` | `0x00000008` | ❌ 写被丢弃 |
| `FECS_PLM` (0x409650) | `0xffffff8f` | `0xFFFFFF8F` | `0xFFFFFFFF` | ⚠️ bit4/5/6 恒 0 |
| `XP3G_PLM` (0x8E1B0) | `0xffffff8f` | `0xFFFFFF8F` | `0xFFFFFFFF` | ⚠️ 同上 |
| `WPR_ADDR` | — | `0x01FFEE00:0x01F7E000` | 下放 `0x1FFFFE00/0` | ❌ 未下放 |
| `PDISP 0x600000` | `0xBADF1100` | — | — | 区域不可达标记 |

三条硬证据互相咬合：

1. **交棒值 == 进系统值**：GSP/驱动**没有**回滚 UEFI 的写入。之前"活不到驱动加载"的假设被推翻。
2. **`SS0/SS1` 写入完全无效**（写什么都不粘）：这是**权限层**问题，不是时序、不是值、不是 SBR。
3. **`FECS_PLM` 的 bit4/5/6 恒为 0 + `WPR` 未下放 + `PDISP` 返回 `0xBADF`**：MMIO 拿不到 Falcon 特权，
   只有 SEC2 内部的 Falcon（Booter 上下文）能置这些位。

> 上游驱动方案 `docs/how-it-works.md` 的原话现获得本机实测背书：
> "FECS 寄存器受 SEC2 保护 → 写完不回读、或值不变 → 必须走 Booter 特权上下文。"

**附带结论（不影响本路线，但记录）：** 本次 Gen2 是靠 retrain round 2（CCC=1）命中的，
上次是 round 1（CCC=0）。说明"两端 CCC 统一为 0"不是普适值，命中哪轮随上电时钟状态变化；
现有两轮兜底写法是对的，保持不变。

---

## 2. 策略决策：移植集成，而非重写

| 方案 | 说明 | 取舍 |
|---|---|---|
| **B1（选定）** | 直接编译上游 `unlock40x_v70.c` 的 DIRECT_SEC2 主线，产出 `unlock40x_v70.efi`，接进 GRUB | Booter 逻辑已被 Windows/Linux 双端验证；改动最小；风险集中在"集成"而非"提权正确性" |
| B2 | 把 Booter 流程（SEC2 booter、FWSEC、WPR2、V67 payload）逐段移植进本项目 `core_40hx.c` | 8883 行、几十轮研究迭代（v2.10→v2.100）的重写，极易引入提权失败/硬挂死；工作量与风险都不可接受 |
| B3 | 走 `linux/cpu-rm` 内核模块（GSP OFF） | 需 `NVreg_EnableGpuFirmware=0`，与 PVE vGPU 常用 GSP 冲突；且是"驱动侧"而非 UEFI，偏离既定边界 |

**B1 的三个有利事实：**

1. `build_v70.sh` 支持 **Linux 原生 ELF → EFI PE**（gnu-efi），无需 Windows/MSYS2。
2. `efi_main`（L8238 起，DIRECT_SEC2 主线）**已内建** `--return-to-grub` / `--return-to-bootloader`：
   命中时**返回 `EFI_SUCCESS` 交回父引导管理器**，不 chainload Windows —— 正是 GRUB `chainloader+boot` 需要的语义。
3. 生产主线 `CPU_RM_CONTROL=full` → `STAGE_CONTROL=9`（`-DCPU_RM_HANDOFF -DHANDOFF_PRELOAD_DEFAULT`），
   是"已测试的 CPU-RM handoff 出货路径"。

---

## 3. 工具链与资产前提

### 3.1 编译依赖（PVE 上需先装齐）

`build_v70.sh` 在 Linux 分支要求：

- `gcc`、`objcopy`、`objdump`、`ld.bfd`（GNU binutils，脚本显式拒绝非 GNU ld）
- **gnu-efi** 开发包，提供：
  - 头文件 `/usr/include/efi`、`/usr/include/efi/x86_64`
  - `/usr/lib/libefi.a`、`/usr/lib/libgnuefi.a`
  - `/usr/lib/crt0-efi-x86_64.o`、`/usr/lib/elf_x86_64_efi.lds`
  - Debian/PVE 包名：`apt install gnu-efi binutils gcc python3`
- `python3`（构建前会跑 4 个 `verify_*.py` 资产校验，任一失败即中止）

> 与本项目现有 `Makefile` 不同：现有 `NVPermissiveEFI` 是手搓 `-mcmodel=large` + 自定义 ELF loader；
> v70 走标准 gnu-efi 运行时（`InitializeLib`/`Print`/`uefi_call_wrapper`）。两者互不依赖，路线 B **新增**一条独立构建，不动现有 Makefile。

### 3.2 二进制资产（11 个 .bin，已随上游仓库存在）

`build_v70.sh` 第 2 步 `objcopy -I binary` 嵌入：

| 文件 | 大小 | 状态 | DIRECT_SEC2/TU106 是否真正使用 |
|---|---|---|---|
| `v67_payload.bin` | 64000 | 真 | ✅ 选择子 payload（MIT，与 cpu-rm 同源） |
| `booter_ucode_prod.bin` | 59136 | 真 | ✅ SEC2 booter ucode（生产） |
| `fwsec_40hx_prod.bin` | 40432 | 真 | ✅ TU106 FWSEC（生产） |
| `bl_gsp_tu102.bin` | 768 | 真 | ✅ GSP bootloader |
| `booter_ucode_dbg.bin` | 59136 | 全零 | 调试占位（RELEASE_BUILD 不走） |
| `fwsec_40hx_dbg.bin` | 40432 | 真 | 调试变体 |
| `gsp_rm_boot_dbg.bin` | 131072 | 全零 | 调试占位 |
| `fwsec_ga102.bin` | 59904 | 全零 | GA102 用，TU106 路径不消费 |
| `fwsec_ga102_sig.bin` | 2248 | 全零 | 同上 |
| `sec2_ucode_vbios_49.bin` | 16384 | 全零 | 变体占位 |
| `sec2_ucode_vbios_89.bin` | 16384 | 全零 | 变体占位 |

**要点：** 全零的 blob 是占位/别的芯片用的，`build_v70.sh` 仍会 embed 它们（链接期需要符号），
但 DIRECT_SEC2 + RELEASE_BUILD + TU106 主线只真正执行 `*_prod` 那几个。资产已在仓库内，**无需联网 fetch**；
`verify_*.py` 会校验其哈希，若与上游 pin 不符需回报（不擅自替换）。

---

## 4. GRUB 集成的关键难点：LoadOptions 传不进去

**现状（本项目已知的坑）：** GRUB 的 `chainloader` 命令加载 EFI 时**不会设置 `LoadedImage->LoadOptions`**。
本项目现有 Makefile 正是因此用编译期 `AUTO=REQUEST_COMPUTE` 把模式烘进 .efi。

v70 的 `--return-to-grub` 是通过 `u40x_has_load_option(ImageHandle, ...)` 读 LoadOptions 判断的，
GRUB 下这条路**读不到参数**。因此需要一个"无参数时即走 return-to-grub + compute"的默认。

### 4.1 三种解法（择一，推荐 M-a）

| 方法 | 做法 | 评价 |
|---|---|---|
| **M-a（推荐）** | 给 v70 加**一个**极小编译开关 `-DU40X_DEFAULT_RETURN_TO_GRUB`，在 `efi_main` 里当 `returnToBootloader` 判定为 FALSE 且无 LoadOptions 时，默认置 `returnToBootloader=TRUE` 并强制 compute handoff 路径 | 单点、可控、不改提权逻辑；与本项目"编译期烘默认"哲学一致 |
| M-b | 让 v70 从 ESP 读一个策略文件（它已有 `u40x_load_rebar_policy` 读 ESP 的机制）来决定 handoff | 复用现成机制，但要新增文件解析分支，改动面比 M-a 大 |
| M-c | 换引导方式：不用 GRUB chainloader，改用 systemd-boot / rEFInd 的 `.conf` 传 args | 引入新引导栈，偏离"PVE + GRUB"既定环境，排除 |

> **已用源码确认（不再是假设）：**
> - `STAGE_CONTROL==9`（即 `CPU_RM_CONTROL=full` 的出货 stage）的收尾 `u40x_finish_stage`
>   在 L7213-7216 打印 `SS=0x88888888/8 WPR2=down` —— 该 stage 就是算力全解锁态。
> - L7241-7244：`if (u40x_cpu_rm_return) return EFI_SUCCESS;` 是 return-to-grub 交回点，
>   位置在 Booter 完成、SS0/SS1 落盘**之后**。
> - L8277-8279 那个 `|| returnToBootloader` 的 abort 被 `#ifndef CPU_RM_HANDOFF` 包着；
>   build 定义了 `CPU_RM_HANDOFF`，故 return-to-grub 合法。
> - L354：`u40x_handoff_mode` 无参数时默认返回 `U40X_HANDOFF_PRELOAD`（正是测试过的默认）。

> **M-a 为什么是必需而非可选（反证）：** 若 `returnToBootloader=FALSE`（GRUB 不传参的默认），
> 则 `u40x_cpu_rm_return=FALSE`，L7242 的 `return EFI_SUCCESS` 被跳过，落到
> `BS->StartImage(win,...)` 而 `win=NULL` → 失败。所以无参数时必须把 return-to-grub 强制为真，
> 交回才能成立。改动点单一：在 L8266-8269 的 `returnToBootloader` 赋值后追加
> "`#ifdef U40X_DEFAULT_RETURN_TO_GRUB` 且无 `--handoff-*`/`--return-*` 显式参数 → 置 TRUE"。

---

## 5. 分步实现里程碑

> 每个里程碑都有"完成判据"，未过不进下一步。M0/M1 不改代码；改代码从 M2 起。

### M0 —— 编译链与资产自检（PVE 上）
```sh
apt-get install -y build-essential gnu-efi python3
# 确认 gnu-efi 关键文件存在
ls /usr/include/efi/efi.h /usr/lib/crt0-efi-x86_64.o /usr/lib/elf_x86_64_efi.lds \
   /usr/lib/libgnuefi.a /usr/lib/libefi.a
ld --version | head -1   # 必须是 GNU ld (BFD)
```
**判据：** 上述文件齐全、`ld` 为 GNU BFD。缺 gnu-efi 就补装，不进 M1。

### M1 —— 原样编出 v70（不改一行代码）
```sh
cd <上游>/windows/tools/unlock40x
CPU_RM_CONTROL=full bash build_v70.sh      # 期望末尾 "EFI build OK: .../unlock40x_v70.efi"
```
**判据：** 4 个 `verify_*.py` 全过；`objdump -p` 显示 `EFI application` 且含 `.reloc`、`.rodata`。
产物 `unlock40x_v70.efi` 先存档备用。**此步只证明"能编"，不代表算力已解。**

> **PVE 实测修正（已完成）：** 上游那条"必须有独立 `.rodata` 段"的校验在 PVE 的 gnu-efi
> 上会**误报** `Missing read-only data section`。诊断（`tools/routeB_diag.sh`）证实：
> gnu-efi 的 `elf_x86_64_efi.lds` 把 `*(.rodata*)` 并进 `.data` 输出段，`linked.so`/`out.efi`
> 的 `.data` 段（`0x7eb28`≈518 KiB，含全部只读数据 + 11 个嵌入 blob）完好，产物本身是对的。
> 已放宽 `build_v70.sh` 的该校验为"`.rodata` 或 `.data` 任一存在即可"，保留非空 guard。
> 放宽后重跑 `CPU_RM_CONTROL=full bash build_v70.sh` 应出现 `EFI build OK`。

### M2 —— 加 return-to-grub 默认（M-a 方案）
1. 读 `u40x_has_load_option` / `u40x_handoff_mode` / STAGE_CONTROL==9 段（L8274-8286），确认默认点。
2. 在 `build_v70.sh` 的 `extra_defs` 追加 `-DU40X_DEFAULT_RETURN_TO_GRUB`。
3. 在 `efi_main` 的 `returnToBootloader` 赋值处，加"无 LoadOptions 且该宏定义 → 默认 TRUE + compute"的最小分支。
4. 重编，确认 banner 打印 `load option: return-to-bootloader`（无参数即出现）。
**判据：** 不带任何参数启动该 .efi，即走 return-to-grub + CPU-RM compute handoff，最终返回 EFI_SUCCESS。

> **已完成（源码定位 + 两处修改）：**
> - STAGE_CONTROL==9 的真实终点是 L8755 `return u40x_finish_stage(...)`（不会走到普通
>   的 L8868 returnToBootloader 分支）；Booter 在 L8719 `booter_load_tu102_direct` 无条件执行。
> - `u40x_finish_stage` 里：L7159 `if (!win && !u40x_cpu_rm_return) abort`——GRUB 下 win=NULL，
>   必须 `u40x_cpu_rm_return=TRUE` 才不 abort；L7182 `u40x_stage_check_booter` 只读校验
>   `SS0==0x88888888 && SS1==8`；L7241 `if (u40x_cpu_rm_return) return EFI_SUCCESS` 是交回点。
> - `u40x_cpu_rm_return = returnToBootloader`（L8283），故只需在无参数时把 `returnToBootloader`
>   强制为 TRUE。已改：`unlock40x_v70.c` L8266-8269 之后插 `#ifdef U40X_DEFAULT_RETURN_TO_GRUB`
>   默认分支；`build_v70.sh` 在 `extra_defs` 后追加该宏（`DEFAULT_RETURN_TO_GRUB=0` 可关）。
> - 副作用已逐条验证：强制 TRUE 仅（1）L8388 跳过 Windows 准备（2）过 L7159 guard + L7242 交回；
>   Booter 不受影响。forbidden 列表（L7003-7005）在 CPU_RM_HANDOFF 下不含 return-to-grub，不会误 abort。

### M3 —— 接入 install-efi.sh 与 GRUB 菜单
- 新增一个菜单项（或替换现有 compute 项的后端）：`chainloader` 指向 `unlock40x_v70.efi`，`boot`。
- 拷贝产物到 ESP `/boot/efi/EFI/UNLOCK/`（沿用现有目录约定）。
- 保持现有 inspect/full 项不动，路线 B 先只挂一个"compute-booter"实验项。
**判据：** `grub-mkconfig` 无报错，菜单出现新项，选中后能进 v70 banner。

> **已完成（`install-efi.sh` 五处改动，`sh -n` 语法自检通过）：**
> - 采用**可选第四项**设计，对现有三项零干扰：`MENU_SET="INSPECT COMPUTE FULL"` 为默认，
>   只有传 `--booter[=<路径>]`（或 `BOOTER_EFI=<路径>`）才 `MENU_SET="$MENU_SET BOOTER"`。
> - 标题 `T_BOOTER`（L57）、`expand_title` 加 `booter)`（L108）、usage 加 `--booter`（L74-76）、
>   参数解析 `OPT_BOOTER`/`OPT_BOOTER_PATH`（L127-128、L153-156）。
> - 路线 B 源探测块（L173-196）：不带路径时依次探测
>   `../40HX-Unlock-Drive-Patch/.../unlock40x_v70.efi` → `$HOME/.../unlock40x_v70.efi` → 仓库内 `CMP40HX-BOOTER.efi`；
>   找不到即报错并给出手动 build 命令，绝不静默装旧文件。
> - ESP 拷贝（L330-334）：`install -m 0755 "$BOOTER_SRC" "$ESP/EFI/UNLOCK/CMP40HX-BOOTER.efi"`。
> - 菜单生成（L356-362）：`BOOTER` 分支 `MFI=CMP40HX-BOOTER`、`ARG` 为空（靠编译期
>   `U40X_DEFAULT_RETURN_TO_GRUB` 定 return-to-grub + compute，与现有项"文件名选路"哲学一致）。
> - 卸载对称：`uninstall-efi.sh` 的 `rm -rf "$ESP/EFI/UNLOCK"` 已覆盖 `CMP40HX-BOOTER.efi`，无需改。
> - **本项只做 Booter 算力，不做 Gen2**（v70 stage 9 不含 Gen2 策略）；compute-booter + Gen2 的
>   合并留作 M4 之后的独立问题（见 §6）。
>
> **PVE 实测通过（M3 完成判据全中）：** `sudo sh install-efi.sh --prebuilt --force --booter` 后
> `grub.cfg` 含 **4** 个 `PVE + CMP40HX UNLOCK` 项；ESP 内 `CMP40HX-BOOTER.efi`（585430 字节）存在；
> `compute-booter` 项的 `chainloader` 行不带参数；Booter 源自动探测命中同仓目录下的 v70 产物。
> 现有三项 sha256 不变（零干扰）。

### M4 —— 实机验证算力（决定性）
```sh
# 重启选新菜单项，进系统后：
sudo python3 tools/read_bar0.py         # 期望 SS0=0x88888888 SS1=0x00000008 verdict=YES
# 独立负载复核（不能只看寄存器）：
clpeak --kernel-run-duration 10 | grep -iE 'fp16|int8|wmma'   # WMMA fp16 应从 ~1 跳到几十 TFLOPS
```
**判据：** SS0 粘住 `0x88888888` **且** clpeak WMMA/INT8 实测上台阶。二者缺一，回到 §6 排查。

> **M4 首测现象（GRUB 选中 compute-booter）：硬挂死，需断电重启。**
> 屏幕停在 GRUB 自己的 `echo "NVPermissiveEFI: BOOTER ..."` 之后，**v70 一行自己的输出都没有**
> （连 `efi_main` 第一句 `Print(L"=== CMP40HX Unlock v70-40HX ...")`（L8259）都没出现）。
> 即 **v70 在打出首行输出前就挂死**，挂在 `InitializeLib`/首次 `Print`，根本没走到
> `u40x_find_gpu`/BAR0/Booter。与同菜单里能正常刷屏+解 Gen2 的手搓 `NVPermissiveEFI.efi`
> （不用 gnu-efi 运行时）形成对比。
> 待区分：**(A)** gnu-efi 镜像在 GRUB chainloader 下起不来（入口/控制台握手）；
> **(B)** v70 在本主板本身就起不来。用 `efibootmgr` 直接固件启动（绕开 GRUB）劈开二者。
>
> **固件直启实测（efibootmgr -c 建 BOOTER-DIAG 项，不经 GRUB）：仍然黑屏挂死、零输出。**
> → **(A) 排除**：锅不在 GRUB chainloader，v70 被固件直接 `StartImage` 也在首行 `Print` 前挂死。
> → 落在 **(B)**：v70 镜像在本主板起不来。**但先排一个廉价真凶候选**：
>   `build_v70.sh` 末尾 `objcopy -O efi-app-x86_64` 在新版 binutils 上可能产出**基址重定位表
>   (`.reloc`) 损坏的 PE**，固件加载后跳进错误入口→“一上来就挂、零输出”。用 `objdump -p/-h`
>   查 Subsystem 与 .reloc 完整性区分“构建缺陷(可修)” vs “运行时不兼容(路线 B 到头)”。
>
> **镜像完整性结论：产物完好，非构建缺陷。** `Subsystem=0x0a (EFI application)`、`.reloc` 存在
> (两条 `ABSOLUTE offset 0`——PIC 程序的正常形态，不是损坏)、`.text/.data` 齐全、binutils 2.40。
> → 排除“objcopy 坏重定位表”假设，挂死属**运行时**。
>
> **最小 gnu-efi hello 实测（`tools/routeB_hellotest.sh`，同 crt0/lds/objcopy 配方）：也黑屏挂死。**
> → **定论：问题特定于 gnu-efi 运行时(crt0/InitializeLib/首次 Print)在本固件上起不来**，
>   非 v70 代码、非 GRUB。对照：手搓非-gnu-efi 的 `NVPermissiveEFI.efi` 同板能刷屏+解 Gen2，
>   证明固件本身能跑 EFI app。(残留小概率假阴性：hello 入口 ABI 桥接与 v70 略有差异；但 v70
>   自身已在首次 Print 前零输出挂死，独立坐实 gnu-efi 路径不通。)
>
> **路线 B（UEFI Booter via v70-EFI）在本板判定为不可行。** 剩余 UEFI 方案只有 B2(把 Booter 重
> 写进手搓非-gnu-efi 框架)，8900 行提权逻辑盲改风险极高且可能踩同一坑。算力 SS0/SS1 的
> Falcon 特权解锁改走 **OS 侧 cpu-rm 内核模块**(需先确认部署形态与 GSP 可否关闭)。

> **后续核实（推翻“拿现成 PE 版走捷径”设想）：** 出货的 `windows/tools/inst40hx/embed/40HXUNLK.EFI`
> 段表含 `.dynamic/.rela/.dynsym/.dynstr`——即它也是 **Linux/gnu-efi 分支编、同样带 crt0**。
> 维护者在 Windows 能跑通是因为他那块板固件能加载 gnu-efi crt0 镜像，本 PVE 板不能。
>
> **手搓框架（`include/uefi_min.h` + `src/main.c`）已具备 Booter 所需原语：** AllocatePages(含
> AllocateMaxAddress)/AllocatePool、PCI Root Bridge 配置空间读写、BAR0 MMIO rd/rmw、Stall、
> Console、ELF/blob 嵌入。缺的只是 v70 里 SEC2/Falcon/WPR2/V67 那几百行寄存器编排 +
> GetMemoryMap 接线；GPU 侧 payload(v67/ucode/fwsec/bl) 是位置无关码，直接嵌入上传即可。
>
> **全局残留风险（适用所有 UEFI-compute 路径）：** 维护者的 Linux 算力走驱动补丁/cpu-rm，
> UEFI compute 仅在 Windows 验证过。SS0/SS1 在 UEFI Booter 写入后，能否撑过 Linux nvidia
> 驱动(GSP ON)初始化而不被重新锁定，**本板未实测**（Windows 驱动不重锁，Linux 同源 RM 推定也不重锁）。

> **B2 战略校正（2026-11-09，回答“把 Booter 重写进手搓框架可行不”）：**
> 1. **无需再测现成 compute.efi 的算力**——本文件 §1 已用 `read_bar0.py`+selector probe 实测：
>    手搓框架 `core_40hx.c write_full_set()` 的**直接 MMIO 写 SS0/SS1 被丢弃**（想写 `0x88888888`/`0x8`，
>    交棒与进系统都停在 factory `0x00110000`/`0x00000006`）。总结文档 §7.1“算力已验证”只验了
>    `nvidia-smi`/CUDA 可用、未验吞吐，寄存器层已被 §1 证伪。**“测现成 compute.efi”这条捷径不成立。**
> 2. **B2 技术上可行，且是几百行编排、不是 8900 行盲改**：手搓框架能跑（Gen2 为证，非-gnu-efi 不碰挂死的
>    crt0），Booter 所需原语（`AllocatePages(AllocateMaxAddress)`<4G DMA、MMIO、SBR、嵌入 ELF/blob）齐备；
>    只需搬 v70 `booter_load_tu102_direct` 的 DIRECT_SEC2 compute 主线（halt SEC2→载 FWSEC/ucode→跑 v67_payload
>    在 Falcon 特权上下文写 SS0/SS1→返回）+ 直接嵌入 v67/ucode/fwsec 的 `.bin`。
> 3. **命门＝存活风险（上方残留风险的具体化）：** UEFI Booter 就算真把 `0x88888888` 写进去了，Linux
>    GSP-on 驱动 probe 时会不会重设回 factory？§1 的“交棒==进系统”回答不了（因为那步根本没写进去）。
>    cmpunlocker 在 Linux 故意走 cpu-rm(GSP OFF) 而非 UEFI，本身就是“Linux RM 可能重锁”的强信号。
>    造“SS0 已被成功设置”前提来测存活，MMIO 做不到、cpu-rm 被“GSP 不能关”堵死、Windows 断电即失——
>    **唯一能同时“写进去+测活”的手段就是 B2 本身**，故 B2 既是实现也是该前提的决定性实验。
> 4. **开工前的零风险侦察（建议先跑）：** Linux GSP-on 运行中 `read_bar0.py` 记 SS0/SS1 现值 →
>    `rmmod nvidia*` → `modprobe nvidia` → 再 `read_bar0.py`，比对驱动 probe 是否主动改写这些寄存器。
>    纯只读、不动硬件。显示“不动”→ 直接开工 B2；显示“probe 重锁”→ B2 白干，须放宽“GSP 不能关”走 cpu-rm 或接受仅 Gen2。

> **ReBAR 8 GiB 不需要 Booter（2026-11-09 核实）：** 驱动补丁 `0003-cmp40hx-rebar-unlock` 的实现
> 就是**普通内核 MMIO**（`ioremap(BAR0+0x88000)`）：① `XVE_CYA 0x88724=0x30` 解锁；
> ② `XVE_CFG 0x88dcc` selector 7+enable(bit31) 把“可选尺寸表”从 0x400 扩到 0x7fc00；
> ③ 真正定尺寸的是**标准 PCIe Resizable BAR 扩展能力**（cfg 0xBB0，BAR1 Control 在 0xBC0，
> MMIO 镜像 `0x88bc0`，bits[13:8]=编码 log2MiB=13 即 8 GiB）——v70 注释明说只写 XVE_CFG
> 不写 RBAR-CTL 时 BAR1 仍留 64 MiB。Windows EFI 也在 UEFI 阶段完成同一套（v70 [11] 节）。
> → 手搓框架已具备 MMIO rd/rmw + 配置空间读写原语，ReBAR 增量≈几十行，**与 Booter 完全解耦**。
> Linux 侧无需驱动补丁：`NVreg_EnableResizableBar=1`（模块参数，非改驱动）→ 原生驱动
> `nv_resize_pcie_bars`→`pci_resize_resource` 走标准 RBAR 能力完成 BAR 重分配。
> 约束：XVE 写必须放在最后一次 SBR 之后（SBR 清端点 PCI 态）；写入后回读校验、失败全量回退。
> 注意：UEFI 解锁+原生驱动 resize 这个组合上游未验证过（上游 Linux 用的是内核补丁），需实测。
>
> **ReBAR 实现落地（v29.68，待 PVE 实测）：**
> - `core_40hx.c` 新增 `rebar_unlock()`：CYA/CFG/RBAR-CTL 三写 + 回读校验，任何不一致全量回退
>   （失败形态 = ReBAR 没开，无副作用）；CAP bit13 只提醒不卡死。新增全局符号
>   `nvperm_core_version`，屏幕会打印 `40hx core v29.68 (compute+gen2+rebar)` 便于确认新产物已进 ESP。
> - `core_abi.h` 新增 `NVPERM_MODE_REBAR=6`；`main.c` compute 流程在最后一次 SBR 与 Gen2 之后
>   调用（非致命）；full 流程暂未接入（按需再加）。菜单横幅同步 bump 到 v29.68。
> - Makefile 无需改动（core 依赖头文件已覆盖 `core_abi.h`，`make release` 自动重刷 CHECKSUMS）。
> - **上游隔离规则（用户 2026-11-09 要求）：驱动补丁仓库只读不动；此前对它的改动
>   （`build_v70.sh` 的 .rodata 放宽 + `unlock40x_v70.c` 的 `U40X_DEFAULT_RETURN_TO_GRUB`）已
>   `git restore` 回滚，工作区干净；借鉴文件一律复制进本项目 `reference/` 再用。**
>
> **B2（算力）状态：等待存活侦察（rmmod/modprobe 前后 `read_bar0.py` 对比）结果再定生死；
> 侦察显示驱动不重锁 → 出 DIRECT_SEC2 逐行移植规格；重锁 → 摊牌放宽约束。**

### M5 —— Secure Boot 与回滚
- 若 PVE 开了 Secure Boot：v70 未签名会被固件拒绝加载。方案：
  - 短期：验证阶段临时关 Secure Boot；或
  - 正式：用 `sign_efi.sh` / sbsign + MOK 自签（上游有 `sign_efi.sh`，198 行）。
- 回滚：删新菜单项 + `update-grub` 即恢复原状；v70 不改驱动、不写固件，掉电即清硬件解锁态。
**判据：** Secure Boot 状态下能加载（或明确记录"需关闭 SB"），回滚一步到位。

---

## 6. 风险与未决问题（诚实清单）

| 风险 | 影响 | 缓解 |
|---|---|---|
| **v70 的 return-to-grub 在 Linux 侧未经独立验证**（README 明说 Linux 验收走的是 cpu-rm 模块，非 EFI） | handoff 点之前可能没写完 SS0/SS1，或 cleanup 与 PVE 驱动加载时序打架 | M2 用源码确认落盘序列；M4 双判据（寄存器 + clpeak）把关 |
| GRUB 不传 LoadOptions | 无法用命令行切模式 | M-a 编译期默认；一个 .efi 只固定一种行为 |
| **提权路径可能硬挂死**（build_v70.sh 注释：某些锁板对 FEAT_OVR/BAR0 事务会 hard-stall） | 开机卡死需断电 | 只在验证机跑；`DIRECT_WRITE_PROBE`/`PRELOAD_PROBE` 保持关闭（默认即关） |
| Secure Boot 拒绝未签名 EFI | 加载失败 | M5：临时关 SB 或 sbsign 自签 |
| 8 GiB ReBAR / WPR 内存布局与 PVE 现有 Gen2/ReBAR 策略叠加 | 内存压力、与现有 EFI 项冲突 | 路线 B 新项先 `norebar`，只做 compute；Gen2 仍交给现有稳定项 |
| 资产哈希与上游 pin 不符 | verify 失败、构建中止 | 不擅自替换 .bin；回报差异 |
| 与现有 `NVPermissiveEFI` compute 项**同一次开机都跑** | 双重提权互相踩 | 菜单二选一；文档写明"勿在同一 boot 叠加两条 compute" |

---

## 7. 交付物清单（路线 B 完成后）

- `build_v70.sh` 的一行 `extra_defs` 追加（`-DU40X_DEFAULT_RETURN_TO_GRUB`）。
- `unlock40x_v70.c` 中 `efi_main` 的**一处**最小默认分支（无参数 → return-to-grub + compute）。
- `install-efi.sh` 新增"compute-booter"菜单项与产物拷贝逻辑（沿用 ESP `/EFI/UNLOCK/`）。
- `tools/read_bar0.py` 增加对 `0x88888888` 的 verdict 命中（已具备，确认即可）。
- 本文 §5 各里程碑的实测记录（selector probe 截图 + clpeak 数字）补进 `40HX-破解-UEFI GRUB方案-总结.md`。

---

## 8. 与现有文档的关系

- 本文只讲**路线 B（Booter 提权）**的实现。
- Gen2 链路：仍由现有 `NVPermissiveEFI` 的 Gen2 三步负责，见 `40HX-Gen2链路问题说明.md`。
- 背景/寄存器/误判史：见 `40HX-破解-UEFI GRUB方案-总结.md`。
- 操作步骤：待路线 B 稳定后，再在 `40HX-破解-UEFI GRUB方案-操作手册.md` 增补"compute-booter"菜单项一节。
