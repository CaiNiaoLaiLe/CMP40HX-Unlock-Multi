# CMP 40HX / PVE：UEFI 前置解锁拿 PCIe Gen2 的问题与解决记录

> 用途：把「UEFI 阶段拿不到 PCIe Gen2」这个问题、做过的实验、最终的根因和修法一次性讲清楚。
> 结构：§0 结论摘要 → §1 环境 → §2 现象 → §3 已做过的实验 → §4 根因与修法。
> **结论：已解决（2026-11-08）。根因是链路两端 `LnkCtl.CCC` 不一致，双端统一为 0 后一次重训即达标。**
> §2、§3 保留的是排查当时的原始记录（包括后来被推翻的判断），换平台复现时按这个路径对照即可。

---

## 0. 一句话

UEFI 阶段写 `CFG(0x8c040)[19:18]=2`、`PL(0x8c1c0)[18:17]=2` 能写进去（回读确认），
`LnkCap` 也成功抬到 `00453d02`、`LnkCtl2.Target` 也锁到了 5GT/s，
但链路永远停在 2.5GT/s x16——表现为「训练完成、无报错、速率不升」。

策略位、GSP、devinit 时序、信号完整性逐一排查都对不上。
**真正的原因是 `LnkCtl.Common Clock Configuration`（bit6）在链路两端不一致。**
UEFI 阶段把它对齐成两端都 = 0 之后，第一次 retrain 就上 Gen2 x16，进系统后保持，
不需要任何驱动补丁。

> 只想看怎么修：直接跳 §4.4。
> 同一张卡上，打过 `0002-cmp40hx-pcie2-unlock.patch` 的驱动也能到 5GT/s x16，
> 但那条路需要改 `nvidia.ko`；本方案在 UEFI 阶段把同一件事做完，不动任何系统组件。

---

## 1. 环境

| 项 | 值 |
|---|---|
| 系统 | Proxmox VE 8.4，内核 `6.8.12-43-pve`（Debian 系，UEFI + GRUB） |
| CPU/芯片组 | Intel Xeon E7 v4 + C224；BIOS 里 Above 4G Decoding 已开 |
| GPU | `0000:05:00.0`，`10de:1f0b`（TU106 CMP 40HX），class `0302`（3D controller），rev `a1`，无显示输出 |
| 上游桥 | `0000:00:03.0`（Intel Xeon E7 v4 IIO PCIe Root Port 3），单跳直连 |
| 亮机卡 | AMD Caicos `0000:01:00.0/01`，挂在 `00:01.0`，**不同 root port**，与本问题无关 |
| 驱动 | NVIDIA 615.78.08（`nvidia` / `nvidia_modeset` / `nvidia_drm` 均已加载） |
| 解锁方式 | UEFI 阶段（GRUB `chainloader` 一个 EFI 程序）写算力寄存器 + SBR + Gen2 策略 |

算力解锁是有效的（`nvidia-smi` 正常、CUDA 可用、8192MiB 显存全可见）。问题只在链路速率。

---

## 2. 现象

### 2.1 端点（GPU）

```text
LnkCap:  Port #0, Speed 5GT/s, Width x16, ASPM L0s L1 ...
LnkCap2: Supported Link Speeds: 2.5-5GT/s
LnkCtl2: Target Link Speed: 5GT/s, EnterCompliance- SpeedDis-
LnkSta:  Speed 2.5GT/s (downgraded), Width x16
LnkSta2: Current De-emphasis Level: -6dB, EqualizationComplete- EqualizationPhase1-
LnkSta 原字 = 0x1101   → speed=1(2.5GT/s), width=16, bit10 TrainingError=0, bit11 Training=0, bit12 SlotClk=1
LnkCap2 原字 = 0x00000006
LnkCtl2 原字 = 0x0002  → TLS=2 (5GT/s)，HASD(bit5)=0
```

### 2.2 上游桥（root port）

```text
LnkCap:  Port #7, Speed 8GT/s, Width x16, ASPM L1
LnkCap2: Supported Link Speeds: 2.5-8GT/s
LnkCtl:  ASPM L1 Enabled; RCB 64 bytes, Disabled- CommClk+
LnkCtl2: Target Link Speed: 5GT/s, SpeedDis-
LnkSta:  Speed 2.5GT/s, Width x16
LnkSta 原字 = 0x7101 → speed=1, width=16, bit12 SlotClk=1, bit13 DLL=1,
                        bit14 Link BW Management Status = 1  ← 说明带宽变更事件发生过
```

### 2.3 GPU BAR0 内 Gen2 策略寄存器（本项目工具读出）

```text
BOOT0 = 0x166000a1      （高 16 位 0x1660 = 已解锁状态）
OVR   = 0x00000006      （= 6，本应打开 Gen2 覆盖）
CFG   = 0x800c5800      → [19:18] = 3   （期望 2）
PL    = 0x00220036      → [18:17] = 1   （期望 2）
MISC  = 0xe0b40d00
CYA   = 0x068731b3      → bit2 (DIS_G2) = 0，Gen2 未被禁用
SS0   = 0x04110000
SS1   = 0x00000006
```

**要点**：能力（5GT/s）、目标速率（双端 TLS=5）、去加重（-6dB）都齐了，重训也没有报 `TrainingError`，但链路就是回到 2.5GT/s。

> 注意：上面这条「参考时钟 `CommClk+` 已就绪」是**当时的判断，后来被推翻**——
> 上游那个 `CCC=1` 本身就是错的，照它对齐反而永远停在 Gen1，详见 §4.3。

---

## 3. 已做过的实验（全部在本机实测）

| # | 做法 | 结果 | 判据 |
|---|---|---|---|
| 1 | **UEFI 阶段**（EFI 程序里）写 5 个策略寄存器 + 双端 `LnkCtl2.TLS=5GT/s` + 上游 `Retrain Link` | 屏幕输出 `gen2 policy: OVR=00000006 CFG=80085600 PL=00340036 MISC=20342d00 CYA=068711b3`（**回读确认当时是 2/2**）；随后 `gen2: endpoint LnkSta=1101`、`Gen2 not confirmed at UEFI stage` | UEFI 里写成功，但生效不了 |
| 2 | 进系统后用纯 `setpci` 改配置空间：双端 `LnkCtl2.TLS`（写死字面值 `0xa8.w=0x0002` / `0xc0.w=0x0002`）+ 双端 `Retrain Link`，按 `0/5/15/35/75/135` 秒退避重试六轮 | 六轮全部 `LnkSta=0x1101`；`SpeedDis-` 已确认、写入确实落地；`TrainingError=0` | 纯配置空间操作无效 |
| 3 | 用户态写 BAR0（sysfs `resource0` 映射）：门禁 → `OVR=6` → 双端 TLS → 上游 RL → 20×100ms 轮询 | `rc=1`；`OVR` 回读 6 ✓；链路仍 `1101` | 单独写 OVR 无效 |
| 4 | 同上，但先 `--force-policy` 把 `CFG[19:18]`、`PL[18:17]`、`CYA.DIS_G2` 在**运行时**写成 2/2，再写 OVR + 重训 | `CFG 0x800c5800 → 0x80085800`、`PL 0x00220036 → 0x00240036`（回读确认，5 分钟后仍是 2/2）；`OVR` 回读 6 ✓；链路**仍** `1101`，`rc=1` | 策略位可写、不回退，但链路不跟 |
| 5 | 关掉 GSP 固件（`NVreg_EnableGpuFirmware=0`，`dmesg` 无 gsp 加载，`nvidia-smi` 仍正常）后重读 7 个寄存器 | 与 GSP 开启时**一字不差**（`CFG=0x800c5800`、`PL=0x00220036`、`OVR=6`、`CYA=0x068731b3`…） | 3/1 不是 GSP 写的，与驱动路径无关 |
| 6 | `options nvidia NVreg_RegistryDwords="RmForceEnableGen2=1;RMPcieLinkSpeed=0x1"` | `/proc/driver/nvidia/params` 能看到该串；寄存器**一个位没变** | 键名无公开出处，RM 对未知 key 静默忽略 |
| 7 | Link Disable / Secondary Bus Reset 催 Gen2 | 本机**未试，且不建议**：无显示输出的宿主上会掉 GSP/RM，只能断电救。后来在源码里实现为 `gen2-ld` 模式，CCC 对齐后已不需要，不生成菜单项 | — |

`LnkSta` 上游 bit14（Link BW Management Status）=1，说明**重训确实发生过**（带宽变更事件），只是回落到了 Gen1。

---

## 4. 结论：已解决（2026-11-08 实测）

### 4.1 一句话

不是 `CFG`/`PL`，不是 GSP，也不是 devinit 时序。是
**`LnkCtl.Common Clock Configuration`（bit6）在链路两端不一致**。
UEFI 阶段把它对齐成两端都 = 0 之后，**第一次 retrain 就上 Gen2 x16，进系统后保持**。

### 4.2 决定性实验

`compute` 条目里在 `apply_gen2()` 中自动跑两轮时钟模式，屏幕输出：

| 轮次 | 双端 `LnkCtl.CCC` | GPU `LnkSta` | 上游 `LnkSta` | 结果 |
|---|---|---|---|---|
| round 1 | 都 = 1（镜像上游） | `1101` spd=1 | `3101` spd=1 | 失败 |
| round 2 | 都 = 0（separate clock） | **`1102` spd=2** | **`3102` spd=2** | **成功** |

> 这是当时那次实验的输出（代码当时是「先 1 后 0」）。现在两轮顺序已倒过来，
> **先试 CCC=0，一次命中**；CCC=1 只作为「平台真的共时钟」时的兜底。

关键细节：round 2 的 `gpu tgt` 那一行（刚写完、还没轮询）就已经是 `1102`。
写入即刻生效，不存在「训练慢需要多等」。所以之前所有「环境都就绪但训练不过」的观察，
都缺了同一个前提。

### 4.3 根因

PCIe 规范要求 `LnkCtl.CCC` 在链路两端相同。本机的原始值：

| 端 | 原始 `LnkCtl` | CCC | 同时声明的 `LnkSta.SlotClk` |
|---|---|---|---|
| 上游 root port（`00:03.0`） | `0042` | **1** common clock | bit12 = 1 |
| GPU 端点（`05:00.0`） | `0000` | **0** separate clock | bit12 = 1 |

两端都说「用插槽时钟」，CCC 却相反。这个组合下重训 Gen2 的表现是：
**无 `TrainingError`、`DLL`(bit13) 保持 active、`LBMS`(bit14) 置位、速率不变**。
`LBMS` 被置上是因为带宽变更流程跑过一轮但没升速，之前被误读成「重训失败」。

实质是 clock skew 预算超出了 Gen2 的容限；Gen1 宽容度大，所以 2.5GT/s 一直正常。
上游 root port 自报的 `CCC=1` 在本平台是**误导性的**——按它设置反而永远停在 Gen1。

### 4.4 完整序列（UEFI 内，驱动加载前）

1. 写 policy：`OVR=6`、`CFG[19:18]=2`、`PL[18:17]=2`、`MISC`、`CYA.bit2=0`；
2. `Stall(200ms)` 让端点把新能力报出来（`LnkCap` 低 4 位 `1` → `2`）；
3. 双端 `LnkCtl2 = TLS(2) | HASD`；
4. 双端 `LnkCtl`：清 `ASPM`、**`CCC` 统一为 0**；
5. 上游 `LnkCtl |= HAWD | Retrain`；
6. 上游写 1 清 `LnkSta.LBMS`；
7. 轮询双端 `spd==2`。

与之前的版本相比只多做了 4 件小事（CCC 对齐、HASD、HAWD、清 ASPM/LBMS），
其中 **CCC 对齐是唯一决定性的**；其余三件是加固。

### 4.5 回答设计初期的四个疑问

1. **谁在驱动加载后把 `CFG`/`PL` 写成 3/1？**
   本方案不再关心这件事。策略值可能确实仍被改写，但**已经跑在 5GT/s 上的链路没有被降回去**
   （进系统后 `lspci` 实测双端 5GT/s、`nvidia-smi` 正常）。

2. **能否在 UEFI/固件阶段改变 devinit 的输入？**
   **不需要了。** UEFI 阶段先把链路训练到位，devinit 的 policy 写入不触发 retrain，
   链路保持原速率。
3. **`CFG[19:18]` / `PL[18:17]` 的语义？**
   仍然是「能力/策略门禁」（必须 = 2 才能把 5GT/s 能力报出来，本机 `LnkCap` 由
   `00453d01` 变 `00453d02` 可证），但它**不是「能否训练成功」的唯一条件**。
4. **驱动补丁能成的关键是什么？**
   本次没有复现驱动补丁路径，无法给出机制层面的结论。但可以确定的是：
   **不需要驱动补丁也能到 Gen2**，那条路对本机不再是必需品。

### 4.6 当前状态与注意事项

- 每次开机选 `compute` 条目即可（寄存器易失，本方案本来就是这样），无需任何额外操作；
- 进系统后 `lspci` 双端 `Speed 5GT/s, Width x16`，上游 `LnkCtl: ASPM Disabled; CommClk-`；
- **`CommClk-`（CCC=0）是解锁后的正确形态，不要「修」成 `CommClk+`**，那会打回 Gen1；
- `gen2-ld`（上游 Link Disable 强制重训）已不需要，菜单项已移除，仅源码保留；
- 代码位置：`src/main.c` 的 `apply_gen2()`；安装脚本 `install-efi.sh`。
