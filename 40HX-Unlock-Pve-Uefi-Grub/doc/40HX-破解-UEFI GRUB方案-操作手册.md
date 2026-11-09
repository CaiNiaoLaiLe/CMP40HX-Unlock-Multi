# CMP40HX-Unlock 操作手册（PVE / UEFI 前置解锁）

> 适用：NVIDIA CMP 40HX（TU106，`10de:1f0b`）装在 **PVE 物理主机**上，机器走 **UEFI + GRUB** 引导
> 不适用：systemd-boot 引导的机器、BIOS/Legacy 引导的机器、双 CMP 卡的机器
> 目标：Tensor/FP16 解锁 + PCIe Gen2 x16，且**能与 vGPU 共存**
> 原理与代码细节见同目录 `CMP40HX-Unlock-项目总结.md`

---

## 步骤 0 · 先确认三个前提（一分钟）

```bash
# 1) 是不是 UEFI 引导
[ -d /sys/firmware/efi ] && echo "UEFI ✓" || echo "UEFI ✗ —— 本方案不适用"

# 2) Secure Boot 是否关闭（必须关闭，否则加载不了未签名 .efi）
mokutil --sb-state 2>/dev/null || echo "未装 mokutil（多数情况下 SB 已关）"

# 3) 用的是不是 GRUB
ls -l /boot/grub/grub.cfg && echo "GRUB ✓"
grep -c '^menuentry' /boot/grub/grub.cfg

# 4) 卡在不在、是不是只有一张（正常 40HX 报 1f0b；vBIOS 被改成 90HX 的会报 220d）
lspci -nn | grep -E '10de:(1f0b|220d)'
lspci -nn | grep -cE '10de:(1f0b|220d)'   # 必须正好是 1
```

四项都过：UEFI ✓、Secure Boot 关闭、有 `/boot/grub/grub.cfg`、卡恰好 1 张。
**任何一项不过就先别往下做**（这不是「试试看」的方案，条件不满足脚本会直接拒绝）。

---

## 步骤 1 · 拿到 .efi（二选一）

### 1A 直接用仓库里的预编译产物（最省事，不用编译环境）

```bash
cd /root/40HX-Unlock-Pve-Uefi-Grub        # 换成你实际放代码的目录
sha256sum -c CHECKSUMS.sha256
```

期望三行全 `OK`：

```
NVPermissiveEFI-COMPUTE.efi: OK
NVPermissiveEFI.efi: OK
NVPermissiveEFI-FULL.efi: OK
```

这三份是作者编译好的 inspect / compute / full，**不需要装编译环境**。直接跳到步骤 2。

### 1B 自己重新编译（改了源码时必须走这条）

代码拷到 PVE 宿主机后，直接这三条：

```bash
cd /root/40HX-Unlock-Pve-Uefi-Grub
apt update
apt install -y build-essential binutils python3
make release
```

`make release` 会依次产出三个模式并刷新 `CHECKSUMS.sha256`（原来那份会被覆盖，属正常）：

```
NVPermissiveEFI.efi          ← inspect（无 LoadOptions 时的兜底模式）
NVPermissiveEFI-COMPUTE.efi  ← compute
NVPermissiveEFI-FULL.efi     ← full
```

编完核对一下：

```bash
ls -l NVPermissiveEFI*.efi
python3 tools/verify_efi.py NVPermissiveEFI-COMPUTE.efi   # 期望 EFI OK
```

---

## 步骤 2 · 安装到 GRUB

```bash
# 交互式（必须在仓库目录里执行，脚本用自身所在目录找 .efi）
# 会先问安装方式：1 = 编译安装（先 make release 再装），2 = 预编译安装（直接用现成 .efi）
sh install-efi.sh

# 一条命令全自动：用现成 .efi，装过就先卸掉重装，默认项设成 compute、菜单等 5 秒
sh install-efi.sh --force --default=compute --timeout=5
```

**参数**（带任一参数即全自动：不问安装方式、不问 yes、装完不进入可选菜单）：

| 参数 | 作用 |
|---|---|
| `--build` / `-b` | 先跑 `make release` 再安装，装的就是刚编出来的产物 |
| `--prebuilt` / `-p` | 直接用仓库里现成的 `.efi`，不编译（**默认行为**） |
| `--force` / `-f` | 已安装时直接卸载重装，不再询问 |
| `--default=标题` | 装完设置默认启动项。可写 `inspect` / `compute` / `full`，也可写完整标题（逐字节校验，不在 `grub.cfg` 里就报错退出） |
| `--timeout=秒` | 装完设置菜单停留时间，非负整数 |
| `--help` / `-h` | 打印用法 |

> **只有给了 `--build` 才会编译**。只带 `--force` / `--default` / `--timeout` 时一律用现成的 `.efi`，
> 不会替你重新编译（换了源码却没加 `--build`，装进去的会是旧产物）。

**不带参数时，脚本会先问安装方式**；输入 1 / 2 以外的任何内容直接退出，一个文件都不动：

```
请选择安装方式:
  1) 编译安装   : 先 make release, 再用刚编出来的 .efi
  2) 预编译安装 : 直接用仓库里现成的 .efi, 不编译
请输入 1 或 2 (其它输入退出): 
```

选 1 时会先检查 `make` / `gcc` / `ld` / `objcopy` / `python3`，再在仓库目录跑 `make release`；
编译失败就直接退出，**不会改动 ESP 和 GRUB**。选 2 则跳过编译，产物不齐全会提示改选 1。

**装过的话脚本会先问一句**（交互模式）。只要 `/etc/grub.d/40-cmp40hx` 或 `ESP/EFI/UNLOCK/` 下已存在产物：

```
  已存在: /etc/grub.d/40-cmp40hx
  已存在: /boot/efi/EFI/UNLOCK/NVPermissiveEFI-COMPUTE.efi

检测到本机已经安装过 NVPermissiveEFI。
输入 yes 或 y: 先卸载, 再重新安装
回车或其它: 退出 (不改动任何文件)
请输入: 
```

- 输 `yes` 或 `y`（大小写不限）：脚本自己调 `uninstall-efi.sh` 卸载，然后原地继续安装（不用你再跑第二遍）；
- 直接回车或乱输：立即退出，**一个文件都不动**。

脚本做三件事：

1. 三个 `.efi` → `/boot/efi/EFI/UNLOCK/`（ESP 不在默认位置时：`ESP=/boot/efi sh install-efi.sh`）
2. 从 `/boot/grub/grub.cfg` 抽取 **PVE 原条目主体**，生成 `/etc/grub.d/40-cmp40hx`
3. `update-grub` 并统计菜单项数量

期望输出（交互式）：

```
请选择安装方式:
  1) 编译安装   : 先 make release, 再用刚编出来的 .efi
  2) 预编译安装 : 直接用仓库里现成的 .efi, 不编译
请输入 1 或 2 (其它输入退出): 2
Secure Boot: SecureBoot disabled
[1/3] ESP 安装完成: /boot/efi/EFI/UNLOCK/
      来源: 仓库内现成的 .efi (未编译)
a301a5fa…  NVPermissiveEFI.efi
055c5446…  NVPermissiveEFI-COMPUTE.efi
a9e8f830…  NVPermissiveEFI-FULL.efi
[2/3] 已生成 /etc/grub.d/40-cmp40hx
[3/3] update-grub 完成, /boot/grub/grub.cfg 中含 3 个新菜单项 (应为 3)

安装完成。

可选操作:
  1) 查看当前 GRUB 启动项名称
  2) 设置默认启动项
  3) 设置菜单停留时间
  4) 查看当前菜单停留时间
  5) 清空默认启动项
  0) 退出
请选择: 
```

（`来源:` 那一行是识别“装的是哪份产物”的最快依据：选 1 应显示“本次 make release 的编译产物”。
选 1 时在 `Secure Boot:` 之前还会多一段 `-------- 编译 (make release) --------` 到 `-------- 编译完成 --------`。
上面三行 sha256 是示例，实际是当次产物的值；关键是**和 `sha256sum NVPermissiveEFI*.efi` 一致**。）

装完会停在这个菜单上，可以直接在安装这一步把默认启动项和等待时间设好：

| 输入 | 作用 |
|---|---|
| `1` | 列出 `grub.cfg` 里所有启动项的完整标题（带行号，仅供肉眼辨认），看完回到菜单 |
| `2` | 输入完整标题（也可只输 `inspect`/`compute`/`full`）→ 校验是否真存在于 `grub.cfg`，通过则写 `GRUB_DEFAULT` 并 `update-grub`；不匹配就报错、不改文件、回菜单 |
| `3` | 输入秒数（非负整数）→ 写 `GRUB_TIMEOUT` 并 `update-grub`；不是数字就报错回菜单 |
| `4` | 显示当前 `GRUB_TIMEOUT`、`GRUB_DEFAULT`（顺带提示 `GRUB_TIMEOUT_STYLE=hidden` 会让停留时间失效） |
| `5` | 清空 `GRUB_DEFAULT`，让 GRUB 回到"取菜单第一条"，也就是原来的 PVE 条目 = 不解锁 |
| `0` | 退出（装完只有这一条会离开菜单） |
| 回车 / 其它 | **回到菜单**，只打一行"无效输入"提示，不退出 |

**含 3 个新菜单项**才算成功。生成的三项是：

| 菜单项 | 作用 |
|---|---|
| `PVE + CMP40HX UNLOCK (inspect, 只读诊断…)` | 只读打印寄存器，不写、不复位，零风险 |
| `PVE + CMP40HX UNLOCK (compute, 最小解锁…)` | 全套寄存器 + 1 次双重 SBR + Gen2，日常用这个 |
| `PVE + CMP40HX UNLOCK (full, …)` | 对齐 Windows 完整解锁，8 次单 SBR，慢 |

原来的 PVE 条目仍然保留 = 不解锁，随时可退回。


常见拦路（脚本会自己报，照做就行）：

| 报错 | 处理 |
|---|---|
| `错误: 本机不是 UEFI 启动, EFI 方案不适用` | 换方案，或确认确实在 UEFI 模式 |
| `错误: Secure Boot 开启会拒绝未签名的 .efi` | 进 BIOS 关 Secure Boot（不是 mokutil 能绕的） |
| `错误: 找不到 /boot/grub/grub.cfg` | 指定实际路径：`GC=/boot/grub/grub.cfg sh install-efi.sh` |
| `错误: 找不到 NVPermissiveEFI-COMPUTE.efi, 先 make` | 你只拷了源码没拷产物；新脚本会说 `错误: 缺少 …/NVPermissiveEFI.efi`，重跑选 1 编译安装，或先 `make release` |
| `错误: 找不到 make / gcc / ld / objcopy / python3` | 选了 1 编译安装但依赖不全：`apt install -y build-essential binutils python3` |
| `错误: make release 失败` | 本次安装已中止，ESP/GRUB 未动；按提示看是否为 `i386pep` 缺 PE 后端 |
| `检测到本机已经安装过 NVPermissiveEFI` + `请输入:` | 输 `yes` 或 `y` 让它自动卸载重装；不想动就回车退出 |
| `错误: 在 … 中找不到 Proxmox menuentry` | PVE 条目名被改过或不是 GRUB 布局，需手工调整 `awk` 匹配串 |

---

## 步骤 3 · 第一次重启：跑 inspect（零风险）

```bash
reboot
```

GRUB 菜单里选 **`PVE + CMP40HX UNLOCK (inspect, 只读诊断…)`**，屏幕会依次出现：

```
CMP40HX Unlock / UEFI proof of concept
Target: NVIDIA 10de:1f0b / 10de:220d. Default mode is read-only inspect.

GPU 0000:05:00.0 ID=1f0b10de
Upstream bridge 0000:00:03.0
BAR0=0x… BAR1=0x…
BOOT0=0x16xxxxxx
Core loaded below 2 GiB: version unavailable

== read-only inspect ==
40HX inspect: BOOT0=0x… XP3G=0x… FECS_PLM=0x…
40HX inspect: SS0=0x… SS0_RD=0x… SS1=0x… FUSE_DISP=0x…
40HX PDISP-EFI: 0x600000=0x… 0x610000=0x… 0x419B54=0x…
read-only inspect complete

Inspect completed. No unlock mode was requested.
Press any key to continue booting PVE/Linux (auto in 10s)...
```

**怎么判读**（这几行决定你后面能不能继续）：

| 看什么 | 正常 | 说明 |
|---|---|---|
| `GPU 0000:05:00.0 ID=1f0b10de` | 有且只有一行 | 多一行/零行都会在下面报 refusing |
| `Upstream bridge` | 通常是 `0000:00:03.0`（CPU 根口下的那个桥） | 报 "Could not identify" 就是拓扑不对 |
| `BOOT0` | `0x16xxxxxx`（TU106） | 白名单接受 `0x17/0x1a/0x16/0x0fxxxxxx/0x0fffffff`，其余直接拒绝 |
| `SS0` 现值 | 原厂通常是 `0x…`（非 `0x88888888`） | 记录它，解锁后再看是否变成 `0x88888888` |

看到 `Press any key to continue booting PVE/Linux (auto in 10s)...` 按键或等 10 秒，机器会继续进 PVE —— **inspect 不改任何东西，这一步一定不会影响系统。**

---

## 步骤 4 · 第二次重启：跑 compute

```bash
reboot
```

这次选 **`PVE + CMP40HX UNLOCK (compute, 最小解锁…)`**。因为要复位 2 次总线、每次沉降 8 秒，**开机大约慢 30 秒**，属正常。

期望关键片段：

```
== compute ==
[core] 40HX: full set written (XP3G/XVE/FECS/SS0/SS1/08610/consumer-chain)
[core] SBR via 0000:00:03.0; settling 8 seconds
[core] SBR device ready: 1f0b10de
[core] SBR via 0000:00:03.0; settling 8 seconds
[core] SBR device ready: 1f0b10de
[core] dual SBR complete
compute complete

== gen2 ==
gen2 policy: OVR=00000006 CFG=80085800 PL=00340036 MISC=20342d00 CYA=068711b3
gen2 gpu  policy CAP=00453d02 CAP2=00000006 CTL=0000 CTL2=0002 STA=1101 spd=1
gen2 root pre    CAP=077a3903 CAP2=0000000e CTL=0042 CTL2=0001 STA=7101 spd=1
gen2: retrain round 1, CCC=0 on both ends
gen2 gpu  tgt    CAP=00453d02 CAP2=00000006 CTL=0000 CTL2=0022 STA=1102 spd=2
gen2 root tgt    CAP=077a3903 CAP2=0000000e CTL=0200 CTL2=0022 STA=3102 spd=2
gen2 gpu  done   ... STA=1102 spd=2
gen2 root done   ... STA=3102 spd=2
gen2: link trained at Gen2 during the UEFI stage.

UNLOCK SEQUENCE COMPLETED.
Boot Linux without powering off.

Press any key to continue booting PVE/Linux (auto in 10s)...
```

**四个判定点：**

1. `SBR device ready:` 出现两次 —— 说明复位后卡回来了。这里打印的是首次扫描保存的真实 ID，本机是 `1f0b10de`（部分卡 vBIOS 报 90HX 的 ID `220d10de`，两者都接受）。
2. `gen2 gpu policy` 的 `CAP` 低 4 位 = `2`（`00453d02`）—— 端点已经把 5GT/s 能力报出来。
3. `retrain round 1, CCC=0` 之后的 `tgt` 行双端 `spd=2` —— 链路真的到 Gen2 x16。
4. `gen2: link trained at Gen2 during the UEFI stage.` —— 本轮成功。

如果看到 `retrain round 2, CCC=1` 说明第一轮没成、自动换共时钟模式重试；
两轮都失败会打印 `still Gen1; upstream Link Control restored.`（上游控制已原样恢复，不影响正常启动）。
此时**不要下结论**——必须进系统后用 `lspci` 确认（步骤 5），EFI 屏幕上的结论不是最终结论。

### 想让机器每次自动跑 compute（无头机）

**最省事的办法是装的时候就在 `install-efi.sh` 的菜单里设好**：选 `1` 看标题、选 `2` 输入完整标题（脚本会校验，输错直接报错不写文件）、选 `3` 设等待秒数。装完就设完了，不用再回系统敲命令。

也可以一条命令搞定（全自动：用现成 `.efi`、已装则重装、默认项 compute、菜单停 5 秒）：

```bash
sh install-efi.sh --force --default=compute --timeout=5
```

> `--default=compute` 会自己展开成完整标题再写进 `GRUB_DEFAULT`，不用手抄。
> 想让菜单完全不显示，把 `/etc/default/grub` 里的 `GRUB_TIMEOUT_STYLE` 改成 `hidden`
> （此时 `GRUB_TIMEOUT` 表示「失败时显示菜单的等待时间」）。

---

## 步骤 5 · 进系统后验收（这一步才算完成）

机器会自己继续启动进 PVE，登录后执行：

```bash
# ① 链路速率（唯一可信的 Gen2 判据）
GPU=$(lspci -D -nn | awk '/10de:1f0b|10de:220d/{print $1; exit}')
echo "===== GPU:$GPU 链路信息 ====="
lspci -vvv -s "$GPU" | grep -E 'LnkCap:|LnkSta:|LnkCap2:|LnkCtl2:'

# ①b 上游 root port（看 CommClk- 和双端速率是否一致）
# 替换掉不稳定 lspci -PP + sed，改用 sysfs 稳定获取上游桥
if [ -n "$GPU" ] && [ -d "/sys/bus/pci/devices/$GPU" ]; then
    UP=$(readlink /sys/bus/pci/devices/"$GPU"/.. -f | xargs basename)
    echo -e "\n===== 上游RootPort:$UP 链路信息 ====="
    lspci -vvv -s "$UP" | grep -E 'LnkSta:|LnkCtl:|LnkCtl2:|CommClk'
else
    echo -e "\n===== 上游RootPort: 未找到GPU设备 ====="
fi

# ② 驱动与总线有没有异常
echo -e "\n===== dmesg nvidia/pcieport 最近日志 ====="
dmesg | grep -iE 'nvidia|pcieport' | tail -30

# ③ 卡是否正常可用
echo -e "\n===== nvidia-smi ====="
nvidia-smi
echo -e "\n===== 显卡基础信息 ====="
nvidia-smi -q | grep -iE 'Product Name|Bus Id|VGPU'

# ④ vGPU 侧（如果跑 vGPU）
echo -e "\n===== nvidia-vgpud 启动日志 ====="
journalctl -b -u nvidia-vgpud | tail -20

```

判据：

| 项 | 通过 | 不通过 |
|---|---|---|
| `LnkSta`（端点） | `Speed 5GT/s, Width x16` | 仍是 `2.5GT/s` → 见故障表 G |
| `LnkSta`（上游） | `Speed 5GT/s, Width x16` | 双端不一致 → 回流重跑 |
| 上游 `LnkCtl` | `ASPM Disabled; … CommClk-` | 出现 `CommClk+` → CCC 被改回 1，链路会回 Gen1 |
| `dmesg` | 无 `fallen off the bus` / `link down` / `probe failed` | 出现即说明 SBR 或时序有问题 |
| `dmesg` AER | 无 correctable/uncorrectable 刷屏 | 有刷屏 → 信号完整性存疑，考虑降 Gen1 |
| `nvidia-smi` | 正常列出 CMP 40HX，显存 8192MiB | 不列卡 → 驱动没起来 |
| vGPU | `nvidia-vgpud` 无报错、虚拟机可分配 | 掉会话 → 本方案本应避免，属异常 |

> `CommClk-`（CCC=0）是解锁后的**正确形态**。不要用 `setpci` 之类去把它改成
> `CommClk+`，那会让 Gen2 训练重新回落。

### FP16 / Tensor 是否真的开了

```bash
# 用现成的 pytorch 跑一次 FP16 矩阵乘，观察 TFLOPS
python3 - <<'PY'
import torch, time
assert torch.cuda.is_available(), "CUDA 不可用"
a = torch.randn(8192, 8192, device='cuda', dtype=torch.float16)
b = torch.randn(8192, 8192, device='cuda', dtype=torch.float16)
for _ in range(3): c = a @ b          # 预热
torch.cuda.synchronize(); t = time.time()
for _ in range(10): c = a @ b
torch.cuda.synchronize()
dt = (time.time() - t) / 10
print(f"FP16 HGEMM: {2*8192**3/dt/1e12:.2f} TFLOPS")
PY
```

参考量级：**解锁成功约 50 TFLOPS 档**，未解锁会明显低一截。受 Gen2 x16（~6.4 GB/s）与 8 GB 显存限制，别拿它和桌面卡比。

---

## 步骤 6 · 故障分流表

| 编号 | 屏幕/系统现象 | 原因 | 处理 |
|---|---|---|---|
| A | `Refusing to continue: found 0 CMP 90HX devices` | 卡没被 UEFI 枚举到，或不在同一 segment | 换槽位/确认 Above 4G Decoding 开启；双卡机器本方案不支持 |
| B | `found 2 …` | 机器上有两张 1f0b | 拔掉一张，或改代码放宽多卡逻辑 |
| C | `Could not identify the immediate upstream PCI bridge` | 桥拓扑异常（比如卡挂在交换机/PLX 后） | 直连 CPU/芯片组根口 |
| D | `BOOT0 is not GA102/TU106. No register write was attempted.` | 读回值不在白名单 | 确认是不是 40HX；BAR0 映射是否正常 |
| E | `SBR identity check failed: expected 1f0b10de, read ffffffff`（90HX vBIOS 的卡会显示 `220d10de`） | 复位后卡没回来（供电/插槽/主板兼容） | 冷断电重试；换槽；确认没有 ASPM/节能把链路关死 |
| E2 | 屏幕最后一行停在 `SBR via 0000:…; settling 8 seconds` | 三种可能：①从 GRUB 退出时应用输出被冲掉，其实后面还有字；②没等够——一次 SBR 的沉降 8 s 加轮询最长约 20 s，`full` 有 8 次；③装的 `.efi` 不是当次编译的那份 | 先**别按键**，等 60 s 再拍照；`install-efi.sh` 会打印 ESP 里三个 sha256，和仓库里 `sha256sum NVPermissiveEFI*.efi` 逐条对比；不一致就重装 |
| E3 | 开机画面报 `ID=220d10de` 但 `BOOT0=0x16xxxxxx` | 卡报的是 CMP 90HX 的 device ID，芯片却是 Turing（40HX）——vBIOS 里的 ID 被改过。本机报的是 `1f0b10de`，属正常 | 不影响解锁（SBR 自检用首次扫描的 `expected_id`，不是硬编码 220d）；想确认就进系统 `lspci -nn -s 05:00.0` |
| F | `gen2: PCIe capability not found on GPU or upstream; skipped.` | 能力链表读不到（虚拟化/桥不支持） | 该项非致命，算力已解锁；Gen2 只能放弃 |
| G | 进系统后 `LnkSta` 仍是 `2.5GT/s`，UEFI 屏幕上也没打出 `link trained at Gen2` | Gen2 三步缺了一步：①`CAP` 低 4 位不是 `2` → 策略没物化；②屏幕走了 `retrain round 2, CCC=1` 仍失败 → 双端时钟模式没对齐；③只有 `pre` 行没有 `tgt` 行 → 根本没发起重训 | 看 UEFI 屏幕的 `policy`/`pre`/`tgt`/`done` 四行定位：`CAP` 不是 `…02` 查策略寄存器；两轮都 `1101` 按 `CMP40HX-Gen2链路问题说明.md` 重测 CCC；顺便确认 `.efi` 是当次编译的那份（看 `来源:` 行 + sha256） |
| H | `Core ELF loader failed: 0x…` | 嵌入 core 加载失败（内存不足/重定位不支持） | 换 `make release` 重新编译；确认没混入 GA102 core |
| I | `FAILED (n). Power the machine fully off before retrying.` | 前面任一步失败 | **冷断电**（拔电 10 秒）再试，不要热重启硬顶 |
| J | 开机卡在 GRUB 菜单不动 | `GRUB_DEFAULT` 标题不匹配 | 手工选条目；或删掉 `GRUB_DEFAULT` 那行 `update-grub` |
| K | 屏幕出现 "Boot Windows…" 字样 | 说明 ESP 里跑的还是旧编译的 `.efi`（新版文案已改为 Linux/纯英文） | `sha256sum /boot/efi/EFI/UNLOCK/*.efi` 与仓库里的值对账，然后 `make release` + `sh install-efi.sh --force` |

> **屏幕上的失败原因会被冲掉**：EFI 应用退出时 GRUB 会重画控制台，你自己那几行（`SBR identity check failed…` / `FAILED (n)…`）会被清空，只剩下 GRUB 的 `error: unknown error.` 和 `Loading Linux …`。所以看到报错时**先别按键**，立刻往前翻/拍照；最稳的办法是先跑 `inspect`（不写、不复位）把 ID/BOOT0 拿准。
>
> **不要怀疑 `sbr_once()` 的时序**：上游原版的这段时序在这台机器上 `compute` / `full` 都能跑完，SBR 部分零改动（后来只为 Gen2 改过 `apply_gen2()`）。历史上曾把“停在 settling”误判成“1 秒等待太短”，重写的版本反而不工作，已全部回退。遇到卡住，按 E2 的三条走：等够、别按键、核对 sha256。

---

## 步骤 7 · 回退（三级，随时可用）

```bash
# 一级：只是这次不想解锁 —— GRUB 菜单里选原来的 "Proxmox Virtual Environment" 条目

# 二级：默认不解锁，但保留选项
sed -i '/^GRUB_DEFAULT=/d' /etc/default/grub && update-grub

# 三级：彻底移除
cd /root/40HX-Unlock-Pve-Uefi-Grub && sh uninstall-efi.sh
# 等价于: rm /etc/grub.d/40-cmp40hx && rm -rf /boot/efi/EFI/UNLOCK && update-grub

# 终极：冷断电。寄存器由 GSP 每次上电重新初始化，断电即回工厂状态
```

`uninstall-efi.sh` 只删菜单项和 ESP 目录，不动 PVE 原有条目，删完 `update-grub` 就干净了。

> **重装不用先手动卸载**：交互模式检测到已安装会问一句，输 `yes` 或 `y` 就自己调 `uninstall-efi.sh` 卸掉再装；带上 `--force` 则连问都不问。回车退出则不碰任何文件。

---

## 坑清单（都是这套方案特有的）

1. **必须 UEFI + GRUB**。systemd-boot 的 PVE 用不了（没有 `chainloader` 这条链路）。
2. **Secure Boot 必须关**，这不是签名问题能绕的，得进 BIOS。
3. **只支持恰好一张卡**，`matches != 1` 直接拒绝，双卡别试。
4. **不要把 EFI 屏幕上的 Gen2 结论当最终结论**，一定进系统 `lspci` 复核。
5. **`make release` 的产物会覆盖 `CHECKSUMS.sha256`**，想保留原始校验值先备份。
6. **compute 开机慢约 30 秒，full 慢约 85 秒**，是 SBR 沉降时间，不是卡死。
7. **屏幕上的 "Windows" 字样说明装的是旧 `.efi`**，新版文案已改为 Linux/纯英文；按故障表 K 处理（核对 sha256 后重编重装），不是“上游残留，忽略即可”。
8. **从 Windows 拷过来的脚本必须确认是 LF 换行**。带 CRLF 时 `sh install-efi.sh` 会报 `Illegal option -`（`set -e` 后面多了 `\r`）；`Makefile` 带 CRLF 报 `*** missing separator`。检查 `grep -c $'\r' install-efi.sh`，输出非 0 就是中招；修法是 `sed -i 's/\r$//' install-efi.sh uninstall-efi.sh Makefile`。用 `scp`/WinSCP 二进制模式传输，别在 Windows 编辑器里另存。

---

## 一页命令版

### 页 1 · 编译 + 安装（执行完要重启）

```bash

git clone https://github.com/CaiNiaoLaiLe/40HX-Unlock-Pve-Uefi-Grub.git

#（代码已拷到 /root/40HX-Unlock-Pve-Uefi-Grub）
cd /root/40HX-Unlock-Pve-Uefi-Grub

# 一条命令全自动（推荐，无人工停顿）：
sh install-efi.sh --force --default=compute --timeout=5

# 改过源码就把 --force 换成 --build（先 make release 再装）：
# sh install-efi.sh --build --force --default=compute --timeout=5

# 重启
reboot
```

### 页 2 · 验收与回退

```bash
# 验收
GPU=$(lspci -D -nn | awk '/10de:1f0b|10de:220d/{print $1; exit}')
echo "===== GPU:$GPU 链路信息 ====="
lspci -vvv -s "$GPU" | grep -E 'LnkCap:|LnkSta:|LnkCap2:|LnkCtl2:'


# 回退
sed -i '/^GRUB_DEFAULT=/d' /etc/default/grub && update-grub    # 回普通启动
sh uninstall-efi.sh                                            # 彻底移除
```
