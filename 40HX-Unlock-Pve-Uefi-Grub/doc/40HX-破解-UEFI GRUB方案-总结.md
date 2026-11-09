# CMP40HX-Unlock 项目总结

> 项目路径：`I:\40HX破解\pve方案\CMP40HX-Unlock-main`
> 上游仓库：<https://github.com/a1092595196/CMP40HX-Unlock>（NVPermissiveEFI，MIT）
> 目标卡：NVIDIA CMP 40HX（TU106，`10de:1f0b`），8 GB，无显示输出
> 目标平台：Proxmox VE 物理主机，UEFI + GRUB 引导
> 文档位置：本文与其余三份说明都在仓库的 `doc/` 目录下（`README.md` 在仓库根）
> 阅读顺序建议：本文 → `CMP40HX-Gen2链路问题说明.md`（Gen2 实验细节）→ `CMP40HX-Unlock-操作手册.md`（动手时看）

**结论先行**：这是一套「在内核加载之前、由 UEFI 应用直接写 GPU 寄存器」的解锁方案。
它不碰驱动、不碰 VBIOS，靠 GRUB 的 `chainloader` 把 EFI 应用插在 PVE 内核启动之前；
应用返回后 GRUB 继续引导 PVE。一次开机同时拿到 **Tensor/FP16 算力** 与 **PCIe Gen2 x16**，
且与 `nvidia-vgpu` 共存。

---

## 1. 项目定位

### 1.1 一句话

**把解锁从「驱动里做」改成「驱动前做」。**

`nvidia.ko` 初始化 GPU 时会把一批寄存器重新写成出厂值。传统做法是在驱动里打补丁、
在初始化流程中间插一手；本方案不动驱动，而是抢在驱动加载之前——此时 GPU 刚被 UEFI
枚举完、没有任何驱动占用——直接把寄存器写成解锁态，让驱动 probe 时看到的就是一个
已经解锁的设备。

### 1.2 与另一套方案（cmpunlocker 内核补丁）的本质区别

同一个目标卡、同一个目标，但作用层和时机完全不同：

| 维度 | cmpunlocker（内核补丁路线） | 本项目（UEFI 前置路线） |
|---|---|---|
| 作用时机 | 驱动 `probe` 内部（SEC2/GSP 流程之中），Gen2 在 `nv_open` | **内核加载之前**，UEFI 阶段 |
| 实现载体 | 打补丁的 `nvidia.ko` + systemd 服务 | EFI 应用 + GRUB `chainloader` |
| 算力怎么开 | 注入签名 610 Booter，在 SEC2 特权上下文里写 FECS | 直接 MMIO 写 FECS（此时 GSP 尚未启动） |
| Gen2 怎么做 | 改策略 → 端点镜像 → 驱动内 retrain（有时序竞态，需延迟补执行） | EFI 阶段写 policy + 双端 TLS + **两端 `LnkCtl.CCC` 对齐** + 上游 RL，且**放在 SBR 之后**做 |
| 失败后果 | 可能装错模块、驱动不加载 | 最坏是 EFI 返回失败 → 跳过解锁正常进 PVE |
| 与 vGPU | 运行期改寄存器，可能让 vGPU 会话掉线 | 内核前完成，不触发 FLR/GSP 重启 |
| 持久性 | 易失（每次开机重做） | 易失（每次开机重做） |
| 前置条件 | 能编译内核模块、版本匹配 | UEFI 引导 + **GRUB**（非 systemd-boot）+ Secure Boot 关闭 |

一句话：**cmpunlocker 是「把驱动改造后跑」，本项目是「抢在驱动前面动手」。** 后者不改
系统任何组件，因此更适合 PVE 这类还要跑 vGPU / 虚拟化的宿主。

### 1.3 为什么能与 vGPU 共存

```
UEFI 阶段写寄存器 + SBR     ← 此时内核没加载、没有驱动、没有虚拟机
        ↓
GRUB 继续引导 → 内核枚举 → nvidia / vGPU 驱动首次 probe
```

**所有会破坏总线状态的动作都发生在「还没人依赖它」的时候。** 驱动 probe 时看到的是一个
已经解锁、已复位完毕、链路已就绪的设备，因此不会触发运行期的 FLR / GSP 重启 /
驱动重协商——而这正是运行时方案会切断 vGPU 会话的原因。

---

## 2. 硬件与环境

### 2.1 本机配置（Gen2 跑通的那台）

| 项 | 值 |
|---|---|
| GPU | `0000:05:00.0`，`10de:1f0b`（TU106 CMP 40HX），class `0302`（3D controller），rev `a1`，8 GB，无显示输出 |
| 上游桥 | `0000:00:03.0`（Intel Xeon E7 v4 IIO PCIe Root Port 3），**单跳直连** |
| CPU / 芯片组 | Intel Xeon E7 v4 + C224，BIOS 中 Above 4G Decoding 已开启 |
| 亮机卡 | AMD Caicos `0000:01:00.0/01`，挂 `00:01.0`，**不同 root port**，与本方案无关 |
| 系统 | Proxmox VE 8.4，内核 `6.8.12-43-pve` |
| 引导 | UEFI + GRUB 2.06，Secure Boot 关闭 |
| 驱动 | NVIDIA 615.78.08（`nvidia` / `nvidia_modeset` / `nvidia_drm`，GSP 开启） |
| 实测结果 | 双端 `Speed 5GT/s, Width x16`，进系统保持；`nvidia-smi` 正常；无需驱动补丁 |

### 2.2 拓扑与「上游桥」是怎么选的

```
CPU (Xeon E7 v4 IIO)
 └── 00:03.0  Root Port 3   ← 本方案要操作的上游桥（LnkCtl2 / LnkCtl / LnkSta）
      └── 05:00.0  CMP 40HX  ← 端点（BAR0 里的算力/策略寄存器）
```

`find_upstream_bridge()` 不靠总线号猜，而是遍历所有 PCI-PCI 桥，挑出同时满足
`secondary ≤ GPU 总线 ≤ subordinate` 且 `span`（subordinate − secondary）最小的那个——
即拓扑上「最近」的一级。这样即使卡换槽、总线号变化也不用改代码。

### 2.3 不适用场景

| 场景 | 原因 |
|---|---|
| systemd-boot 引导的 PVE（ZFS + proxmox-boot-tool 的 systemd-boot 分支） | 没有 `chainloader` 这条链路 |
| BIOS / Legacy 引导 | 没有 UEFI 阶段可借 |
| 双 CMP 卡 | 代码要求 `matches == 1`，多卡直接拒绝 |
| 卡挂在 PCIe switch / PLX 后面 | 会选出 switch 的上游口，双端语义与直连不同 |
| 改名过的 GRUB 条目（不含 "Proxmox"） | `install-efi.sh` 靠 `awk '/^menuentry.*Proxmox/'` 抽取原条目 |

---

## 3. 仓库结构与文件职责

### 3.1 源码

| 文件 | 体积 | 作用 |
|---|---|---|
| `src/main.c` | 45 KB / 约 1.2k 行 | UEFI 应用主体：模式解析、PCI 扫描定位卡、选上游桥、解码 BAR、BOOT0 校验、SBR、Gen2 重训、屏幕输出、`efi_main` |
| `src/core_40hx.c` | 5.3 KB / 115 行 | 真正的算力寄存器写入逻辑（`write_full_set` / `do_permissive`），编译成 ELF 后嵌入 `.efi`，运行时加载执行 |
| `src/elf_loader.c` | 12.2 KB / 321 行 | 把嵌入的 SysV ELF 重定位目标加载到 2 GiB 以下，并解析符号/重定位 |
| `include/core_abi.h` | 1.8 KB | core 与 UEFI 宿主之间的 ABI，含 `_Static_assert` 偏移校验（`fb_mmio`=16、`host_alloc`=40、`platform_data`=112、size=120） |
| `include/uefi_min.h` | 9.1 KB / 272 行 | 最简 UEFI 类型/协议定义（不依赖 EDK2） |
| `include/elf64_min.h` | 1.3 KB | ELF64 结构定义 |
| `include/elf_loader.h` | 0.4 KB | ELF 加载器接口 |

### 3.2 构建、安装与工具

| 文件 | 体积 | 作用 |
|---|---|---|
| `Makefile` | 5.7 KB / 129 行 | 编译 + `objcopy` 嵌入 core + `ld -mi386pep` 链接（**直接输出到 `build/efi/`**，不留中间副本）+ `verify_efi.py` 校验；`make release` 一次产出 inspect/booter 两个模式到 `build/efi/` 并刷新 `build/efi/CHECKSUMS.sha256` |
| `install-efi.sh` | 25.7 KB / 628 行 | 安装器：第 1 步幂等补齐两个 ReBAR Linux 前置（`/etc/modprobe.d/nvidia-rebar.conf` 的 `NVreg_EnableResizableBar=1` 与 `/etc/default/grub` 的 `pci=realloc`，已有不动、缺失才写，改完先 `update-grub` 再烘焙），再拷 `.efi` 到 ESP、从 `grub.cfg` 抽取 PVE 原条目、生成 `/etc/grub.d/40-cmp40hx`、`update-grub`；收尾再做一次默认启动项幂等检查（`GRUB_DEFAULT` 未指向本方案则自动补 `booter`，已指向本方案或被有意设成其它真实条目则不动）；支持交互选「1 编译安装 / 2 预编译安装」，以及 `--build` / `--prebuilt` / `--force` / `--default` / `--no-default` / `--timeout` |
| `uninstall-efi.sh` | 4.2 KB / 81 行 | 删除菜单项与 ESP 目录，并对称回收三处配置改动（同样幂等：有就删、没有跳过）：① `nvidia-rebar.conf` 仅在内容为纯本方案行时才删，防误删用户自管同名文件；② `pci=realloc` 从 `GRUB_CMDLINE_LINUX_DEFAULT` 摘除；③ `GRUB_DEFAULT` 只当它指向本方案条目时才清除（单/双/无引号三种写法都认，指向其它条目一律不动），并把 `GRUB_TIMEOUT` 恢复为 PVE 出厂值 5，`GRUB_TIMEOUT_STYLE=hidden` 时额外提醒 5 秒不显示菜单；最后 `update-grub` |
| `tools/verify_efi.py` | 1.3 KB / 31 行 | 校验产物是合法 PE32+ / x86-64 / subsystem=10（EFI 应用） |

> 上游还带过 `tools/fetch-core.sh`、`tools/verify_core.py`（下载并校验 GA102 版 core），
> 本仓库**已不含**这两个脚本：core 由 `src/core_40hx.c` 自行编译，不走下载流程。

### 3.3 产物与文档

| 文件 | 作用 |
|---|---|
| `build/efi/NVPermissiveEFI-INSPECT.efi` | inspect 模式（无参数启动时的兜底） |
| `build/efi/NVPermissiveEFI-BOOTER.efi` | booter 一条龙模式 |
| `build/efi/CHECKSUMS.sha256` | 两个 `.efi` 的 SHA256，`make release` 会覆盖 |
| `doc/CMP40HX-Unlock-操作手册.md` | 从装机到验收的完整步骤、参数详解、故障分流表、回退 |
| `doc/CMP40HX-Gen2链路问题说明.md` | Gen2 从卡住到跑通的完整实验记录与根因分析 |
| `doc/CMP40HX-Unlock-项目总结.md` | 本文 |
| `README.md` | 项目构成、文件职责、硬件环境、原理速览 |

> 产物文件名里的 `INSPECT` / `BOOTER` 只描述**无参数启动时的默认模式**（编译期 `-DAUTO_MODE`）；
> 从 GRUB 菜单进来时菜单项会显式传 `inspect` / `booter`，**运行时以参数为准**。
> 旧版无后缀的 `NVPermissiveEFI.efi` 已改名；`uninstall-efi.sh` 是整个删 `EFI/UNLOCK` 目录，
> 因此升级重装后 ESP 里不会残留旧名文件。

### 3.4 `main.c` 内部函数地图

| 函数 | 职责 |
|---|---|
| `efi_main` | 入口：拿 BootServices、找 RootBridgeIO、依次调下面各步 |
| 模式解析 | 读 `LoadOptions` → `inspect` / `compute` / `full` / `gen2` / `gen2-ld`；无参数按编译期 `AUTO_MODE` 兜底 |
| `find_cmp90hx` | 遍历所有 RootBridge 的 bus/dev/func，找 `10de:1f0b`（也接受 `10de:220d`），要求**恰好 1 个** |
| `find_upstream_bridge` | 按 `secondary ≤ GPU 总线 ≤ subordinate` 且 span 最小选出最近上游桥 |
| `prepare_pci_resources` / `decode_memory_bar` | 解码 BAR0/BAR1，打开 MEMORY + MASTER |
| `restore_gpu_config` | SBR 之后写回复位前备份的 6 个 BAR + COMMAND |
| `load_nvpermissive_core` | 定位 `.core` 段、加载 ELF、解析 `do_permissive` 与版本符号 |
| `run_requested` | 按模式分发：只读 / 写寄存器 / SBR / Gen2 |
| `sbr_once` / `dual_sbr` | 单次 / 连续两次 PCI 二次总线复位，含沉降与设备 ID 轮询 |
| `apply_gen2` | Gen2 三部曲：写策略 → 物化能力 → 双端 CCC 对齐 + 重训 |
| `apply_gen2_link_disable` | `gen2-ld` 专用：上游 Link Disable → Enable 强制物理层重训（当前不出菜单） |
| `dump_link_state` / `link_is_gen2` | 打印单端 `LnkCap/LnkCap2/LnkCtl/LnkCtl2/LnkSta` 全字段 / 判定是否已到 Gen2 |
| `console_write` / `console_printf` / `gpu_log` | 纯 ASCII 日志（固件字库没有中文，中文会显成方块） |

> **不带 printf 的做法**：`printf` 是给「有 CRTC 的 EFI 应用」用的，本应用只往
> `SystemTable->ConOut` 写 ASCII，所以自己实现了 `append_char/append_string/append_number`
> 和 `format_message`——这是上游的设计，改动时别引入 `Print()`。

---

## 4. 一次开机的完整执行链路

### 4.1 时序

```
BIOS/UEFI POST
  │
  ├─ 选 GRUB 菜单项 "PVE + CMP40HX UNLOCK (compute, ...)"
  │    （install-efi.sh 生成：insmod chain → search 定位 ESP →
  │      set save_root=$root → set root=$unlockroot
  │      → chainloader /EFI/UNLOCK/NVPermissiveEFI-COMPUTE.efi compute
  │      → boot）
  │
  ├─ chainloader 载入 EFI 应用、boot 移交控制
  │    ┌──────────── NVPermissiveEFI 运行（UEFI 阶段）─────────────┐
  │    │ ① parse_mode：读 LoadOptions → compute/full/gen2/inspect │
  │    │ ② find_cmp90hx：遍历所有 RootBridge 的 bus/dev/func       │
  │    │     找 10de:1f0b（也接受 10de:220d），要求**恰好 1 个**   │
  │    │ ③ find_upstream_bridge：按 secondary≤GPU总线≤subordinate  │
  │    │     且 span 最小，选出"最近的"上游 PCI-PCI 桥             │
  │    │ ④ prepare_pci_resources：解码 BAR0/BAR1，开 MEMORY+MASTER │
  │    │ ⑤ 读 BOOT0，按架构白名单校验（不符就拒绝、不写）          │
  │    │ ⑥ load_nvpermissive_core：把嵌入的 core ELF 加载到 <2 GiB  │
  │    │ ⑦ run_requested：按模式写寄存器、做 SBR、做 Gen2          │
  │    │ ⑧ 打印结果，等按键（10 秒无按键自动继续）                 │
  │    └──────────────────────────────────────────────────────────┘
  │
  ├─ EFI 应用返回 EFI_SUCCESS，控制权回到 GRUB
  │    GRUB 继续执行同一 menuentry 里 `boot` 之后的行 = 抽出来的 PVE 原条目
  │    （linux / initrd / boot），于是 **PVE 照常启动**
  │
  └─ PVE 内核加载 → PCI 重新枚举 → nvidia / nvidia-vgpu 驱动 probe
       寄存器已是解锁态；驱动不再重复解锁，也不存在时序竞态
```

### 4.2 关键：`chainloader` 之后为什么还能继续启动

GRUB 的 `chainloader` + `boot` 对一个 EFI 应用是「调用并等待返回」，应用退出后
GRUB 会接着执行脚本的下一条命令。`install-efi.sh` 正是把抽取出来的 PVE 原条目主体
（去掉 `menuentry` 头、保留结尾的 `}`）拼在 `boot` 之后，于是形成：

```
menuentry "PVE + CMP40HX UNLOCK (compute, ...)" {
    insmod chain
    search --no-floppy --fs-uuid ... --set=unlockroot
    set save_root=$root
    set root=$unlockroot
    chainloader /EFI/UNLOCK/NVPermissiveEFI-COMPUTE.efi compute
    boot                       ← 应用在这里执行、返回
    set root=$save_root        ← 恢复原 root
    <PVE 原条目主体：linux / initrd / boot>
}
```

**这是整个方案成立的关键。** 一旦换成 systemd-boot 或直接 `exit`，这条链就断了。

### 4.3 菜单项是怎么生成的

`install-efi.sh` 不硬编码内核版本，而是从 `/boot/grub/grub.cfg` 里 `awk` 出第一条
`menuentry ... Proxmox` 的完整函数体，逐字节复制进新的 `40-cmp40hx`，只在前后加上
`chainloader` 与 `set root` 恢复。因此内核升级后 `update-grub` 会自动带上新内核，
不需要重新安装。为了让 `update-grub` 的计数正确，脚本按 `menuentry` 行数统计，
安装后还会逐字节 `cmp` 对比 ESP 里的产物与仓库里的产物。

---

## 5. 模式与寄存器动作

### 5.1 三种可选模式（外加两个源码保留模式）

| 模式 | 载体文件 | 动作 | SBR 次数 | 预估耗时 |
|---|---|---|---|---|
| `inspect` | `NVPermissiveEFI-INSPECT.efi` | 只读打印 BOOT0 / XP3G / FECS_PLM / SS0 / SS0_RD / SS1 / FUSE_DISP / PDISP | 0 | 秒级 |
| `compute` | `NVPermissiveEFI-COMPUTE.efi` | 全套算力寄存器 + 1 次双重 SBR + Gen2 | 2 次单 SBR | ~30 s |
| `full` | `NVPermissiveEFI-FULL.efi` | compute → graphics → gr-reset → gr-acr，最后 Gen2 | 8 次单 SBR | ~85 s |
| `gen2`（源码保留） | 任意 `.efi` 传参 | 只写 Gen2 策略 + 重训，不写算力寄存器、不复位 | 0 | 秒级 |
| `gen2-ld`（源码保留） | 任意 `.efi` 传参 | 完整 Gen2 策略 + 上游 Link Disable 强制物理层重训 | 0 | 秒级 |

未传参数且不是 AUTO 构建时，默认落到 `inspect`（只读，零风险）。

后两个模式**不再生成菜单项**，理由见 6.7。

### 5.2 算力寄存器（BAR0）

| 偏移 | 写入值 | 代码位置 | 含义 |
|---|---|---|---|
| `0x008E1B0` XP3G_PLM | `0xFFFFFFFF` | `core_40hx.c` | 功耗/性能锁 PLM 清除 |
| `0x00409650` FECS_PLM | `0xFFFFFFFF` | `core_40hx.c` | FECS 权限位全开 |
| `0x00409664` SS0 | `0x88888888` | `core_40hx.c` | SM 速度选择子（Tensor/FP16 的核心） |
| `0x0040966C` SS1 | `0x00000008` | `core_40hx.c` | 配套状态位 |
| `0x00088610` XVE_08610 | `0x00001001` | `core_40hx.c` | 与 RTX 2070 vBIOS 同值（粘滞、语义未公开） |
| `0x00409500/0960C/09610/09614` | `0xFFFFFFFF` | `core_40hx.c` | consumer chain（推测在 GSP 初始化时被消费一次） |
| `0x00419B54` / `0x00600000` | `0xFF` / `0xFFFFFFFF` | graphics 阶段 | PDISP/显示探测（无害，可能被门控） |

Gen2 相关的策略寄存器见 6.2。`SS0=0x88888888`、`SS1=0x8`、`FECS_PLM` 这组值与公开
spec（ngthaihoc/CMP30HXmodtoGEN2）交叉核对一致。

### 5.3 SBR（PCI 二次总线复位）

```c
sbr_once():
  读上游桥 BRIDGE_CONTROL(0x3e) → 置位 0x40（BUS_RESET）
  Stall 100 ms
  清 0x40
  Stall 1 s
  restore_gpu_config()      // 写回 SBR 前备份的 6 个 BAR + COMMAND
  Stall settle_ms (3000)
  轮询设备 ID（最多 100 次 × 100 ms），与首次扫描保存的 expected_id 比较
```

`booter` 一条龙只在入口做一次 `sbr_once(3000)`（归零复位）；早期 `compute`/`full` 的 `dual_sbr()`（连续两次 `sbr_once`）已随这两个模式退役、代码里不再存在。

settle 从 8000ms 降到 3000ms **只压缩了 `restore_gpu_config()` 之后的那段保守静置垫**：紧随其后还有设备 ID 轮询（最多 100×100 ms，一读到 `10de:1f0b` 就放行）作为真正的就绪门，所以缩短它不影响复位可靠性。注意这与 §9.1 被撤销的「改撤复位→写 BAR 之间的 1 s」是**两处不同的时间**，后者才是不能动的。

**两处细节值得注意：**

1. **SBR 会清掉端点的链路状态**（`LnkCtl2` 回到 Gen1），所以 `compute` 的流程是
   「写寄存器 → 双重 SBR → **再**做 Gen2」，`full` 也把 Gen2 放在所有 SBR 之后。
2. **设备 ID 校验用的是首次扫描时保存的 `expected_id`**，而不是上游硬编码的 `220d`。
   否则 40HX 复位后读回 `1f0b` 会被判成「复位失败」——这是相对上游的四处改造之一。

### 5.4 core 的嵌入与加载机制

这一层是「为什么能一边用 C 写裸逻辑、一边塞进 .efi」的关键：

```
src/core_40hx.c
  │ gcc -ffreestanding -mcmodel=large -fno-pie …（SysV ABI，不是 MS ABI）
  ▼
build/40hx-core.o            （交叉编译产出 COFF）
  │ objcopy -I pe-x86-64 -O elf64-x86-64   → 转成 ELF64 可重定位目标
  │ objcopy -I binary -O elf64-x86-64 …    → 再当成二进制数据包装
  │ --rename-section .data=.core
  │ --redefine-sym …=_binary_nvpermissive_core_o_start / _end
  ▼
core_blob.o 里的 .core 段 → 链接进 .efi
  ▼
运行时：load_nvpermissive_core()
  AllocatePages(<2 GiB) → 按 program/section 拷贝 → 处理重定位
  → 用 16 字节 trampoline（mov rax,imm64; jmp rax）把外部符号接上
  → 解析 do_permissive 与 nvperm_core_version
```

- **重定位只支持 4 种**：`R_X86_64_64`、`PC32`、`PLT32`、`32S`；
- **外部符号白名单**：`ioread32`、`iowrite32`、`memcpy`、`memcpy_toio`，全部由 `main.c` 提供；
- **ABI 有静态断言**：`core_abi.h` 里 `platform_data` 必须在偏移 112、结构体必须是 120 字节。
  改结构要先改断言，否则会静默错位；
- 加载到 2 GiB 以下是为了让 `32S` 重定位仍然有效。

---

## 6. PCIe Gen2 全链路

这是本项目最花时间的一块，也是与上游差别最大的地方。

### 6.1 为什么要分三步

Gen2 不是「写一个寄存器就升速」。本机实测表明，必须依次打通三件事，任何一步缺失都
会表现为「**训练完成、无报错、速率不变**」：

| 步骤 | 做什么 | 缺了会怎样 |
|---|---|---|
| ① 物化能力 | 写策略寄存器，让端点把 5GT/s 报出来 | `LnkCap` 停在 `00453d01`（Gen1 only），`LnkCtl2.Target` 只能是 2.5GT/s |
| ② 对齐时钟模式 | 双端 `LnkCtl.CCC` 统一 | `LnkCap` 已是 2、`TLS=2` 也写进去了，但重训后仍回落 `1101` |
| ③ 发起重训 | 双端 TLS/HASD、上游 HAWD/Retrain、清 LBMS | 链路不会重新协商，永远停在上电时的速率 |

### 6.2 第一步：策略寄存器与能力物化

| 偏移 | 操作 | 作用 |
|---|---|---|
| `0x008872C` XVE_OVR | 置 `0x6` | PCIe Gen2 override 总开关 |
| `0x008C040` CFG | bits[19:18] = 2 | 最大链路速率策略（3 = Gen1） |
| `0x008C1C0` PL | bits[18:17] = 2 | 物理层速率策略 |
| `0x008841C` MISC | 清 `0x5000` / 置 `0x2800` | 私有 policy（**本项目独有，内核补丁路线不写这里**） |
| `0x008C2C0` CYA | 清 bit2（`DIS_G2=0`） | 允许 Gen2 |

写完后必须 `Stall(200 ms)` 等端点重新生成能力表：`LnkCap` 低 4 位由 `1` 变 `2`，
这是「能力物化成功」的唯一证据。**如果 `LnkCap` 还是 `00453d01`，后面两步不用做了。**

### 6.3 第二步：CCC 对齐（本次的根因）

PCIe 规范要求 `LnkCtl.Common Clock Configuration`（bit6）在链路两端相同。本机的原始值：

| 端 | 原始 `LnkCtl` | CCC | 同时声明的 `LnkSta.SlotClk` |
|---|---|---|---|
| 上游 root port（`00:03.0`） | `0042` | **1** common clock | bit12 = 1 |
| GPU 端点（`05:00.0`） | `0000` | **0** separate clock | bit12 = 1 |

两端都说「用插槽时钟」，CCC 却相反。这个组合下重训 Gen2 的表现是：
**无 `TrainingError`、`DLL`(bit13) 保持 active、`LBMS`(bit14) 置位、速率不变**。
`LBMS` 被置上是因为带宽变更流程跑过一轮但没升速，早期被误读成「重训失败」。

实质是 clock skew 预算超出了 Gen2 容限；Gen1 宽容度大，所以 2.5GT/s 一直正常。
**上游 root port 自报的 `CCC=1` 在本平台是误导性的**——按它设置反而永远停在 Gen1。

实测对比（同一台机器、同一份代码，只改 CCC）：

| 轮次 | 双端 `LnkCtl.CCC` | GPU `LnkSta` | 上游 `LnkSta` | 结果 |
|---|---|---|---|---|
| round 1 | 都 = 1（镜像上游） | `1101` spd=1 | `3101` spd=1 | 失败 |
| round 2 | 都 = 0（separate clock） | **`1102` spd=2** | **`3102` spd=2** | **成功** |

关键细节：round 2 的 `gpu tgt` 那一行（刚写完、还没轮询）就已经是 `1102`——**写入即刻生效**，
不存在「训练慢需要多等」。所以早期所有「环境都就绪但训练不过」的观察，都缺了同一个前提。

代码最终形态是**先试 CCC=0**（一次命中），CCC=1 只作为「平台真的共时钟」时的兜底；
两轮都失败则把上游 `LnkCtl` **原样恢复**，保证进系统时的链路状态和不解锁时一致。

### 6.4 第三步：重训序列

```
1. 写 policy（6.2 的 5 个寄存器）
2. Stall(200ms)，等 LnkCap 低 4 位 = 2
3. 双端 LnkCtl2 = TLS(2) | HASD(bit5)      ← 禁止链路硬件自己把速率降回去
4. 双端 LnkCtl：清 ASPM(bit1:0)、CCC(bit6) 统一为 0
5. 上游 LnkCtl |= HAWD(bit9) | Retrain(bit5=RL)
6. 上游写 1 清 LnkSta.LBMS(bit14, RW1C)
7. 轮询双端 LnkSta.spd == 2（20 × 100ms）
```

与上一版相比只多做 4 件小事（CCC 对齐、HASD、HAWD、清 ASPM/LBMS），
**其中 CCC 对齐是唯一决定性的**，其余三件是加固。

`apply_gen2()` 会把每一步的前后状态都打到屏幕上（`policy` / `pre` / `tgt` / `done` 四行 ×
双端 × 全字段），这是排查时唯一的现场证据——旧版只打一条 `endpoint LnkSta=1101`，
根本分不清「能力没物化」和「能力有了但训练上不去」。

### 6.5 实测数据（UEFI 阶段）

```text
gen2 policy: OVR=00000006 CFG=80085800 PL=00340036 MISC=20342d00 CYA=068711b3
gen2 gpu  policy CAP=00453d02 CAP2=00000006 CTL=0000 CTL2=0002 STA=1101 spd=1
gen2 root pre    CAP=077a3903 CAP2=0000000e CTL=0042 CTL2=0001 STA=7101 spd=1
gen2: retrain round 1, CCC=0 on both ends
gen2 gpu  tgt    CAP=00453d02 CAP2=00000006 CTL=0000 CTL2=0022 STA=1102 spd=2
gen2 root tgt    CAP=077a3903 CAP2=0000000e CTL=0200 CTL2=0022 STA=3102 spd=2
gen2 gpu  done   CAP=00453d02 CAP2=00000006 CTL=0000 CTL2=0022 STA=1102 spd=2
gen2 root done   CAP=077a3903 CAP2=0000000e CTL=0200 CTL2=0022 STA=3102 spd=2
gen2: link trained at Gen2 during the UEFI stage.
```

读数要点：

| 字段 | 含义 |
|---|---|
| `CAP=00453d02` | 端点 `LnkCap` 低 4 位 = 2，5GT/s 能力已物化（`00453d01` = 只有 Gen1） |
| `CTL2=0022` | `LnkCtl2` = TLS(2) \| HASD(bit5)，目标速率已锁 5GT/s |
| `CTL=0000`（gpu） | endpoint 的 `LnkCtl`：ASPM 关、CCC=0、未禁用 |
| `CTL=0200`（root） | 上游 `LnkCtl`：只剩 HAWD(bit9)，RL 已自清 |
| `STA=1102` / `3102` | `LnkSta`：bits[3:0]=2 → 已训到 5GT/s；bit8=1 → width=16；`3102` 多出的 `0x2000` 是上游的 DLL active |

### 6.6 进系统后

```text
端点 0000:05:00.0
  LnkCap:  Speed 5GT/s, Width x16
  LnkCap2: Supported Link Speeds: 2.5-5GT/s
  LnkCtl2: Target Link Speed: 5GT/s
  LnkSta:  Speed 5GT/s, Width x16

上游 0000:00:03.0
  LnkCap:  Speed 8GT/s, Width x16
  LnkCtl:  ASPM Disabled; RCB 64 bytes, Disabled- CommClk-
  LnkCtl2: Target Link Speed: 5GT/s
  LnkSta:  Speed 5GT/s, Width x16

nvidia-smi: NVIDIA CMP 40HX, 8192MiB, P8, 14W/184W
```

三点结论：

1. **链路保住了**：RM/GSP 初始化后双端仍是 5GT/s x16，没有被降回 Gen1；
2. **`CommClk-` 是正确形态**：它表示 CCC=0 挺过了驱动初始化。**不要用 `setpci` 把它
   「修」成 `CommClk+`**，那会让 Gen2 重新回落；
3. **不需要任何驱动补丁**：`nvidia-smi` 正常、CUDA 可用、8 GB 显存全可见。

### 6.7 `gen2` / `gen2-ld` 为什么不出菜单了

- **`gen2`**：寄存器是易失的，每次开机都必须重跑 `compute`，而 `compute` 末尾本来就包含
  完全相同的策略 + 重训。单独留一个 `gen2` 条目会让人误以为「先 compute 一次，以后只跑 gen2 就行」。
- **`gen2-ld`**：上游 Link Disable → Enable 能强制物理层重新协商（`cmpunlocker` 的
  `PCIE_LINK_DISABLE_AUDIT.md` 在 Alder Lake 上实测到 Gen2 x16），但 CCC 对齐后普通重训
  已经够用，没必要冒断链风险——链路可能回不来、进系统后 GPU 读成 `ffffffff`，
  恢复只能整机断电。代码保留，参数仍可用（`gen2-ld`），只是不生成菜单项。

---

## 7. 验证结果

### 7.1 已验证

| 项 | 结果 |
|---|---|
| 三个 `.efi` 与 `CHECKSUMS.sha256` | 全部一致 |
| 产物格式 | `verify_efi.py` 全部通过（MZ、PE、machine=0x8664、PE32+、subsystem=10） |
| 代码与文档的寄存器表 | 逐条对得上（`apply_gen2` + `core_40hx.c` 的 `write_full_set`） |
| 菜单项数量 | 3 个（inspect / compute / full），与 README、操作手册一致 |
| 算力解锁 | `nvidia-smi` 正常、CUDA 可用、8192 MiB 全可见 |
| **PCIe Gen2（2026-11-08）** | UEFI 阶段 `1102`/`3102`；进系统双端 `Speed 5GT/s, Width x16`；上游 `CommClk-`；无 AER 报错 |
| 持久性 | 冷启动/热重启后重跑 `compute` 均能复现（寄存器由 GSP 每次重写，必须重跑） |

### 7.2 机制上仍有推测成分的地方

- `0x88610` 写成 `0x1001`：语义未公开，只是「与 RTX 2070 vBIOS 同值、可写且粘滞」；
- `0x409500/40960C/409610/409614` 的 consumer chain：推测「在 GSP 初始化时被消费一次」，
  所以放在 EFI 阶段写；运行期写虽然能粘住但不影响判定；
- `MISC(0x8841C)` 的 `0x2800` 位组：私有 policy，未公开；
- 「为什么两端 CCC=0 才对」：实测确定，机制层面归因为 clock skew 容限，属合理推断。

这些都属于「实测有效但原理未完全闭环」的部分，改动时不要当成已证实的因果。

---

## 8. 已知瑕疵与边界

| 项 | 说明 | 影响 |
|---|---|---|
| `Makefile` 引用了不存在的 `tests/test_elf_loader.c` | 仓库里没有 `tests/` 目录 | `make test` 必失败；`make release` 不受影响 |
| 构建工具链 | 装 `build-essential binutils python3` 后 `make release` 即可 | 若 `ld` 不含 PE 后端（`unrecognised emulation mode: i386pep`），改用 `CC/LD/OBJCOPY=x86_64-w64-mingw32-*` |
| 只接受**恰好 1 张**卡 | `matches != 1` 直接拒绝 | 双卡机器无法使用 |
| 依赖 **GRUB** | 走 `chainloader` + 抽 `grub.cfg` 里的 PVE 条目 | systemd-boot 的 PVE 不适用 |
| 抽取菜单项靠 `awk '/^menuentry.*Proxmox/'` | 依赖 PVE 默认条目名含 "Proxmox" | 改过菜单名的系统需手工指定 |
| `BOOT0` 白名单较宽 | 同时接受 `0x17`/`0x1a`/`0x16`/`0x0fxxxxxx`/`0x0fffffff` | 兼容性好，但「全 1 读回」这种异常值也能通过校验 |
| 换平台需复验 | Gen2 结论只对本机（Xeon E7 v4 IIO）成立 | 换主板/CPU 后按 6.5 的四个判据重测 |
| 上游文案残留 | 旧版 `.efi` 里还有 "Windows" 字样 | 看到即说明装的是旧产物，按操作手册 K 项处理 |

---

## 9. 踩坑记录

### 9.1 构建 / 安装环节（2026-11-08）

**当时的结论：解锁寄存器逻辑本身没问题，`compute` / `full` 在这台 PVE 主机上可以完整跑完；
问题全在构建与安装环节。**（后来为解决 Gen2 又改了 `apply_gen2()`，见第 6 章。）

排查中曾误判一次：屏幕停在 `[core] SBR via 0000:00:03.0; settling 8 seconds` 之后没有下文，
被理解为「撤复位后只等 1 秒就写 BAR，时序太短」，于是重写了 `sbr_once()`（先轮询端点 30 s、
BAR 重试 10 次、加日志）。**实测证明这个判断是错的**——回退原版重新编译后，同一台机器、
同一张卡跑通了。该重写已撤销。

> 后记：现在 `sbr_once()` 里 `restore_gpu_config()` **之后**的 settle 已从 8 s 缩到 3 s（见 §5.3），但它后面有设备 ID 轮询兜底，和被撤销的「撤复位→写 BAR 之间的 1 s」不是同一处，别混为一谈。

真正的原因在构建/安装环节：

| 现象 | 真实原因 |
|---|---|
| 自编 `.efi` 一上来就 `error: unknown error.`，连 banner 都没有 | 安装进 ESP 的产物不是当次编译的那份（旧文件残留 / 编译链被混用），固件在 `LoadImage` 就失败 |
| 屏幕停在 `settling 8 seconds` 后无输出 | 早期那份二进制本身有问题；正常产物会继续，整段 SBR 沉降 + 轮询最长约 20 秒，等不够也会误判成「卡死」 |
| 同一份脚本今天能跑、明天报 `set: Illegal option -` | 从 Windows 拷过去的 `.sh` 带了 CRLF（`sed -i 's/\r$//'` 解决） |

**判据**：`install-efi.sh` 安装时会打印 ESP 里两个 `.efi` 的 SHA256，与仓库里
`( cd build/efi && sha256sum -c CHECKSUMS.sha256 )` 对账，不一致就是装错了。

本仓库相对上游保留的改动，收敛为三类：

1. `install-efi.sh`（3 个菜单项、交互选编译/预编译、备份去执行位、按 `menuentry` 行计数、
   安装后逐字节 `cmp` 校验）；
2. `src/main.c` 的**文案**（等键 60 s → 10 s，Windows 字样改 Linux，中文改纯 ASCII 以免
   固件显示方块）；
3. `src/main.c` **`apply_gen2()` 的重训逻辑**（双端 CCC 对齐、HASD、HAWD、清 ASPM/LBMS、
   双端全字段打印）。

解锁寄存器表与 SBR 的核心时序（撤复位→写 BAR 的 1 s）仍是上游原版；唯一改动是 §5.3 所述、把复位后有 ID 轮询兜底的那段 settle 静置垫从 8 s 缩到 3 s。

### 9.2 Gen2 环节（同一天，误判史）

| 曾经相信的结论 | 被什么推翻 |
|---|---|
| 「策略位被 GSP 改回 3/1，所以训不上」 | 关掉 GSP 后寄存器一字不差，链路照样 `1101` |
| 「PHY 在 devinit 时配置一次，之后重训必然回落」 | 进系统后链路稳定在 5GT/s，没有被回落 |
| 「重训没生效（`RL` 没自清）」 | `0x0262 → 0x0240`，RL 确实自清了，`LBMS` 也置位了 |
| 「信号完整性不够」 | 无 `TrainingError`、`DLL` active、无 AER 报错 |
| 「需要写入后多等一会」 | round 2 的 `tgt` 行即刻就是 `1102` |

真正的差异只有一个变量：**双端 `LnkCtl.CCC` 是否一致**。

---

## 10. 安全边界与回退

- 不写 VBIOS / SPI ROM、不动 eFuse、不改主板 BIOS —— 全部是运行时寄存器写；
- 完全易失：每次上电由 GSP 重新初始化，本方案只是每次开机「抢先写一遍」；
- 最坏情况：EFI 阶段失败（找不到卡 / BOOT0 不符 / SBR 后 ID 校验失败）→ 返回错误 →
  跳过解锁按原条目进 PVE，不会卡死也不会变砖；
- SBR 是标准 PCI 二次总线复位，等价于拔插卡，且发生在内核加载之前，不影响已加载的驱动；
- 三级回退：改 `GRUB_DEFAULT` 回普通条目 → `uninstall-efi.sh` 彻底移除 →
  冷断电即回工厂状态。

唯一需要格外小心的是**不要用 `setpci` 去改运行中链路的 `LnkCtl`**。运行期改配置空间
可能让链路直接掉，只能整机断电恢复。

---

## 11. 文档与代码的对应关系

| 想知道… | 看哪里 |
|---|---|
| 文件都是干什么的、硬件什么情况 | `README.md` |
| 从装机到验收怎么做、报错怎么办 | `doc/CMP40HX-Unlock-操作手册.md` |
| 架构、执行链路、core 嵌入机制 | 本文 §3–§5 |
| Gen2 为什么这么做、实测数据 | 本文 §6 + `doc/CMP40HX-Gen2链路问题说明.md` |
| Gen2 相关的代码 | `src/main.c` 的 `apply_gen2()` / `apply_gen2_link_disable()` |
| 算力寄存器相关的代码 | `src/core_40hx.c` 的 `write_full_set()` |
| SBR 相关的代码 | `src/main.c` 的 `sbr_once()` / `dual_sbr()` |
| 安装逻辑 | `install-efi.sh` |

---

## 12. 一句话总结

**把解锁从「驱动里做」改成「驱动前做」：用 GRUB 的 `chainloader` 借一次 EFI 应用执行窗口，
写完全套寄存器、做完 SBR、把双端 CCC 对齐后训好链路，再把控制权交回 GRUB 继续启动 PVE。**

代价是必须 UEFI + GRUB + 关 Secure Boot，并且每次开机多等 30（compute）～85（full）秒；
换来的是不修改任何系统组件、不与 vGPU 抢总线状态、失败可无损回退。
