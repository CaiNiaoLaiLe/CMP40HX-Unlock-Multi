# CMP40HX-Unlock

NVIDIA **CMP 40HX**（TU106，`10de:1f0b`）的 **UEFI 前置解锁**方案。

在 PVE 内核加载之前，由 GRUB 调用一个 EFI 应用直接写 GPU 寄存器，
一次开机同时拿到 **Tensor / FP16 算力** 和 **PCIe Gen2 x16**。

- 不写 VBIOS / SPI ROM，不改 `nvidia.ko`，不装内核模块；
- 所有写操作都发生在内核加载前，不触发运行期 FLR / GSP 重启，
  因此可以与 `nvidia-vgpu` 共存；
- 完全易失：每次开机重做一遍；失败就跳过，最坏结果是等于没解锁。

上游仓库：<https://github.com/a1092595196/CMP40HX-Unlock>（NVPermissiveEFI，MIT）

> **操作步骤看 [`doc/CMP40HX-Unlock-操作手册.md`](doc/40HX-破解-UEFI GRUB方案-操作手册)。**
> 本 README 只讲项目构成、代码文件职责和硬件环境。

---

## 硬件与环境

本机实测配置（Gen2 已跑通的那台）：

| 项 | 值 |
|---|---|
| GPU | CMP 40HX，TU106，`10de:1f0b`，8 GB，无显示输出，`0000:05:00.0` |
| 上游桥 | `0000:00:03.0`（Intel Xeon E7 v4 IIO PCIe Root Port），单跳直连 |
| CPU / 芯片组 | Intel Xeon E7 v4 + C224，BIOS 中 Above 4G Decoding 已开启 |
| 系统 | Proxmox VE 8.4，内核 `6.8.12-43-pve` |
| 引导 | UEFI + **GRUB 2.06**，Secure Boot 关闭 |
| 驱动 | NVIDIA 615.78.08（开源模块，GSP 开启） |
| 实测结果 | UEFI 阶段训练到 Gen2 x16，进系统保持：双端 `Speed 5GT/s, Width x16`，`nvidia-smi` 正常 |

**不适用**：systemd-boot 引导、BIOS/Legacy 引导、双 CMP 卡、卡挂在 PCIe switch/PLX 之后。

---

## 仓库结构

| 文件 | 作用 |
|---|---|
| `src/main.c` | UEFI 应用主体：模式解析、PCI 扫描定位 `10de:1f0b`、选最近的上游桥、解码 BAR0/BAR1、BOOT0 校验、SBR、`apply_gen2()` 链路重训、日志落盘、`efi_main` |
| `src/booter.c` | **booter 一条龙核心**：DIRECT_SEC2 特权启动（v70 移植），经 SEC2/GSP boot 提权后写 SS0/SS1 完成算力解锁，不做 SBR |
| `src/core_40hx.c` | 嵌入的 core ELF：只读 inspect 寄存器回显 + ReBAR XVE aperture 解锁（compute/full 直接写寄存器路径已随 booter 一条龙退役） |
| `src/elf_loader.c` | 把嵌入的 SysV ELF 加载到 2 GiB 以下，处理重定位并用 trampoline 接上外部符号 |
| `include/core_abi.h` | core 与 UEFI 宿主之间的 ABI，含 `_Static_assert` 的偏移/长度校验（`platform_data`=112、size=120） |
| `include/booter*.h`、`include/elf64_min.h`、`include/elf_loader.h` | booter 寄存器/资产/WPR 元数据，与 ELF64 加载器接口 |
| `include/uefi_min.h` | 不依赖 EDK2 的极简 UEFI 类型与协议定义 |
| `Makefile` | 一次产出两种模式：core 编译 → `objcopy` 包成 `.core` 段 → `ld -mi386pep` 链接（直接输出到 `build/efi/`）→ `verify_efi.py` 校验 |
| `assets/` | booter 必需的 7 个固件资产（`.bin`，自包含副本）；仅链接 booter 变体时使用，同步本目录即可构建 |
| `install-efi.sh` | 安装器：两个 `.efi` 拷进 ESP、生成 `/etc/grub.d/40-cmp40hx`（2 个菜单项）、`update-grub`，收尾检查开机默认项（未指向本方案则自动补 `booter`）；支持交互选「1 编译安装 / 2 预编译安装」，或 `--build` / `--prebuilt` / `--force` / `--default` / `--no-default` / `--timeout` |
| `uninstall-efi.sh` | 删除菜单项与 ESP 目录，并回收两个 ReBAR 前置与 `GRUB_DEFAULT`（仅当它指向本方案时清除）、倒计时恢复 `GRUB_TIMEOUT=5`，回到工厂状态 |
| `tools/verify_efi.py` | 校验产物是合法的 PE32+ / x86-64 / subsystem=10（EFI 应用） |
| `tools/check_unlock.sh` | 进系统后的验收脚本（gen2 / rebar / compute / booter trace 分项 PASS/WARN/FAIL，内置 BAR0 只读回显） |
| `build/efi/NVPermissiveEFI-INSPECT.efi` | inspect 模式（无参数启动时的安全默认）；`make` 直接输出到此，一个模式只有一份 |
| `build/efi/NVPermissiveEFI-BOOTER.efi` | booter 一条龙模式（日常用这个）；`make` 产物 |
| `build/efi/CHECKSUMS.sha256` | 两个 `.efi` 的 sha256 清单，`make release` 自动刷新；预编译模式安装前可 `sha256sum -c` 对账 |
| `doc/` | 四份文档，见文末「文档索引」 |

---

## 运行模式

两个镜像的区别只在**无参数启动时的默认模式**（编译期 `-DAUTO_MODE`）；从 GRUB 菜单进入时
菜单项会显式传 `inspect` / `booter` 参数，**运行时以参数为准**，文件名主要供人识别。

| 模式 | 载体 | 行为 |
|---|---|---|
| `inspect`（默认） | `NVPermissiveEFI-INSPECT.efi` | 只读：打印 BOOT0 / 各寄存器现值，不写、不复位，零风险 |
| `booter`（日常用） | `NVPermissiveEFI-BOOTER.efi` | 一条龙：DIRECT_SEC2 算力提权 → Gen2 重训 → ReBAR 8 GiB。走 SEC2/GSP boot 提权写 SS0/SS1，不经 core ELF、不做 SBR |

> `compute` / `full` / `gen2` / `gen2-ld` 等早期独立模式**已从源码移除**：booter 一条龙已完整覆盖其效果（实测 compute/gen2/rebar 均 PASS）；
> 直接写寄存器被 SEC2 权限层丢弃的历史路线已由 booter 特权启动取代。

---

## 原理速览

### 算力寄存器（BAR0）—— booter 目标终态参考

DIRECT_SEC2 在 SEC2 特权环境下最终写入的目标值（历史直接写寄存器路径已退役，以下为成功后的预期寄存器态）：

| 偏移 | 目标值 | 作用 |
|---|---|---|
| `0x008E1B0` XP3G_PLM | `0xFFFFFFFF` | 功耗/性能锁 PLM 清除 |
| `0x409650` FECS_PLM | `0xFFFFFFFF` | FECS 性能锁清除 |
| `0x409664` SS0 | `0x88888888` | SM 速度选择子（Tensor/FP16 的核心） |
| `0x40966C` SS1 | `0x00000008` | 配套状态位 |

目标值与公开 spec（ngthaihoc/CMP30HXmodtoGEN2）交叉核对一致。

### PCIe Gen2

三步，缺一不可：

1. **物化能力**：写 `OVR(0x8872c)=6`、`CFG(0x8c040)[19:18]=2`、`PL(0x8c1c0)[18:17]=2`、
   `MISC(0x8841c)`、`CYA(0x8c2c0).bit2=0`，等 200 ms 让端点把 `LnkCap` 从
   `00453d01` 抬到 `00453d02`；
2. **对齐时钟模式**：双端 `LnkCtl.CCC`(bit6) 必须相同。本机上游报 1、GPU 报 0，
   这种「不一致」会让 Gen2 重训**无报错地回落到 2.5GT/s**。实测两端统一为 **0** 才通得过；
3. **发起重训**：双端 `LnkCtl2 = TLS(2) | HASD`、清 ASPM、上游 `HAWD | Retrain`、
   写 1 清 `LnkSta.LBMS`，然后轮询双端 `spd==2`。

成功后进系统不会被 RM/GSP 改回去 —— `lspci` 里上游显示 `ASPM Disabled; CommClk-`，
这是**正常形态**，不要把它「修」成 `CommClk+`。

详细数据和推理过程见 [`doc/CMP40HX-Gen2链路问题说明.md`](doc/40HX-Gen2链路问题说明)。

### ReBAR（8 GiB BAR1 aperture，v29.68 新增）

三个普通 MMIO 写（与 Gen2 同属 XVE 族，**无需 Booter 特权**），booter 一条龙里在算力提权与 Gen2 之后最后执行（booter 不做 SBR，无地址冲突）：

| BAR0 偏移 | 写入 | 作用 |
|---|---|---|
| `0x88724` CYA | `0x30` | XVE override 解锁 |
| `0x88DCC` CFG | selector 7 + bit31 | 可选尺寸表 `0x400` → `0x7fc00`（8 GiB 进入列表） |
| `0x88BC0` RBAR-CTL | bits[13:8]=13 | 标准 PCIe Resizable BAR 控制，选定 `2^13 MiB = 8 GiB` |

仅以 CFG 回读作为解锁成功判据（CYA 是只写 key、CTL 是固件镜像，都不作硬判据），真正失败才回写原值（失败形态 = ReBAR 没开，BAR1 维持 64 MiB，无副作用）。
Linux 侧接手：`NVreg_EnableResizableBar=1`（原生驱动模块参数，非改驱动）完成 BAR1 重分配，
内核命令行还需 `pci=realloc` 允许内核重排 BAR、撑大上游桥窗口。
这两项前置由 `install-efi.sh` 安装时自动幂等补齐（已有则不动），`uninstall-efi.sh` 对称回收。
寄存器值移植自驱动补丁 `0003-cmp40hx-rebar-unlock`（来源：驱动补丁方案 `40HX-Unlock-Drive-Patch`，仅溯源说明，本目录构建不依赖它）；
“UEFI 解锁 + 原生驱动 resize”这个组合上游未验证，以本机实测为准。

---

## 构建

依赖 `gcc` / GNU `ld`（支持 `-mi386pep`）/ `objcopy` / `python3`。
两个方案项目完全独立：booter 固件资产已自包含在本目录 `assets/` 里，
同步/拷贝只需要 `40HX-Unlock-Pve-Uefi-Grub/` 这一个目录就能 `make`：

```bash
apt install -y build-essential binutils python3
make release                      # 一次产出两个模式到 build/efi/ 并刷新 build/efi/CHECKSUMS.sha256
( cd build/efi && sha256sum -c CHECKSUMS.sha256 )
```

单独构建某个模式：

```bash
make clean && make                          # build/efi/NVPermissiveEFI-INSPECT.efi（inspect）
make clean && make AUTO=REQUEST_BOOTER      # build/efi/NVPermissiveEFI-BOOTER.efi（booter）
```

仓库里 `build/efi/` 下的预编译 `.efi` **不会**自动包含源码改动。改了源码就用
`sh install-efi.sh --build`（或交互模式选 1）重新编译并安装。

`ld` 若报 `unrecognised emulation mode: i386pep`，改用 mingw 交叉工具链：

```bash
make release CC=x86_64-w64-mingw32-gcc LD=x86_64-w64-mingw32-ld \
             OBJCOPY=x86_64-w64-mingw32-objcopy
```

---

## 安装

```bash
sh install-efi.sh          # 交互：先选 1(编译安装) / 2(预编译安装)，其它输入退出
reboot
```

全自动等价写法：

| 命令 | 行为 |
|---|---|
| `sh install-efi.sh --build` | 先 `make release` 再安装 |
| `sh install-efi.sh --prebuilt` | 直接用现成的 `.efi`（默认） |
| `sh install-efi.sh --force --default=booter --timeout=5` | 用现成 `.efi`，已装则直接重装，并把默认项设为 booter |
| `sh install-efi.sh --force --timeout=5` | 不给 `--default` 也一样：收尾发现 `GRUB_DEFAULT` 没指向本方案（出厂的 `=0` 也算）就自动补 `booter` |

默认项检查是幂等的：已指向本方案（不管 inspect 还是 booter）就不动；被有意设成另一个真实存在的条目
也尊重不动，只提示怎么改。要让开机保持普通 PVE（不解锁）加 `--no-default`。

只有 `--build` 才编译；其余情况一律用仓库里现成的 `.efi`，不需要编译环境。
安装时会自动幂等补齐两个 Linux 侧 ReBAR 前置（`/etc/modprobe.d/nvidia-rebar.conf`
与 `/etc/default/grub` 的 `pci=realloc`），卸载脚本对称回收，无需手工配置。
安装细节、菜单项说明、无头机设默认项、回退步骤全部在操作手册里。

---

## 验证

```bash
# 链路：双端都应是 Speed 5GT/s, Width x16
GPU=$(lspci -D -nn | awk '/10de:1f0b|10de:220d/{print $1; exit}')
lspci -vvv -s "$GPU" | grep -E 'LnkCap:|LnkSta:|LnkCtl2:'

# 驱动/算力
nvidia-smi
```

FP16 用 CUDA 基准（HGEMM / pytorch `float16 matmul`）实测，解锁成功约 50 TFLOPS 档。
受 Gen2 x16（~6.4 GB/s）与 8 GB 显存限制，别拿它和桌面卡比。

```bash
# ReBAR：BAR1 应为 8 GiB（EFI 阶段先解锁 aperture，Linux 侧驱动完成重分配）
lspci -vvv -s "$GPU" | grep -A8 'Physical Resizable BAR'
nvidia-smi -q | grep -A2 'BAR1 Memory'      # 期望 Total : 8192 MiB

# Linux 侧两个 ReBAR 前置（install-efi.sh 已自动幂等写入，下面是手工核查命令）
grep -h '^options nvidia' /etc/modprobe.d/nvidia-rebar.conf   # ① NVreg_EnableResizableBar=1
grep 'pci=realloc' /etc/default/grub                          # ② 内核命令行参数
# 手工改过 /etc/default/grub 后必须重跑 install-efi.sh 重新烘焙 UNLOCK 条目，
# 单独 update-grub 对已烘焙的内核行无效。
```

版本确认：开机进 booter 时屏幕（或 `BOOTER_TRACE.TXT`）应出现
`Core loaded below 2 GiB: 40hx core v29.69 (inspect+rebar)`；看不到 = ESP 里仍是旧产物，用 `--build` 重装。

---

## 文档索引

| 文档 | 写什么 | 什么时候看 |
|---|---|---|
| `README.md`（本文） | 项目构成、代码文件职责、硬件环境、原理速览 | 第一次了解这个项目 |
| `doc/CMP40HX-Unlock-操作手册.md` | 从装机到验收的完整步骤、参数详解、故障分流表、回退 | **动手操作时** |
| `doc/CMP40HX-Unlock-项目总结.md` | 架构、执行链路、core 嵌入机制、Gen2 全链路、验证与风险 | 想搞懂「为什么能这样」 |
| `doc/CMP40HX-Gen2链路问题说明.md` | Gen2 从卡住到跑通的完整实验记录与根因分析 | 换平台复现、或 Gen2 又不通时 |

---

## 安全边界

- 不写 VBIOS / SPI ROM，不动 eFuse，不改主板 BIOS —— 全是运行时寄存器写；
- 完全易失：每次上电由 GSP 重新初始化，本方案只是在 GSP 之前抢先写一遍；
- 最坏情况：EFI 阶段失败（找不到卡 / BOOT0 不符 / SBR 后 ID 校验失败）→
  返回失败、跳过解锁按原条目进 PVE，不会卡死也不会变砖；
- SBR 是标准 PCI 二次总线复位，等效拔插卡，且发生在内核加载之前；
- 三级回退：改 `GRUB_DEFAULT` 回普通条目 → `uninstall-efi.sh` 彻底移除 → 冷断电回工厂状态。

---

## 许可

MIT。上游 NVPermissiveEFI 的版权与许可见 [LICENSE](LICENSE)；
寄存器目标值仅与公开资料做数值核对，无代码复制。
