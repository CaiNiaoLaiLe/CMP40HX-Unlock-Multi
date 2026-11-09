# CMP 40HX 项目总结与 PCIe Gen2 排查实录

> 平台：PVE 8.4（Debian 12）/ 内核 `6.8.12-43-pve` / 主板 `D1581_R3` / GPU `0000:05:00.0`
> 记录时间：2026-11-08
> 现象：算力解锁成功，PCIe 链路停在 Gen1 x16（2.5 GT/s）
> 结论：**不是破解失败，是驱动内重训练的执行时机太早；延迟补执行一次即成功。**

---

## 目录

1. 项目定位
2. 三条安装路线（勿混用）
3. 仓库结构与关键文件
4. 核心技术原理（算力 / PCIe Gen2 / ReBAR / Vulkan，含流程图与寄存器级细节）
5. 排查方法论（这一章是本实录的核心）
6. 本次问题的现场信息
7. 完整日志与逐条解读
8. 问题定性过程（5 步，含每步的判断依据）
9. 根因
10. 解决方案（脚本 + 开机自启）
11. 验收与后续观察项
12. 边界、风险与「不要做的事」
13. 附录：全部命令速查

---

## 1. 项目定位

针对 NVIDIA CMP 40HX（TU106，PCI ID `10de:1f0b`，8 GB 显存）的运行时解锁项目。CMP 系列是矿卡，出厂被固件/安全初始化流程限制；本项目**不刷 VBIOS**，全部在运行时打开，因此：

- **易失性**：断电或 GPU 复位后全部失效，每次开机都要重新执行。
- **可逆**：`install.sh --uninstall` 只删除自己登记过的模块文件。

四类解锁：

| 能力 | 内容 | 层面 |
|---|---|---|
| 算力 | 恢复 Tensor Core / INT8 dp4a / CUDA FP32・FP16 FMA | SEC2 + GSP Booter 固件 |
| PCIe Gen2 | 2.5 GT/s → 5 GT/s（x16 带宽 3.4 → 6.8 GB/s） | GSP/RM 策略 + PCI 配置空间 |
| ReBAR | BAR1 从 64 MiB 到 8 GiB | XVE 寄存器 + 选择子 |
| Vulkan 节流 | 去掉 `vkCmdBindPipeline` 的 240 次 NOP/WAIT | 用户态 `libnvidia-glcore.so` |

注意两点常见误解：

- **没有全局 FP32 锁**。原厂卡走 cuBLASLt / OpenCL / Vulkan 本来就是 8.3–9 TFLOPS，解锁前后不变；放开的是 Tensor Core（约 63 倍）、INT8（约 33 倍）、clpeak 的 CUDA 标量循环（约 3 倍）。
- **Gen3 未实现，且已论证在可及层面做不到**：端点在任何可及时机都只 advertise Gen1，策略写入后 RM 会规范化成 Gen2，`LCAP`/`LCAP2` 对软件只读，`0x88050` 在 Booter 之前就已锁死。

---

## 2. 三条安装路线（勿混用）

| 路线 | 驱动 | GSP | 算力 | Gen2 | ReBAR |
|---|---|---|---|---|---|
| **A. 开源内核模块补丁**（`install.sh`） | `open-gpu-kernel-modules` 580.173.02 / 610.57.04 / **615.78.08（默认）** | 开 | `0001` 补丁 | `0002` 补丁 | `0003` 补丁 |
| **B. 专有驱动 CPU-RM**（`linux/cpu-rm/`） | NVIDIA 专有模块，`NVreg_EnableGpuFirmware=0` | **关** | `cmp40_preboot.ko` 在驱动加载前跑 | `gen2.sh` 在原生初始化后跑 | ✗ |
| **C. Windows / UEFI** | Windows 驱动 | 关 | `40HXUNLK.EFI` | WinRing0 提链路 | EFI 内 |

**A 与 B 互斥**：A 打 NVIDIA 开源模块，B 完全不改驱动而是自己在驱动加载前跑 Booter。本次用的是 A。

---

## 3. 仓库结构与关键文件

```
cmpunlocker-main/
├── install.sh                       # 路线A 一键安装：下载→SHA256→打补丁→编译→装模块→initramfs
├── patches/580.173.02 | 610.57.04 | 615.78.08
│   ├── 0001-cmp40hx-unlock.patch              # 算力（615 版内嵌 610 签名 Booter）
│   ├── 0002-cmp40hx-pcie2-unlock.patch        # Gen2 正式版
│   ├── 0002-cmp40hx-pcie2-diagnostic.patch    # Gen2 诊断版（与正式版二选一）
│   └── 0003-cmp40hx-rebar-unlock.patch
├── linux/cpu-rm/                    # 路线B（unlock.sh / gen2.sh / kernel/*.c）
├── windows/                         # 路线C
├── tools/
│   ├── cmp40hx-driver-reprobe-gen2.sh       # 只做 unbind/bind，latch 端点能力位
│   ├── cmp40hx-alder-lake-link-disable-test.sh  # 危险实验，永久排除
│   ├── check-linux-patches.sh               # 校验补丁能否干净应用
│   └── check-cmp40-booters.py
├── cmp_glcore_patch/                # Vulkan 节流用户态补丁
└── docs/                            # 安装/原理/兼容性/结果 + docs/research/ 11 篇
```

安装产物：

- 模块：`/lib/modules/$(uname -r)/updates/cmpunlocker/{nvidia,nvidia-uvm,nvidia-drm,nvidia-modeset,nvidia-peermem}.ko`
- 清单：`/var/lib/cmp40hx/install-<kernel>.manifest`（记录驱动版本、源码 SHA256、每个补丁与模块的哈希、`pcie_mode`）

---

## 4. 核心技术原理

这一章讲「锁在哪一层、用什么手段打开」。四个解锁作用层完全不同，**写错层，任何命令都不会生效**。

### 4.1 三层锁：四个解锁分别作用在哪一层

| 层 | 谁在管 | 限制内容 | 能用什么改 |
|---|---|---|---|
| 固件安全初始化层（FWSEC / SEC2 / Booter / GSP-RM） | 卡上的 Falcon 安全处理器 + NVIDIA 签名固件 | SM 速度选择子、FECS 权限、Tensor/INT8 路径、PCIe 速率策略、BAR 尺寸 | **只能在固件执行上下文里改**（本项目注入签名 Booter，或改驱动模块） |
| GPU 内部寄存器层（BAR0 MMIO） | XVE / FECS / PFALCON | 上述状态的落地点 | 特权态 MMIO 写；部分受 SEC2 保护，用户态写不进 |
| 标准 PCI 配置空间 | PCIe 规范定义 | `LnkCap` / `LnkCap2` / `LnkCtl2` / `LnkSta` | `setpci` 可读写，但只能改「协商参数」，改不了端点自称的能力 |

四个解锁的分层归属：

```
算力(SM/Tensor)   ◄─ 固件层：SEC2 Booter 特权上下文（0001）
PCIe Gen2 策略     ◄─ 固件层：GSP-RM bootstrap（0002 前半）
PCIe Gen2 重训练   ◄─ 驱动层：nv_open + 标准 PCIe 配置空间（0002 后半）
ReBAR 8 GiB        ◄─ 驱动层：PCI probe 时改 XVE 选择子 + 内核 BAR 重分配（0003）
Vulkan 节流        ◄─ 用户态库：libnvidia-glcore.so（cmp_glcore_patch）
```

### 4.1.1 Linux 启动时序：四个解锁点各自落在哪一步

这是本章最重要的一张图。**横轴是时间，每一阶段都标了「谁在执行 / 做了什么 / 本项目在哪里插手 / 用什么日志锚点验证」。**

```
════════════════════════════════════════════════════════════════════════
 T0  UEFI / BIOS POST                                    （固件，非 Linux）
════════════════════════════════════════════════════════════════════════
 做什么：训练 PCIe 链路到「本槽允许的最高速率」、枚举设备、分配 32 位/64 位 MMIO 空间
 ★ 前置条件（不做后面全白费）：
   · Above 4G Decoding = Enabled      → 否则 8 GiB BAR1 无处安放
   · CSM = Disabled / UEFI 纯模式      → 否则大 BAR 分配不稳定
   · 该槽 PCIe Speed = Auto/Gen2       → 若被钉死 Gen1，后面策略再写也不生效
   · Resizable BAR = Enabled（若有）
 观测：无（BIOS 内设置）。进系统后 `lspci -vv` 的 LnkCap 能反映槽的最大能力。
────────────────────────────────────────────────────────────────────────
 T1  Linux 内核启动：PCI 枚举
════════════════════════════════════════════════════════════════════════
 做什么：pci_scan_bus 发现 05:00.0 → 读 BAR 尺寸 → 分配地址
   · BAR0 映射出一段 MMIO 窗口（XVE 段在 +0x88000，落在其中，可 ioremap）
   · BAR1 初始 64 MiB aperture（ReBAR 未开）
   · 链路此时是 VBIOS 建好的 Gen1 x16（2.5 GT/s）
 ★ 此阶段还没有任何解锁，全是标准 PCI 行为
 观测：`lspci -vv -s 05:00.0 | grep BAR`（BAR 1 = 64MiB）、`LnkSta 2.5GT/s`
────────────────────────────────────────────────────────────────────────
 T2  nvidia.ko probe（udev / systemd-modules-load / initramfs 触发）
     nv_pci_probe() → nv_resize_pcie_bars() → nv_start_device()
════════════════════════════════════════════════════════════════════════
 T2.1  ◄── 解锁点 ① ReBAR（补丁 0003，nv-pci.c）
       函数：nv_cmp40_unlock_bar1_resize()，从 nv_resize_pcie_bars() 里调用
       做什么：
         ioremap(BAR0 + 0x88000) 拿到 XVE 窗口
         读 CYA(0x724) / CFG(0xdcc) / CAP(0xbbc)
         写 CYA_UNLOCK=0x30（先解锁该组寄存器）
         写 CFG = (原值 & ~size_mask) | ENABLE(bit31) | selector(7)
           → selector 7 叠加在 ReBAR 基码 6 之上 = 2^(6+7) = 8 GiB
         读回校验，不一致则回滚并返回 -EIO
         置 NVreg_EnableResizableBar=1，由内核 pci_rebar_set_size 真正重排 BAR1
       ★ 必须在 RmInitAdapter 之前：驱动的地址映射要按重排后的 8 GiB 建
       观测：`NVRM: CMP40_REBAR: selector=7 mask=…`、`final BAR1 size=8192 MiB`

 T2.2  ◄── 解锁点 ② 算力（补丁 0001，kernel_gsp_booter_tu102.c）
       阶段：RmInitAdapter → FWSEC → kgspBootstrap → s_executeBooterUcode_TU102()
       做什么：加载签名 610.57.04 Booter 到 SEC2 → 栈溢出夺取控制流
               → 特权写 FECS_SPEED_PLM/SS0/SS1 + WPR2 下移
               → 签名原位恢复、DMEM 清零、原生 cleanup 停机
       观测：`CMP40_COMPUTE_UNLOCK_V526: … FECS=0xffffff8f SS0=0x88888888 SS1=8`
             `CMP40_COMPUTE_UNLOCK_V525: INPLACE_STOCK_BOOTER_AFTER_UNLOCK_PASS`

 T2.3  ◄── 解锁点 ③ Gen2 策略（补丁 0002 前半，kernel_gsp_tu102.c 尾部）
       阶段：GSP-RM（gsp_tu10x.bin + LibOS）启动完成、FECS 选择子已可读
       做什么：写 CFG(0x8c040) bits[19:18]=2、PL(0x8c1c0) bits[18:17]=2
               → GPU 内部「最大速率策略」和「物理层速率」都定到 Gen2
       观测：`CMP40_GSP_READY_V551: … ss0=0x88888888 ss1=0x00000008`
             `CMP40_PCIE_GEN2_V2: LNKCFG=…(bits[19:18]=2) PL_RATE=…(bits[18:17]=2) CYA0=…(DIS_G2=0)`
────────────────────────────────────────────────────────────────────────
 T3  probe 结束：设备节点可用（/dev/nvidia0、/dev/nvidiactl…）
     此刻链路仍是 Gen1：策略只改了「允许的最大速率」，物理链路还没重新协商
     观测：`lspci -vv` 仍 `Speed 2.5GT/s`；`CAP2` 可能还是 `00000002`
────────────────────────────────────────────────────────────────────────
 T4  用户态第一次 open("/dev/nvidia0")   ← 真正的触发者
     谁在触发：nvidia-persistenced / udev 规则 / nvidia-smi / 第一个 CUDA 进程
════════════════════════════════════════════════════════════════════════
 T4.1  ◄── 解锁点 ④ Gen2 重训练（补丁 0002 后半，nv.c）
       函数：nv_open() 内、failed_release_irq 之后调用 nv_cmp40hx_retrain_gen2(nv)
       做什么：
         · 门禁：读 CYA/CFG/PL，任一不满足就 "GSP policy not ready; skipped"
         · 写 OVR(0x8872c)=6 → 让 XVE 接受外部 override
         · GPU + 上游桥 LnkCtl2.TLS = 5.0GT/s
         · 上游桥 LnkCtl.LinkControl |= Retrain Link(RL)   ← 触发重新协商
         · 20×100 ms 轮询 GPU LnkSta，≥5GT/s 成功，否则 RETRAIN_FAIL
       ★ 这是全案的事故点：距 T2.3 只有几百毫秒，GSP-RM 的晚策略还在改写 PL 寄存器
       观测：`RETRAIN_PASS status=1102` / `RETRAIN_FAIL status=1101`
────────────────────────────────────────────────────────────────────────
 T5  运行期：CUDA / Vulkan / nvidia-smi
     · 算力：SM 按 SS0/SS1 选择子跑满速（WMMA/INT8 高，FP32 不变）
     · PCIe：Gen2 x16 → 带宽 3.4 → 6.8 GB/s
     · ReBAR：BAR1 = 8 GiB，大显存映射走大窗口
     · Vulkan 经典管线：若装了 cmp_glcore_patch，绑定开销从 240 次 NOP+WAIT 降到 0
════════════════════════════════════════════════════════════════════════
```

**另一条路线（`linux/cpu-rm`）把 ② 挪到哪里：**

```
T1 之后、T2 之前
  blacklist nvidia → insmod cmp40_preboot（占住设备、驱动不 probe）
    → 自带签名 610.43.03 Booter 做同样的 SEC2 注入（1 MiB 小 WPR 堆）
    → 让原生 Booter 跑完、校验选择子/FECS/WPR 拆除、清 SEC2 DMEM、恢复暂存态
    → rmmod cmp40_preboot
T2'  modprobe nvidia + NVreg_EnableGpuFirmware=0（GSP OFF，走 CPU 侧 RM）
    → 专有驱动在「已被解锁」的卡上正常初始化，不再重做 SEC2 注入
```
差别：这条路线**不 patch 驱动、不跑 GSP-RM**，所以 ReBAR 与 Gen2 策略都不生效（Gen2 需另用 CPU-RM 专用 helper）。它换来的是：解锁发生在 probe 之前，时序天然更安全。

### 4.1.2 时序约束：顺序反过来为什么就失败

四个解锁点不是随便放的，每个都被一个时间窗口夹住：

| 解锁 | 必须发生在 | 早了会怎样 | 晚了会怎样 |
|---|---|---|---|
| ① ReBAR（0003） | `RmInitAdapter` 之前（`nv_resize_pcie_bars` 内） | — | 驱动已按 64 MiB aperture 建好映射，再改 BAR 需重建整套地址空间 |
| ② 算力（0001） | SEC2 Booter 的特权执行窗口内 | — | FWSEC 已收尾，FECS 寄存器回到保护态，`setpci`/`devmem` 写不进 |
| ③ Gen2 策略（0002 前半） | GSP-RM 起来之后（策略由 GSP 侧寄存器承载） | 写不进去，日志报 `GSP policy not ready` | 与 ④ 一起被推迟，端点能力迟迟不出现 |
| ④ Gen2 重训练（0002 后半） | 端点 `CAP2` 出现 `0x04` 位 **且** GSP-RM 晚策略收尾之后 | 能力没就绪 → `LnkCtl2` 被截断回 `0000`，无速率可谈 | 无（晚一点只是慢几百毫秒） |

**本次故障就是 ④ 被夹在了「③ 刚做完」与「GSP-RM 晚策略还没收尾」之间**：策略位已 `=2`、门禁能过，但端点的 Gen2 能力镜像与私有 policy 寄存器仍在被固件改写，链路拒绝重新协商。第 5 章的采样法和第 9 章的根因都是围绕这个窗口展开的。

### 4.2 算力解锁：向签名 Booter 注入特权 payload

**为什么不能直接写。** 目标寄存器（`FECS_SPEED0/1`、`FECS_SPEED_PLM`）在 FWSEC 完成后处于 SEC2 保护态，普通内核态或用户态 MMIO 写入会被丢弃或触发保护错误——这正是 `setpci` / `devmem` 在这里无效的原因。

**做法。** 借 NVIDIA 自己签名的 `610.57.04` TU102 Booter——它本来就以最高特权在 SEC2 上运行——利用其内部 DMA 校验函数的一处**栈溢出**夺取控制流，把执行引到我们放在 DMEM 里的 payload。（`linux/cpu-rm` 独立模块路线用的是 `610.43.03` 的同款 Booter，机制一致。）

```
① 申请独立 WPR 与元数据页
   本轮注入单独申请一个 WPR 堆（Linux 1 MiB / Windows EFI 960 KiB）
   把 WPR 起点抬到 0x1fe000000，避开原生 ACR 代码所在的 0x1fdf97000
   且每次启动都换新的物理元数据页 —— SEC2 会缓存第一次的 WPR 元数据页，
   复用会导致第二次启动读不到新签名地址

② 加载签名 Booter 到 SEC2，走正常 GSP 引导路径
   （注入用 610.43.03，起 GSP 仍用本机原生 615 Booter）

③ 触发溢出：DMA 校验函数的栈被写到受控 DMEM

④ 栈迁移 + 返回地址改写（关键，保证签名校验不被破坏）
   gadget 0x64ec（mov sp, r6 类）把栈切到受控 DMEM 0xc100
   gadget 0x0c1d 改写已消费的返回槽 0xff4c → 正常调用点 0x277b
   在 0xff14 重建校验器栈帧，从运行期 PC 0x3988 继续
   —— 不跳过校验器尾部：签名 Booter 会跑完自己剩余的两项安全检查后自然返回

⑤ 特权态写寄存器（payload 的真正目的）
   FECS_SPEED_PLM  (0x409650) = 0xFFFFFF8F          恢复原厂满速权限掩码
   FECS_SPEED0     (0x409664) = 0x88888888          SS0 速度选择子
   FECS_SPEED1     (0x40966C) = 0x00000008          SS1
   WPR2_ADDR_LO/HI (0x1FA824/0x1FA828) = 0x1FFFFE00 / 0   放下 WPR2
   SEC2_RESET_PLM  (0x8403C4) = 0x000000FF

⑥ 收尾（"stock relock"，必须与原厂状态完全一致）
   把被 payload 挪动过的签名原位恢复（backing 0xfa00 / advertised 0x1000）
   清 SEC2 两个 mailbox、把整段 DMEM [0x0,0x10000) 清零、清 Falcon GPR
   调用 NVIDIA 原生 booterCleanupAndHalt()（运行期 PC 0x7c43）自然停机
   保留 RESET_PLM=0xff，让驱动走它自己的 bounded reset

⑦ 交棒给原生 GSP-RM
   驱动继续执行原本的 FWSEC / RISCV boot args / in-place stock Booter
   GSP 正常启动，打出 feature=0xf3 ss0=0x88888888 ss1=0x8
```

日志与步骤的对应关系（每一条都能在本机 dmesg 上验证）：

| dmesg 日志 | 对应步骤 |
|---|---|
| `CMP40_BOOTER_COMPAT_V552: UNLOCK_BOOTER=610.57.04 GSP_BOOTER=615.78.08` | ② 注入用 610、起 GSP 用 615 |
| `CMP40_COMPUTE_UNLOCK_V140: fresh WPR metadata …` | ① 独立元数据页 |
| `CMP40_COMPUTE_UNLOCK_V526: signed fullspeed stock-relock WPR-down commit FECS=0xffffff8f SS0=0x88888888 SS1=8 … gadgets=0x0cc8,0x0cbd,0x0c1d,0x1d62,0x1f89,0x5ff,0x64ec` | ③④ 溢出与 gadget 链 |
| `CMP40_COMPUTE_UNLOCK_V526: SIGNED_FULLSPEED_STOCK_RELOCK_WPR_DOWN_PASS` | ⑤⑥ 寄存器已写、WPR 已放下 |
| `CMP40_COMPUTE_UNLOCK_V132: restored stock signature in-place phys=… backing=0xfa00 advertised=0x1000` | ⑥ 签名原位恢复 |
| `s_clearCmp40Sec2ExploitDmem: FULL_DMEM_ZERO_PASS [0x00000000,0x00010000)` | ⑥ DMEM 清零 |
| `CMP40_COMPUTE_UNLOCK_V525: INPLACE_STOCK_BOOTER_AFTER_UNLOCK_PASS … FECS_PLM=0xffffff8f` | ⑦ 交棒成功 |
| `CMP40_GSP_READY_V551: feature=0x000000f3 ss0=0x88888888 readout=0x00000000 ss1=0x00000008` | 最终状态校验 |

**`SS0` / `SS1` 是「速度选择子」**，决定 SM 走哪套速度/功耗许可表；`FECS_PLM` 是权限掩码，`0xffffff8f` 是原厂满速值。三者合起来才是「算力」，**它不是一个 FP32 开关**——所以普通 FP32 GEMM、OpenCL、Vulkan 图形在本项目里本来就满速，解锁前后不变。

实测效果（同一张卡，615.78.08 / 34 SM / sm_75，选择子 ZERO↔FULL 对照）：

| 指标 | 锁定（ZERO） | 解锁（FULL） | 倍数 |
|---|---:|---:|---:|
| FP16 WMMA | 1.044 TFLOPS | 66.16 TFLOPS | 63× |
| cuBLASLt FP16 | 1.034 TFLOPS | 52.13 TFLOPS | 50× |
| INT8 WMMA | 2.072 TOPS | 131.04 TOPS | 63× |
| INT4 WMMA | 4.145 TOPS | 245.95 TOPS | 59× |
| cuBLASLt INT8 | 2.068 TOPS | 84.25 TOPS | 41× |
| 纯 CUDA FP32（标量循环） | 7.914 TFLOPS | 7.475 TFLOPS | 0.94×（不变） |

判读要点：**倍数集中在 Tensor Core 与低精度路径**，FP32 一行几乎不动——这正好印证「锁的是选择子/权限，不是 FP32 单元」。所以如果你测出「Tensor 快了几十倍、FP32 没变」，那是**解锁成功的正常表现**，不是没解开。

### 4.3 PCIe Gen2：三段式（策略 → 能力 → 重训练）

Gen2 需要三个环节全部成立，缺一条就停在 Gen1。**这是本次故障定位的核心框架**。

#### 4.3.1 环节一：GSP 侧策略（补丁 `0002` 的第一个 hunk，打在 `kernel_gsp_tu102.c`）

在 `kgspBootstrap_TU102` 里、`CMP40_GSP_READY_V551` 之后写两个 BAR0 寄存器：

```c
GPU_REG_WR32(pGpu, 0x0008c040, (lnkcfg & ~0x000C0000) | 0x00080000);  // CFG bits[19:18] = 2
GPU_REG_WR32(pGpu, 0x0008c1c0, (pl_rate & ~0x00060000) | 0x00040000); // PL  bits[18:17] = 2
```

含义：把「最大链路速率策略」和「物理层速率策略」都定为 Gen2（编码 2 = 5.0 GT/s）。写完立刻读回并打印：

```
CMP40_PCIE_GEN2_V2: LNKCFG=0x……(bits[19:18]=2) PL_RATE=0x……(bits[18:17]=2) CYA0=0x……(DIS_G2=0)
```

**判读**：两个 `=2` 且 `DIS_G2=0` 才算这一环节成立。若打印的是 `GSP policy not ready` 或 bits 不等于 2，说明策略没写进去（多见于旧的 615 移植树只带了 `nv.c` 半边补丁）。

#### 4.3.2 环节二：端点能力向量（由策略间接产生，不可直接写）

策略生效后，GPU 内部 XVE 状态会镜像到 PCI 配置空间：

| 寄存器 | 正常 Gen2 值 | 未生效值 |
|---|---|---|
| `LnkCap`（`CAP_EXP+0c.L`） | `00453d02`（Speed 5GT/s, x16） | `00453d01`（2.5GT/s） |
| `LnkCap2`（`CAP_EXP+2c.L`） | `00000006`（2.5/5 GT/s） | `00000002`（仅 2.5） |

`0x04` 位是「支持 5GT/s」的标志。**只要 `CAP2` 还是 `00000002`，端点就在对外声称「我不支持 Gen2」，此时任何上游目标速率设置都是无效的** —— 这是本次排查最关键的一条。

#### 4.3.3 环节三：主机侧重训练（补丁 `0002` 的第二个 hunk，打在 `kernel-open/nvidia/nv.c`）

**调用点是 `nv_open()`**（`failed_release_irq` 之后、`rm_get_gpu_uuid_raw()` 之前），意味着它**不在 probe 时执行，而是第一个进程打开 `/dev/nvidia*` 时执行**（`nvidia-smi`、CUDA 程序、persistenced 都会触发）。

函数 `nv_cmp40hx_retrain_gen2()` 的流程：

```
读 CYA(0x8c2c0) / CFG(0x8c040) / PL(0x8c1c0)
├─ 若 CYA bit2 != 0，或 CFG bits[19:18] != 2，或 PL bits[18:17] != 2
│     → "GSP policy not ready; retrain skipped" 并返回
├─ 写 OVR(0x8872c) = 6，读回，msleep(50)
├─ 写 GPU    LnkCtl2.TLS = 5.0GT/s
├─ 写 上游桥 LnkCtl2.TLS = 5.0GT/s
├─ 给上游桥 LnkCtl.LinkControl 置 Retrain Link(RL)
└─ 20 × 100 ms 轮询 GPU LnkSta.CLS
      ≥ 5GT/s → "RETRAIN_PASS status=1102 attempt=N"
      超时   → "RETRAIN_FAIL status=1101"
```

量级参考：`OVR=6` 是 XVE 的 override 控制字；两个 `LnkCtl2.TLS` 是规范定义的目标速率字段；`RL` 是规范定义的「重新协商」触发位。**整个序列只用标准 PCIe 手段 + 一个 GPU 内部 override 字，不做 Link Disable**（断链会掉 GSP/RM，见第 12 章）。

**总窗口只有 2 秒**，且发生在 GSP bootstrap 刚结束的瞬间。本次故障就出在这里（见第 8 章）。

#### 4.3.4 BAR0 寄存器偏移表（诊断版日志字段的含义）

| 日志字段 | BAR0 偏移 | 含义 |
|---|---:|---|
| `OVR` | `0x8872c` | XVE override/control，写入 6 |
| `CAP` | `0x88084` | 端点 Link Capabilities 镜像 |
| `LNK` | `0x88088` | Link Control/Status 镜像 |
| `CAP2` | `0x880a4` | 支持的速率向量镜像 |
| `CTL2` | `0x880a8` | Link Control 2 / Status 2 镜像 |
| `MISC1` | `0x8841c` | PCIe 私有 policy/override 状态 |
| `CFG` | `0x8c040` | 内部最大速率策略 |
| `PL` | `0x8c1c0` | 物理链路速率策略 |
| `CYA` | `0x8c2c0` | 含 `DIS_G2` 位（bit2） |

诊断版（`--pcie-diagnostic`）会在 `before_ovr / after_ovr_immediate / after_ovr_50ms / after_target_writes / retrain_pass|retrain_fail` 五个阶段各打一行 BAR0 镜像和一行 PCI 配置空间镜像，前缀 `CMP40_PCIE_GEN2_DIAG_V1`。**这五行快照是本次定位的决定性工具。**

### 4.4 ReBAR：改 XVE 尺寸选择子，让驱动自己重排 BAR

三步，不碰配置空间的 BAR 编码本身：

```
① 0003 补丁在 GSP bootstrap 里写 XVE 的 BAR1 尺寸选择子 = 7（对应 8 GiB）
② 驱动启动时按这个尺寸做标准的 PCI BAR 重分配（reassign），把 BAR1 从 64 MiB 扩到 8 GiB
③ 验收：nvidia-smi 显示 BAR1 Total 8192 MiB；lspci 显示 BAR 1 为 8GB
```

要点：**尺寸由 GPU 侧选择子决定，重分配由驱动按规范完成**。所以不能靠 `setpci` 直接改 BAR 寄存器——那只会让地址空间和 GPU 内部窗口不一致，轻则 BAR 读写出错，重则驱动初始化失败。这也是为什么 `linux/cpu-rm` 路线（GSP OFF、不改驱动）**不支持 ReBAR**。

### 4.5 Vulkan pipeline 节流：唯一一个用户态补丁

前三项都在固件/内核层，这一项在**用户态库**里，与内核模块无关。

```
触发点：每次经典 vkCmdBindPipeline
  NVC597_CALL_MME_MACRO(52), 参数 0xf0
  → MME 宏内重复 240 次：NVC597_PIPE_NOP + NVC597_WAIT_FOR_IDLE
  → 每次绑定白烧掉一段固定延迟
处理：cmp_glcore_patch 只把脉冲参数 0xf0(240) 改成 0，保留宏调用与命令形状
      610.57.04 特征偏移 0xb20c2d、0xdcbf60（64 位库）
使用：不替换系统库，走 LD_LIBRARY_PATH 局部加载
```

注意：这是**性能节流**，不是算力限制；它只影响经典管线绑定的开销，不影响 SM／Tensor 的实际吞吐。

### 4.6 为什么这些都不能用 `setpci` / `devmem` 直接改

这是排查时最容易走偏的地方，四条硬约束：

| 约束 | 表现 | 后果 |
|---|---|---|
| FECS 寄存器受 SEC2 保护 | 写完不回读、或值不变 | 必须走 Booter 特权上下文 |
| PCIe 能力向量是策略的镜像 | 写 `LnkCap2` 无效，读回不变 | 只能先改策略，再等它镜像出来 |
| 能力未就绪时 `LnkCtl2.TLS` 被截断 | 回读变 `0000` | 看似"写失败"，实为"此刻不接受" |
| BAR 尺寸由 GPU 选择子决定 | 改配置空间 BAR 与内部窗口不一致 | 驱动初始化失败 |

一句话：**PCI 配置空间只能"谈判"，不能"造能力"；能力必须先由固件侧策略造出来。** 本次故障正是卡在「谈判时机」上。


---

## 5. 排查方法论（可以复用到其他硬件解锁问题）

### 5.1 方法总览

| 步骤 | 手段 | 解决的问题 |
|---|---|---|
| 1 | `dmesg` 分层看：补丁日志 vs 驱动日志 | 补丁到底装了没有、跑没跑 |
| 2 | 装诊断版补丁，拿 5 阶段快照 | 失败发生在哪一环 |
| 3 | **同一 boot 内制造第二次初始化**（rebind），对比 `before_ovr` | 能力向量什么时候才就绪 |
| 4 | **能力就绪后手动重放同一串寄存器写**，高频采样 | 链路到底能不能训上去 |
| 5 | 用累积退避补执行 | 落地修复 |

### 5.2 工具与偏移

只用 `pciutils` 两个命令，无需额外工具：

```bash
# 读能力
setpci -s 05:00.0 CAP_EXP+0c.L     # LnkCap
setpci -s 05:00.0 CAP_EXP+2c.L     # LnkCap2（0x04 位 = 支持 5GT/s）
# 读/写控制
setpci -s 05:00.0 CAP_EXP+10.W     # LnkCtl（bit5 = Retrain Link）
setpci -s 05:00.0 CAP_EXP+30.W=0002 # LnkCtl2（低 4 位 = Target Link Speed）
# 读状态
setpci -s 05:00.0 CAP_EXP+12.W     # LnkSta
```

`LnkSta` 字段解位（例 `1102`）：

| 位 | 含义 | `1102` 的值 |
|---|---|---|
| `[3:0]` | 当前速率编码（1=2.5G，2=5G） | 2 ✓ |
| `[9:4]` | 链路宽度 | 16 ✓ |
| `[11]` | Link Training 进行中 | 0（已训完） |
| `[13]` | DLL Link Active | — |

### 5.3 高频采样：判断「有没有真的训练过」

一次成功训练只需要几十微秒，用默认命令只能看到结果。判定「RL 到底有没有被执行」，必须连续采样：

```bash
prev=""
for i in $(seq 1 200); do
  cur="$(setpci -s 05:00.0 CAP_EXP+12.W|tr -d ' ')/$(setpci -s 00:03.0 CAP_EXP+12.W|tr -d ' ')"
  [ "$cur" != "$prev" ] && { echo "$(date +%T.%3N) GPU/UP=$cur"; prev="$cur"; }
  sleep 0.05
done
```

同时采 GPU 和上游根口两端：`LT` 位闪 1、或根口的 DLL Link Active 掉一下，证明训练确实发生；全程一个值不变，说明**根本没有任何训练**（注意，这并不等于"根口拒绝"，也可能是"没有可协商的速率"，见 5.5 的坑）。

### 5.4 快照对比法：判断「能力什么时候就绪」

同一个 boot 里做一次 rebind（等价于第二次 GSP bootstrap），再取 `before_ovr`：

```bash
sudo dmesg -C
echo 0000:05:00.0 > /sys/bus/pci/drivers/nvidia/unbind; sleep 1
echo 0000:05:00.0 > /sys/bus/pci/drivers/nvidia/bind
nvidia-smi; sleep 1
sudo dmesg -T | grep -E 'CMP40_PCIE_GEN2'
```

对比两次 `before_ovr` 的 `CAP2` / `CTL2`：
- 第一次 `02`、第二次 `06` → 能力是**分阶段**出现的，第一次必然失败。
- 两次都 `06` 但都失败 → 才需要怀疑平台/物理层。

### 5.5 三个必须避开的判断陷阱

1. **把「状态寄存器不变」直接判成「平台拒绝重训练」**。本次最初就误判为「Intel 根口不响应 RL」。真相是端点此刻不承认 5GT/s，压根没有目标速率可协商，训练自然什么都不会发生。
2. **把「寄存器写回 0」当成写失败**。回读被截断成 `0000` 是**硬件当前不认可该值**的表现，不代表写入指令有问题。
3. **把 `cmp40hx-driver-reprobe-gen2.sh` 的 `FAIL` 当成故障证据**。该脚本只做 unbind/bind（把能力位 latch 出来），**它自己不发任何 `setpci` 写、不做重训练**，跑完仍显示 `1101` 属于其正常行为。它的门禁甚至会主动拒绝「端点已 advertise Gen2」的情况。

---

## 6. 本次问题的现场信息

| 项 | 值 |
|---|---|
| 系统 | PVE 8.4 / Debian 12 bookworm |
| 内核 | `6.8.12-43-pve` |
| 驱动 | NVIDIA Open Kernel Module `615.78.08`，`/lib/modules/6.8.12-43-pve/updates/cmpunlocker/nvidia.ko` |
| manifest | `pcie_mode=normal`（正式版补丁）、`profiling=disabled` |
| GPU | `0000:05:00.0`，`10de:1f0b`，ASUS `1043:8804`，`Physical Slot 0-4` |
| VBIOS | **`90.06.67.00.06`**（不是社区反复出问题的 `…00.04`） |
| 上游桥 | `0000:00:03.0`，CPU 直连根口，`Port #7`，`8GT/s x16` |
| 主板 / BIOS | `D1581_R3` / `5.11` / `2021-11-17`（双路 Intel 服务器板） |
| 启动参数 | `intel_iommu=on iommu=pt pcie_aspm=off` |
| 直通 | `/etc/pve/qemu-server/*.conf` 无 `hostpci`，GPU 由宿主机 `nvidia` 持有 |

---

## 7. 完整日志与逐条解读

### 7.1 补丁确实生效（首次 boot，23:58）

```
CMP40_COMPUTE_UNLOCK_V526: signed fullspeed stock-relock WPR-down commit FECS=0xffffff8f SS0=0x88888888 SS1=8 …
CMP40_GSP_READY_V551: feature=0x000000f3 ss0=0x88888888 readout=0x00000000 ss1=0x00000008
CMP40_PCIE_GEN2_V2: LNKCFG=0x80085800(bits[19:18]=2) PL_RATE=0x00240036(bits[18:17]=2) CYA0=0x068731b3(DIS_G2=0)
CMP40_PCIE_GEN2_V2: CYA=068731b3(DIS_G2=0) CFG=80085800(bits[19:18]=2) PL=00240036(bits[18:17]=2)
CMP40_PCIE_GEN2_V2: RETRAIN_FAIL status=1101
```

解读：算力解锁完整（`SS0=0x88888888`、`FECS=0xffffff8f`）；Gen2 的两个策略位都是 `2`，`DIS_G2=0`，门禁通过（没有出现 `GSP policy not ready`）；但重训练 2 秒后放弃。

### 7.2 装诊断版后的五个阶段快照（00:48:20，冷启动首次 `nv_open`）

```
phase=before_ovr  BAR0 OVR=00000000 CAP=00453d01 LNK=11010142 CAP2=00000002 CTL2=00000000 MISC1=e0b46500 CFG=80085800 PL=00350036 CYA=068731b3
phase=before_ovr  PCICFG GPU_CAP=00453d01 GPU_LNKSTA=1101 GPU_CAP2=00000002 GPU_CTL2=0000 UP_CAP=077a3903 UP_LNKSTA=7101 UP_CAP2=0000000e UP_CTL2=0001
phase=after_ovr_immediate BAR0 OVR=00000006 CAP=00453d01 CAP2=00000002 CTL2=00000000 … PL=00350036
phase=after_ovr_50ms       BAR0 OVR=00000006 CAP=00453d01 CAP2=00000002 CTL2=00000000 … PL=00350036
phase=after_target_writes BAR0 OVR=00000006 CAP=00453d01 CAP2=00000002 CTL2=00000000 … PL=00350036
                          PCICFG GPU_CAP=00453d01 GPU_LNKSTA=1101 GPU_CAP2=00000002 GPU_CTL2=0000 … UP_CTL2=0002
phase=retrain_poll attempt=1 status=1101 speed=1 width=x16 training=0 slot_clock=1
phase=retrain_fail        BAR0 … PL=00250036 …
CMP40_PCIE_GEN2_V2: RETRAIN_FAIL status=1101
```

三条决定性读数：

1. `OVR` 从 `00000000` 变成 `00000006` 并稳定读回 —— **写入成功，OVR 不是瓶颈**。
2. 但 `CAP` 仍是 `00453d01`、`CAP2` 仍是 `00000002`、`CTL2` 仍是 `00000000` —— **能力没变，补丁写的 `LnkCtl2=0002` 被静默截断回 `0000`**；同一时刻上游根口的 `UP_CTL2` 从 `0001` 变成 `0002`，说明写指令本身没问题，是端点此刻不接受 5GT/s。
3. `retrain_poll` 只有 `attempt=1` 一行、2 秒内状态毫无变化 —— 不是 RL 被忽略，而是**没有速率可谈**。

补充：`PL` 从 `00350036` 漂到 `00250036`，说明 GSP 的晚策略在我们之后还在改写 PCIe 寄存器。

### 7.3 同一 boot 内 rebind 后的快照（00:53:56）

```
before_ovr BAR0 OVR=00000006 CAP=00453d02 LNK=11010142 CAP2=00000006 CTL2=00000002 MISC1=e0b44500 CFG=80085800 PL=00240036 CYA=068731b3
before_ovr PCICFG GPU_CAP=00453d02 GPU_LNKSTA=1101 GPU_CAP2=00000006 GPU_CTL2=0002 UP_CAP=077a3903 UP_LNKSTA=7101 UP_CAP2=0000000e UP_CTL2=0002
retrain_poll attempt=1 status=1101 speed=1 width=x16 training=0 slot_clock=1
RETRAIN_FAIL status=1101
```

**这一行是全案的关键**：`CAP2=00000006`、`CTL2=0002`、`CAP=00453d02` —— 能力已就绪、目标速率已写入，驱动内那次重训练**仍然失败**。于是「平台不支持 Gen2」这个假设开始动摇。

同时 `MISC1` 从 `e0b46500` 变成 `e0b44500`（工作机运行时是 `e0b42d00`），说明私有 policy 寄存器还在被固件持续调整。

### 7.4 手动重放同一串寄存器写（00:57:41）

```
pre  GPU=1101  UP=7101
UP LnkCtl=0042
00:57:41.054 GPU/UP=1102/7102
final GPU=1102  UP=7102
```

**一次成功**。GPU `LnkSta=1102`（5GT/s、x16、训练完成），上游根口 `7102` 同步升速。与驱动内那次唯一的差别：**晚了约 4 分钟**。

### 7.5 结论表

| 环节 | 状态 |
|---|---|
| 补丁是否生效 | ✅ 算力、策略、OVR、双端 TLS 全部写入 |
| 端点能力向量 | ✅ 第二次 bootstrap 后 `CAP2=00000006` |
| 链路物理能力 | ✅ 一次 RL 就训到 Gen2 x16 |
| **失败点** | ❌ 重训练发生在 `nv_open` 内、bootstrap 刚结束的瞬间，2 秒窗口；此时固件晚策略未收尾，链路拒绝重新协商 |

---

## 8. 问题定性过程（5 步）

| 步 | 做了什么 | 看到什么 | 得出什么 |
|---|---|---|---|
| 1 | `dmesg` 分层看补丁日志 | 算力全绿、`LNKCFG/PL` 都 `=2`、`DIS_G2=0`、`RETRAIN_FAIL 1101` | 补丁没坏，问题在链路训练阶段，而不是"没解锁" |
| 2 | 排除模块/直通/版本 | `modinfo filename` 指向 `updates/cmpunlocker`、vermagic 匹配、无 `hostpci`、`nvidia` in use | 不是装错模块、不是 VFIO 直通 |
| 3 | 装诊断版取 5 阶段快照 | `OVR=6` 写成功，但 `CAP2=02`、`CTL2` 被截断成 `0000`、poll 只有 1 行 | **能力没就绪**，不是根口拒绝 RL（修正了最初的误判） |
| 4 | 同一 boot 内 rebind 再取快照 | 第二次 `before_ovr` 已 `CAP2=06`、`CTL2=0002`，仍 `RETRAIN_FAIL` | 能力就绪与重训练之间存在时间差 |
| 5 | 能力就绪后手动重放同一串写 | 一次 `1102` | 根因确定为**驱动重训练窗口过早**，链路本身没问题 |

关键转折在步 3 → 步 4：如果只看第一次快照，会得出"平台不支持"的错误结论；补上第二次快照后，方向立刻转为时序问题。

---

## 9. 根因

驱动把 `nv_cmp40hx_retrain_gen2()` 放在 `nv_open()` 里同步执行，轮询窗口 2 秒。此刻：

- GSP bootstrap 刚刚结束，RM 的 PCIe 晚策略 pass 仍在改写 `PL`/`MISC1`；
- 端点的能力向量要等到这次初始化流程走完（或下一次初始化）才稳定成 `06`；
- 在这次窗口内发出的目标速率写入被硬件截断，RL 触发后没有可协商速率，2 秒后放弃。

晚几十秒到几分钟之后，同样的寄存器串一次成功。**所以这不是硬件/平台缺陷，也不是补丁写错，而是"太早"。**

---

## 10. 解决方案

不动内核模块，改为**开机后在驱动稳定之后补执行一次同样的重训练**。

### 10.1 修复脚本（脚本原文已内置在《40HX破解-快速操作手册》步骤 6）

脚本不再以独立文件交付，原文直接写在 `40HX破解-快速操作手册.md` 的步骤 6 里，用 heredoc 落到 `/usr/local/sbin/`。逻辑分四段：

| 段 | 作用 |
|---|---|
| 定位 | 从 `lspci -d 10de:1f0b` 取 BDF，从 sysfs 父节点解析上游桥 |
| 等待 | 最多 180 秒等驱动挂载且 `nvidia-smi` 可用 |
| 前置 | 若 `CAP2` 缺 `0x04` 位，做一次 unbind/bind 把能力 latch 出来（失败也继续尝试训练） |
| 重试 | 按 `0 / 5 / 20 / 55 / 130 / 265` 秒累积退避，每轮执行「双端 `LnkCtl2.TLS=5GT/s` → 上游 `LnkCtl | RL` → 5 秒轮询」，命中即 `PASS at +Ns` 并退出 0 |

关键设计：

- 写入采用**读-改-写**（`(cur & ~0xf) | 0x2`），不覆盖 `Compliance` 等其它位；
- 全程只碰标准 PCIe 能力寄存器，失败的最坏结果就是"仍然是 Gen1"，不会掉卡；
- journal 里那行 `PASS at +Ns` 就是这台机器需要的确切等待时间，可据此把退避表收紧成固定延时。

### 10.2 开机自启单元

```ini
[Unit]
Description=CMP 40HX late PCIe Gen2 retrain
Documentation=https://github.com/Cyridd/cmpunlocker
After=systemd-modules-load.service nvidia-persistenced.service
# 不要加 ConditionPathExists=/dev/nvidia0：该条件只在启动瞬间判一次，
# 那时驱动还没建出节点就会被跳过，且 oneshot 不会重试。
# 等待驱动的工作交给脚本自己的 180 秒轮询。

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/cmp40-gen2-retrain.sh
RemainAfterExit=yes
TimeoutStartSec=600

[Install]
WantedBy=multi-user.target
```

### 10.3 安装（一条链路搞定，路径是关键）

手册步骤 6 里的两条 heredoc 已经包含「写文件 + 赋权」：

```bash
# ① 写脚本到 /usr/local/sbin/（原文见手册步骤 6.1）
sudo tee /usr/local/sbin/cmp40-gen2-retrain.sh >/dev/null <<'CMP40_SCRIPT_EOF'
…脚本原文…
CMP40_SCRIPT_EOF
sudo chmod 0755 /usr/local/sbin/cmp40-gen2-retrain.sh

# ② 写单元到 systemd（原文见手册步骤 6.2）
sudo tee /etc/systemd/system/cmp40-gen2-retrain.service >/dev/null <<'CMP40_UNIT_EOF'
…单元原文…
CMP40_UNIT_EOF

# ③ 启用
sudo systemctl daemon-reload
sudo systemctl enable --now cmp40-gen2-retrain.service
```

`ExecStart` 指向 `/usr/local/sbin/cmp40-gen2-retrain.sh`，所以脚本**必须**落在那个路径；直接从别处引用会报找不到文件。

### 10.4 为什么不用 `cmp40hx-driver-reprobe-gen2.sh`

核对源码可知它只做 `unbind → 3s → bind`，**没有任何 `setpci` 写、不发起重训练**。它的用途是把端点能力位从 `02` latch 到 `06`；重训练仍由驱动在同一个过早的时刻做，所以跑完依旧 `1101` —— 它报 `FAIL` 是正常行为，不是新故障。新脚本已内置等价的 rebind 逻辑（仅在需要时），因此**不需要再手动跑它**。

---

## 11. 验收与后续观察项

### 11.1 验收步骤

```bash
# 1) 手工跑一次（当前链路若已是 Gen2，会立刻 PASS，仅验证脚本可用）
sudo systemctl start cmp40-gen2-retrain.service
journalctl -u cmp40-gen2-retrain -n 20 --no-pager

# 2) 冷断电（必须真断电，reboot 不算）
sudo poweroff   # 等电源灯灭，拔插头 10 秒，再上电

# 3) 开机 2–3 分钟后
journalctl -u cmp40-gen2-retrain -b --no-pager
lspci -vvv -s 0000:05:00.0 | grep -E 'LnkSta:|LnkCtl2:'
```

期望：journal 出现 `PASS at +Ns: … speed=2 width=x16`；`lspci` 显示 `LnkSta: Speed 5GT/s, Width x16`、`LnkCtl2: Target Link Speed: 5GT/s`。

### 11.2 遗留观察项（当前不阻塞）

- `MISC1`：工作机运行时 `e0b42d00`，本机 `e0b46500 → e0b44500`（`bit11` 未置位、`bit14` 曾置位）。开源补丁路径不写这个寄存器。若未来出现「训上了、跑一会儿又掉回 Gen1」，第一个查它。
- dmesg 中驱动自带的 `RETRAIN_FAIL status=1101` 会一直在，属预期噪声。要彻底干净需改补丁：把 `nv_open` 内的同步 2 秒轮询改成延时 workqueue 重试。
- `pcie_aspm=off` 保留。ASPM 与速率协商无直接关系，但关掉可减少训练期干扰。

---

## 12. 边界、风险与「不要做的事」

- **易失性**：断电即失效，路线 A 靠模块自动重跑策略，Gen2 靠本单元补训练。
- **版本硬绑定**：580.173.02 / 610.57.04 / 615.78.08 三选一，内核模块与用户态必须同版本；换内核要重建。
- **SHA256 钉死**：源码包校验失败会直接退出。
- **Secure Boot**：未签名模块加载不了。
- **Gen3 不可达**：已论证，不要试。
- **不要做**：`cmp40hx-alder-lake-link-disable-test.sh`、任何 Link Disable、`Secondary Bus Reset`。这些能训成 Gen2，但会掉 GSP/RM；在无显示输出的 PVE 上等于卡失联，只能断电恢复。既然只是时序问题，没有任何理由承担这个风险。
- **不要只看 `max_link_speed`**：sysfs 可能过时。以 `LnkSta` 实测为准。

---

## 13. 附录：全部命令速查

```bash
# --- 身份 ---
lspci -Dnn -d 10de:1f0b
cat /proc/driver/nvidia/version
modinfo nvidia | grep -E '^(filename|version|vermagic)'
cat /var/lib/cmp40hx/install-$(uname -r).manifest
ls -l /lib/modules/$(uname -r)/updates/cmpunlocker/

# --- 安装 ---
sudo ./install.sh --driver-version=615.78.08               # 正式版
sudo ./install.sh --driver-version=615.78.08 --pcie-diagnostic  # 诊断版
sudo ./install.sh --verify
sudo ./install.sh --uninstall

# --- 日志 ---
dmesg -T | grep -E 'CMP40_PCIE_GEN2|CMP40_COMPUTE_UNLOCK|CMP40_GSP_READY'
dmesg -T | grep -c CMP40_PCIE_GEN2          # 0 = 从未打开过设备
dmesg -T | grep -E 'CMP40_PCIE_GEN2_DIAG_V1.*(before_ovr|retrain)'

# --- 寄存器 ---
GPU=$(lspci -Dnn -d 10de:1f0b | awk 'NR==1{print $1}')
UP=$(basename $(readlink -f /sys/bus/pci/devices/$GPU/..))
setpci -s $GPU CAP_EXP+0c.L CAP_EXP+2c.L CAP_EXP+12.W CAP_EXP+30.W
setpci -s $UP  CAP_EXP+0c.L CAP_EXP+2c.L CAP_EXP+12.W CAP_EXP+30.W

# --- 手动重训练（能力已就绪时）---
setpci -s $GPU CAP_EXP+30.W=0002
setpci -s $UP  CAP_EXP+30.W=0002
CTL=$(setpci -s $UP CAP_EXP+10.W|tr -d ' '); setpci -s $UP CAP_EXP+10.W=$(printf '%04x' $(( (16#$CTL)|0x20 )))
sleep 1; setpci -s $GPU CAP_EXP+12.W

# --- 修复脚本 ---
sudo /usr/local/sbin/cmp40-gen2-retrain.sh
systemctl status  cmp40-gen2-retrain --no-pager
journalctl -u cmp40-gen2-retrain -b --no-pager

# --- 验收 ---
lspci -vvv -s $GPU | grep -E 'LnkCap:|LnkSta:|LnkCap2:|LnkCtl2:'
```
