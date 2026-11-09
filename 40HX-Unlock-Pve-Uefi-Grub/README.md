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
| `src/main.c` | UEFI 应用主体：模式解析、PCI 扫描定位 `10de:1f0b`、选最近的上游桥、解码 BAR0/BAR1、BOOT0 校验、SBR、`apply_gen2()` 链路重训、屏幕输出、`efi_main` |
| `src/core_40hx.c` | 真正写算力寄存器的核心（XP3G / FECS_PLM / SS0 / SS1 / consumer chain），编译成 ELF 后嵌入 `.efi`，运行时加载执行 |
| `src/elf_loader.c` | 把嵌入的 SysV ELF 加载到 2 GiB 以下，处理重定位并用 trampoline 接上外部符号 |
| `include/core_abi.h` | core 与 UEFI 宿主之间的 ABI，含 `_Static_assert` 的偏移/长度校验（`platform_data`=112、size=120） |
| `include/uefi_min.h` | 不依赖 EDK2 的极简 UEFI 类型与协议定义 |
| `include/elf64_min.h`、`include/elf_loader.h` | ELF64 结构与加载器接口 |
| `Makefile` | 一次产出三种模式：core 编译 → `objcopy` 包成 `.core` 段 → `ld -mi386pep` 链接 → `verify_efi.py` 校验 |
| `install-efi.sh` | 安装器：三个 `.efi` 拷进 ESP、生成 `/etc/grub.d/40-cmp40hx`（3 个菜单项）、`update-grub`；支持交互选「1 编译安装 / 2 预编译安装」，或 `--build` / `--prebuilt` / `--force` / `--default` / `--timeout` |
| `uninstall-efi.sh` | 删除菜单项与 ESP 目录，回到工厂状态 |
| `tools/verify_efi.py` | 校验产物是合法的 PE32+ / x86-64 / subsystem=10（EFI 应用） |
| `CHECKSUMS.sha256` | 三个 `.efi` 的 SHA256，`make release` 会刷新 |
| `NVPermissiveEFI.efi` | inspect 模式（无 LoadOptions 时的兜底） |
| `NVPermissiveEFI-COMPUTE.efi` | compute 模式 |
| `NVPermissiveEFI-FULL.efi` | full 模式 |
| `doc/` | 四份文档，见文末「文档索引」 |

---

## 运行模式

| 模式 | 载体 | 行为 |
|---|---|---|
| `inspect`（默认） | `NVPermissiveEFI.efi` | 只读：打印 BOOT0 / 各寄存器现值，不写、不复位，零风险 |
| `compute` | `NVPermissiveEFI-COMPUTE.efi` | 全套算力寄存器 + 1 次双重 SBR + Gen2 重训，日常用这个（开机约慢 30 秒） |
| `full` | `NVPermissiveEFI-FULL.efi` | compute → graphics → gr-reset → gr-acr，8 次单 SBR，对齐 Windows 完整解锁（约 85 秒） |

`gen2` 与 `gen2-ld` 两个模式仍保留在源码里（靠 GRUB 传参触发），但**不生成菜单项**：
`compute` 末尾已经包含 Gen2 策略与重训，单独再跑一次没有意义；
`gen2-ld`（上游 Link Disable 强制重训）在 CCC 对齐后已不需要，且断链有风险。

---

## 原理速览

### 算力寄存器（BAR0）

| 偏移 | 写入值 | 作用 |
|---|---|---|
| `0x008E1B0` XP3G_PLM | `0xFFFFFFFF` | 功耗/性能锁 PLM 清除 |
| `0x409650` FECS_PLM | `0xFFFFFFFF` | FECS 性能锁清除 |
| `0x409664` SS0 | `0x88888888` | SM 速度选择子（Tensor/FP16 的核心） |
| `0x40966C` SS1 | `0x00000008` | 配套状态位 |
| `0x0088610` XVE_08610 | `0x00001001` | 链路配置（与 RTX 2070 vBIOS 同值） |
| `0x409500` / `0x40960C` / `0x409610` / `0x409614` | `0xFFFFFFFF` | consumer chain |

`graphics` 阶段追加 `0x419B54=0xFF`、`0x600000=0xFFFFFFFF`（显示探测，无害）。
SBR = 两次 PCI 二次总线复位（桥 `CTL 0x3e` 位 `0x40`），间隔 100 ms 沉降。

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

---

## 构建

依赖 `gcc` / GNU `ld`（支持 `-mi386pep`）/ `objcopy` / `python3`：

```bash
apt install -y build-essential binutils python3
make release                      # 一次产出三个模式并刷新 CHECKSUMS.sha256
sha256sum -c CHECKSUMS.sha256
```

单独构建某个模式：

```bash
make clean && make                       # NVPermissiveEFI.efi
make clean && make AUTO=REQUEST_COMPUTE  # NVPermissiveEFI-COMPUTE.efi
make clean && make AUTO=REQUEST_FULL     # NVPermissiveEFI-FULL.efi
```

仓库里的预编译 `.efi` **不会**自动包含源码改动。改了源码就用
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
| `sh install-efi.sh --force --default=compute --timeout=5` | 用现成 `.efi`，已装则直接重装，并把默认项设为 compute |

只有 `--build` 才编译；其余情况一律用仓库里现成的 `.efi`，不需要编译环境。
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
