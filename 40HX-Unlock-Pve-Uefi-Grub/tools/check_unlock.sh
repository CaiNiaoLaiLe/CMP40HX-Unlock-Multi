#!/usr/bin/env bash
# check_unlock.sh — CMP40HX UEFI 解锁（Gen2 / ReBAR / 算力）本机验收脚本
#
# 用法:
#   bash tools/check_unlock.sh          一键快检: 每项一行 PASS/FAIL/WARN + 总结
#   sudo bash tools/check_unlock.sh     快检 + 算力寄存器判读（读 BAR0 需要 root）
#   bash tools/check_unlock.sh --help   帮助
#
# 只读脚本: 不写寄存器、不改配置空间、不动 GRUB/驱动。
# 唯一触碰硬件的地方是算力项对 BAR0 的 mmap 只读回显（本脚本
# 内置的偏移白名单，均在已验证可安全读取的区域内）。
set -u

# ---------- 颜色与输出 ----------
if [ -t 1 ]; then
    C_G=$'\e[32m'; C_R=$'\e[31m'; C_Y=$'\e[33m'; C_B=$'\e[36m'; C_Z=$'\e[0m'
else
    C_G=""; C_R=""; C_Y=""; C_B=""; C_Z=""
fi

N_PASS=0; N_WARN=0; N_FAIL=0; N_SKIP=0
LAST_STATUS=0   # 0=PASS 1=WARN 2=FAIL 3=SKIP （供 DETAIL 模式统计）

ok()    { N_PASS=$((N_PASS+1)); printf '  [%sPASS%s] %-8s %s\n' "$C_G" "$C_Z" "$1" "$2"; LAST_STATUS=0; }
warn()  { N_WARN=$((N_WARN+1)); printf '  [%sWARN%s] %-8s %s\n' "$C_Y" "$C_Z" "$1" "$2"; LAST_STATUS=1; }
fail()  { N_FAIL=$((N_FAIL+1)); printf '  [%sFAIL%s] %-8s %s\n' "$C_R" "$C_Z" "$1" "$2"; LAST_STATUS=2; }
skip()  { N_SKIP=$((N_SKIP+1)); printf '  [%s--%s]  %-8s %s\n' "$C_B" "$C_Z" "$1" "$2"; LAST_STATUS=3; }
detail(){ [ "${DETAIL:-0}" -eq 1 ] && printf '%s\n' "$@"; return 0; }
head2() { echo; echo "== $* =="; }

usage() {
    cat <<'EOF'
用法: bash tools/check_unlock.sh

  (无参数)   一键快检 Gen2 / ReBAR / 驱动与链路健康 / 环境前置，
             每项一行结果，末尾给总体判定。root 下附加算力寄存器判读。
             (需逐项原始明细时用: DETAIL=1 bash tools/check_unlock.sh)

检查项:
  env      环境前置: UEFI/SecureBoot/卡识别/驱动绑定/ResizableBar 模块参数
  gen2     PCIe 链路: 双端 Speed/Width、上游 CommClk、LnkCtl2 目标速率
  rebar    内核命令行 pci=realloc、BAR1: sysfs 实际尺寸、lspci Resizable BAR 能力、nvidia-smi BAR1
  driver   健康: nvidia-smi、Xid、AER、pcieport/nvidia 错误日志
  compute  算力寄存器: SS0/SS1/FECS_PLM/XP3G 现值判读（需 root，只读 BAR0）
  booter   DIRECT_SEC2 首跑 trace 判读（BOOTER_TRACE.TXT，无需 root）
EOF
}

# --help / -h 不需 lspci/GPU 即可打印
case "${1:-}" in -h|--help) usage; exit 0 ;; esac

# ---------- 前置：定位 GPU ----------
if ! command -v lspci >/dev/null 2>&1; then
    echo "错误: 找不到 lspci，请先: apt install -y pciutils"; exit 1
fi
GPU=$(lspci -D -nn 2>/dev/null | awk '/\[10de:(1f0b|220d)\]/{print $1; exit}')
if [ -z "$GPU" ]; then
    echo "错误: 未找到 CMP 40HX (10de:1f0b / 10de:220d)，机器插对了卡吗？驱动把卡吃掉了？"
    exit 1
fi
SYSFS="/sys/bus/pci/devices/$GPU"
UP=$(basename "$(readlink -f "$SYSFS" 2>/dev/null)" 2>/dev/null)   # sysfs 父目录 = 上游桥
DRV=$(basename "$(readlink "$SYSFS/driver" 2>/dev/null)" 2>/dev/null || echo "none")

# ---------- 拉取 EFI 阶段 trace 到仓库 logs/ ----------
# EFI app 只能写 ESP(FAT)，Linux 侧开机后把它拷进仓库 logs/ 便于归档。
# 尽力而为: 找不到 ESP/trace 就静默跳过, 不影响验收。
REPO_ROOT=$(cd "$(dirname "$0")/.." 2>/dev/null && pwd)
pull_trace() {
    [ -n "${REPO_ROOT:-}" ] || return 0
    local src
    for src in /boot/efi/EFI/UNLOCK/LOGS/BOOTER_TRACE.TXT \
               /boot/efi/EFI/UNLOCK/BOOTER_TRACE.TXT; do
        if [ -r "$src" ]; then
            mkdir -p "$REPO_ROOT/logs" 2>/dev/null || return 0
            cp -f "$src" "$REPO_ROOT/logs/booter_trace.txt" 2>/dev/null \
                && echo "[trace] 已拷贝 $src → logs/booter_trace.txt"
            return 0
        fi
    done
    echo "[trace] 未找到 EFI trace (EFI 未跑或 ESP 未挂在 /boot/efi), 跳过"
    return 0
}

# 定位可读的 booter trace（优先 pull_trace 拷到本地的副本，再回退 ESP 原件）。
# 命中则置全局 TRACE_FILE 并返回 0；否则清空并返回 1。
TRACE_FILE=""
trace_find() {
    local c
    for c in "${REPO_ROOT:-.}/logs/booter_trace.txt" \
             /boot/efi/EFI/UNLOCK/LOGS/BOOTER_TRACE.TXT \
             /boot/efi/EFI/UNLOCK/BOOTER_TRACE.TXT; do
        if [ -r "$c" ]; then TRACE_FILE="$c"; return 0; fi
    done
    TRACE_FILE=""
    return 1
}

# ---------- BAR0 寄存器读取（python3, 只读） ----------
# 算力白名单(SS0/SS1/FECS_PLM/XP3G_PLM) + ReBAR 佐证(CYA/CFG/CAP/CTL)
read_regs() {
    python3 - "$GPU" <<'PY'
import sys, os, mmap, struct
bdf = sys.argv[1]
# name, bar0 offset, 期望解锁值(无则 None)
QUICK = [
    ("SS0",          0x409664, 0x88888888),
    ("SS1",          0x40966C, 0x00000008),
    ("FECS_PLM",     0x409650, 0xFFFFFFFF),
    ("XP3G_PLM",     0x8E1B0,  0xFFFFFFFF),
    ("XVE_REBAR_CYA",0x88724,  0x00000030),
    ("XVE_REBAR_CFG",0x88DCC,  None),
    ("XVE_REBAR_CAP",0x88BBC,  None),
    ("XVE_RBAR_CTL", 0x88BC0,  None),
]
regs = QUICK
res = "/sys/bus/pci/devices/%s/resource0" % bdf
try:
    fd = os.open(res, os.O_RDWR | os.O_SYNC)
except OSError as e:
    print("REGFAIL 打开 %s 失败: %s (读 BAR0 需要 sudo)" % (res, e)); sys.exit(0)
size = os.fstat(fd).st_size
top = max(off for _, off, _ in regs) + 4
if size < top:
    print("REGFAIL BAR0 仅 0x%x 字节, 不足以读到 0x%x" % (size, top)); sys.exit(0)
mm = mmap.mmap(fd, size, mmap.MAP_SHARED, prot=mmap.PROT_READ)
vals = {}
print("  %-16s %-10s %-12s %s" % ("register", "offset", "read", "at target?"))
print("  " + "-" * 48)
for name, off, want in regs:
    v = struct.unpack("<I", mm[off:off + 4])[0]
    vals[name] = v
    state = "-" if want is None else ("YES" if v == want else "no")
    print("  %-16s 0x%06X    0x%08X    %s" % (name, off, v, state))
    sys.stdout.flush()          # 每行即时刷出: 万一某区域异常, 最后一行即断点
mm.close(); os.close(fd)

ss0, ss1 = vals.get("SS0"), vals.get("SS1")
if ss0 == 0x88888888 and ss1 == 0x00000008:
    print("  VERDICT COMPUTE: SS0/SS1 = 解锁值 (若 clpeak 吞吐达标才算真正确认)")
elif ss0 in (0x00000000, 0xFFFFFFFF, 0x00110000) and ss1 in (0x00000000, 0xFFFFFFFF, 0x00000006):
    print("  VERDICT COMPUTE: SS0/SS1 停在出厂态 -> UEFI 直写被 SEC2 权限层丢弃, 算力未解")
    print("                   (本机已知限制: 需 Booter 特权路线, 见 doc/40HX-路线B)")
else:
    print("  VERDICT COMPUTE: SS0=0x%08X SS1=0x%08X 既非锁定也非解锁值, sudo 重跑看完整寄存器表" % (ss0 or 0, ss1 or 0))
cya = vals.get("XVE_REBAR_CYA"); ctl = vals.get("XVE_RBAR_CTL"); cap = vals.get("XVE_REBAR_CAP")
sel = ((ctl or 0) >> 8) & 0x3F
print("  VERDICT REBAR:  CYA=0x%08X CTL选择子=%d CAP=0x%08X -> XVE aperture %s" %
      (cya or 0, sel, cap or 0, "已解锁(8GiB)" if (cya == 0x30 and sel == 13) else "未见解锁痕迹"))
PY
}

# ---------- 检查项 ----------
chk_env() {
    head2 "env · 环境前置"
    if [ -d /sys/firmware/efi ]; then ok env "UEFI 引导"
    else fail env "非 UEFI 引导, EFI 方案不适用"; fi

    local sb="unknown"
    if command -v mokutil >/dev/null 2>&1; then
        sb=$(mokutil --sb-state 2>/dev/null | head -1)
    elif [ -r /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c ]; then
        local sbv=$(od -An -tu1 -j4 -N1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c | tr -d ' ')
        if [ "$sbv" = "0" ]; then sb="SecureBoot disabled"; else sb="SecureBoot enabled"; fi
    fi
    case "$sb" in
        *disabled*) ok env "Secure Boot 已关闭" ;;
        *enabled*)  warn env "Secure Boot 开启: 未签名 .efi 会被拒绝加载" ;;
        *)          skip env "Secure Boot 状态不可读 (mokutil 未装)" ;;
    esac

    ok env "GPU $GPU 已识别, 上游桥 $UP"

    case "$DRV" in
        nvidia)     ok env "驱动绑定: nvidia" ;;
        vfio-pci)   warn env "驱动绑定: vfio-pci (卡已直通给 VM, 宿主侧无法验收)" ;;
        none)       fail env "无驱动绑定: nvidia 未加载或 probe 失败, 链路/BAR 结果只能看 lspci" ;;
        *)          warn env "驱动绑定: $DRV (预期 nvidia)" ;;
    esac

    if [ -r /proc/driver/nvidia/params ]; then
        local erb=$(awk -F': ' '/^EnableResizableBar/{print $2}' /proc/driver/nvidia/params)
        if [ "${erb:-0}" = "1" ]; then ok env "NVreg_EnableResizableBar=1"
        else fail env "NVreg_EnableResizableBar=${erb:-0} → Linux 不会做 BAR 重分配, ReBAR 验收必失败
         修复: echo 'options nvidia NVreg_EnableResizableBar=1' | sudo tee /etc/modprobe.d/nvidia-rebar.conf && sudo update-initramfs -u && sudo modprobe -r nvidia && sudo modprobe nvidia"
        fi
    else
        skip env "/proc/driver/nvidia/params 不可读 (nvidia 未加载?)"
    fi
}

# 通用链路检查: $1=BDF $2=标签
link_check() {
    local bdf=$1 tag=$2 sta ctl ctl2 speed width
    sta=$(lspci -vvv -s "$bdf" 2>/dev/null | grep -m1 'LnkSta:' || true)
    [ -z "$sta" ] && { fail gen2 "$tag: 读不到 LnkSta (无权限? 设备消失?)"; return; }
    speed=$(echo "$sta" | grep -oE 'Speed [0-9.]+GT/s' | awk '{print $2}')
    width=$(echo "$sta" | grep -oE 'Width x[0-9]+'  | awk '{print $2}')
    detail "  $tag LnkSta:  $sta"
    case "$speed" in
        5GT/s|8GT/s|16GT/s)
            if [ "$width" = "x16" ]; then ok gen2 "$tag $speed x16"
            else warn gen2 "$tag $speed 但 $width (非 x16)"; fi ;;
        2.5GT/s)
            fail gen2 "$tag 仍在 Gen1 (2.5GT/s $width) → 本次开机 EFI 阶段 Gen2 没做成, 重跑 compute" ;;
        *)  warn gen2 "$tag 速率异常: '$speed' $width" ;;
    esac
}

chk_gen2() {
    head2 "gen2 · PCIe 链路"
    link_check "$GPU" "端点 $GPU"
    [ -n "$UP" ] && [ -d "/sys/bus/pci/devices/$UP" ] && link_check "$UP" "上游 $UP"
    local ctl=$(lspci -vvv -s "$UP" 2>/dev/null | grep -m1 'LnkCtl:' || true)
    detail "  上游 LnkCtl:  $ctl"
    if echo "$ctl" | grep -q 'CommClk-'; then
        ok gen2 "上游 CommClk- (CCC=0, 解锁后的正确形态, 不要'修'成 +)"
    elif [ -n "$ctl" ]; then
        warn gen2 "上游 CommClk+ → 若当前已是 5GT/s 属正常(共时钟平台), 若掉回 Gen1 则说明被改回"
    fi
    local tls=$(lspci -vvv -s "$GPU" 2>/dev/null | grep -m1 'LnkCtl2:' || true)
    detail "  端点 LnkCtl2: $tls"
    echo "$tls" | grep -q 'Target Link Speed: 5GT/s' \
        && ok gen2 "端点 TLS=5GT/s 已锁定" \
        || warn gen2 "端点 TLS 非 5GT/s: ${tls##*LnkCtl2:}"
}

chk_rebar() {
    head2 "rebar · BAR1 8 GiB"
    # 0) 本次启动的内核命令行必须带 pci=realloc (以 /proc/cmdline 实测为准,
    #    手改 /etc/default/grub 未重烘条目的情况在这里会直接暴露)
    if grep -q 'pci=realloc' /proc/cmdline; then
        ok rebar "当前启动内核命令行含 pci=realloc"
    else
        fail rebar "当前启动内核命令行没有 pci=realloc → 内核不会重排 BAR, 8 GiB 大概率放不下
         修复: 重跑 sh install-efi.sh --force 重新烘焙 UNLOCK 条目后 reboot(单独 update-grub 对已烘焙条目无效)"
    fi

    # 1) sysfs 实际 BAR1 尺寸 (OS 已重分配的最终结果, 最硬证据)
    local bytes=0 s e
    if [ -r "$SYSFS/resource" ]; then
        read -r s e _ < <(awk 'NR==2' "$SYSFS/resource")
        # resource 里是 0x 前缀十六进制, 64 位 bash 直接参与算术
        [ -n "$s" ] && [ -n "$e" ] && bytes=$(( e - s + 1 ))
    fi
    bytes=${bytes:-0}
    if [ "$bytes" -ge 8589934592 ]; then
        ok rebar "BAR1 实际尺寸 $((bytes/1073741824)) GiB"
    elif [ "$bytes" -gt 0 ]; then
        fail rebar "BAR1 实际尺寸仅 $((bytes/1048576)) MiB (期望 8192) → 排查: ①EFI 屏幕 rebar 段是否 MISMATCH ②EnableResizableBar ③NVConfig 是否重置过卡"
    else
        skip rebar "sysfs resource 读不到 BAR1 (权限? 无驱动?)"
    fi
    detail "  $(lspci -vvv -s "$GPU" 2>/dev/null | grep -m1 'Region 1:')"

    # 2) PCIe Resizable BAR 能力 (EFI 写的尺寸表)
    local rsz=$(lspci -vvv -s "$GPU" 2>/dev/null | grep -A6 -m1 'Physical Resizable BAR' || true)
    detail "$rsz"
    # 新版 lspci 对 >=1GB 打印 "8GB" 而非 "8192MB", 两种格式都要认
    if echo "$rsz" | grep -qE '8192[ ]?MB|8[ ]?GB'; then
        ok rebar "lspci Resizable BAR 支持表含 8GiB (EFI XVE_CFG 生效)"
    elif [ -n "$rsz" ]; then
        fail rebar "Resizable BAR 支持表仍无 8GiB 档 → EFI 阶段 XVE_CFG 没写进去或被 SBR 后未重跑 compute"
    else
        skip rebar "lspci 未显示 Physical Resizable BAR 能力"
    fi

    # 3) nvidia-smi BAR1 (RM 视角)
    if command -v nvidia-smi >/dev/null 2>&1; then
        local bar1=$(nvidia-smi -q 2>/dev/null | grep -A3 'BAR1 Memory' | grep -m1 'Total' | grep -oE '[0-9]+' | head -1)
        if [ -n "${bar1:-}" ] && [ "$bar1" -ge 8000 ]; then
            ok rebar "nvidia-smi BAR1 Total ${bar1} MiB"
        elif [ -n "${bar1:-}" ]; then
            warn rebar "nvidia-smi BAR1 Total ${bar1} MiB (< 8192; 驱动未 resize 或驱动版本不支持)"
        else
            skip rebar "nvidia-smi 读不到 BAR1 Memory Usage"
        fi
    else
        skip rebar "nvidia-smi 不可用"
    fi

    # 4) 寄存器级佐证 (root 才读)
    if [ "$(id -u)" = "0" ]; then
        local rr=$(read_regs)
        echo "$rr" | grep 'VERDICT REBAR' >/dev/null && detail "$rr"
        if echo "$rr" | grep -q 'aperture 已解锁'; then
            ok rebar "BAR0 XVE 寄存器: aperture 处于解锁态"
        elif echo "$rr" | grep -q '未见解锁痕迹'; then
            warn rebar "BAR0 XVE 寄存器未见解锁痕迹 (驱动运行期可能重置了 XVE 镜像, 以 BAR1 实际尺寸为准)"
        fi
    else
        skip rebar "寄存器级佐证需 sudo (bash tools/check_unlock.sh 加 sudo 重跑)"
    fi
}

chk_driver() {
    head2 "driver · 驱动与日志健康"
    if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
        ok driver "nvidia-smi 正常: $(nvidia-smi -L 2>/dev/null | head -1)"
    else
        fail driver "nvidia-smi 不可用/无输出 → 驱动未就绪"
    fi

    local klog=""
    klog=$(dmesg 2>/dev/null || journalctl -k -b --no-pager 2>/dev/null || true)
    if [ -z "$klog" ]; then
        skip driver "内核日志不可读 (加 sudo 重跑)"
        return
    fi
    local xid=$(echo "$klog" | grep -ci 'NVRM.*Xid' || true)
    if [ "${xid:-0}" -eq 0 ]; then ok driver "无 Xid 记录"; else
        fail driver "发现 $xid 条 Xid → 逐条看: dmesg | grep Xid"
        detail "$(echo "$klog" | grep -i 'NVRM.*Xid' | tail -5)"
    fi
    local aer=$(echo "$klog" | grep -iE 'aer.*(0000:)?'"${GPU#0000:}" | grep -c 'error' || true)
    [ "${aer:-0}" -eq 0 ] && ok driver "GPU 无 AER 报错" \
        || { warn driver "GPU 相关 AER 报错 $aer 条"; detail "$(echo "$klog" | grep -iE 'aer' | grep "${GPU#0000:}" | tail -5)"; }
    local perr=$(echo "$klog" | grep -i 'pcieport' | grep -icE 'link (is )?down|data link layer|training failed|timeout' || true)
    [ "${perr:-0}" -eq 0 ] && ok driver "pcieport 无 link down/训练失败事件" \
        || { warn driver "pcieport 异常事件 $perr 条(开机 SBR 期间的 link down 属预期, 之后不应再有)"; \
             detail "$(echo "$klog" | grep -i 'pcieport' | grep -iE 'link (is )?down|data link layer|training failed|timeout' | tail -5)"; }
}

chk_compute() {
    head2 "compute · 算力寄存器 (SS0/SS1)"
    if [ "$(id -u)" != "0" ]; then
        skip compute "读 BAR0 需要 root: sudo bash tools/check_unlock.sh"
        return
    fi
    local out=$(read_regs)
    if echo "$out" | grep -q '^REGFAIL'; then
        fail compute "$out"
        return
    fi
    detail "$out"
    if echo "$out" | grep -q 'VERDICT COMPUTE: SS0/SS1 = 解锁值'; then
        ok compute "SS0/SS1 处于解锁值 (吞吐复核建议: clpeak WMMA fp16)"
    elif echo "$out" | grep -q '算力未解'; then
        warn compute "SS0/SS1 停在出厂态 — compute 直写被 SEC2 丢弃(预期); 若已跑 booter 菜单仍如此, 看下方 booter 项 trace 判读"
    else
        warn compute "寄存器值非两种参考态, 见上方明细"
    fi
}

# DIRECT_SEC2 首跑验收：解析 EFI 阶段 tee 出的 BOOTER_TRACE.TXT（无需 root，
# 不碰硬件）。逐里程碑判定，未跑到 [10] 时回显最后几条日志以定位断点（对应
# 交接文档 §3.2：>4G 身份映射挂 / 卡在某个 risky MMIO 读）。booter 日志经
# gpu_log 统一带 "[core] " 前缀落盘，故只 grep 行内子串。
chk_booter() {
    head2 "booter · DIRECT_SEC2 首跑 trace (BOOTER_TRACE.TXT)"
    if ! trace_find; then
        skip booter "未找到 BOOTER_TRACE.TXT (EFI booter 未跑, 或 ESP 未挂在 /boot/efi)"
        return
    fi
    detail "  trace: $TRACE_FILE ($(wc -l < "$TRACE_FILE" 2>/dev/null) 行)"

    # 0) 本次开机是否进过 DIRECT_SEC2 流程
    if ! grep -q 'DIRECT_SEC2 compute unlock' "$TRACE_FILE"; then
        skip booter "trace 里没有 DIRECT_SEC2 流程标记 (本次开机没选 booter 菜单项?)"
        return
    fi
    ok booter "已进入 DIRECT_SEC2 流程"

    # 1) prepare [6][7]: >4GiB 身份映射/分配 —— §3.2 头号风险
    if grep -q 'booter: prepare FAILED' "$TRACE_FILE"; then
        fail booter "prepare 失败 → >4GiB 身份映射/分配翻车 (交接文档 §3.2 第1条)"
        detail "$(grep -E 'prep: (alloc|DONE|v67|ucode|bl|dummy|radix|meta|fwsec)' "$TRACE_FILE" | tail -12 | sed 's/^/    /')"
    elif grep -q 'prep: DONE meta=' "$TRACE_FILE"; then
        ok booter "prepare 完成: 高缓冲分配 + 身份映射直接解引用 OK"
    else
        warn booter "prepare 既未 DONE 也未 FAILED → 疑似卡在 prepare 阶段 (看下方最后几行)"
    fi

    # 2) fwsec [8b]: WPR2 闩锁 (观测值, 不阻断 [9])
    local frc
    frc=$(grep -oE 'booter: fwsec -> -?[0-9]+' "$TRACE_FILE" | tail -1 | grep -oE -- '-?[0-9]+$')
    if [ -n "$frc" ]; then
        if [ "$frc" = "0" ]; then ok booter "fwsec [8b]: WPR2 已闩 (rc=0)"
        else warn booter "fwsec [8b]: WPR2 未闩 (rc=$frc) — 不阻断, 继续看 [9]"
        fi
    fi

    # 3) load [9]: DIRECT_SEC2 端口装载
    if grep -q 'mbox0=0 -> booter OK' "$TRACE_FILE"; then
        ok booter "load [9]: booter OK (mbox0=0)"
    elif grep -q 'booter: load_tu102_direct ->' "$TRACE_FILE"; then
        warn booter "load [9]: 未达 booter OK (mbox0!=0)"
    fi

    # 4) verdict [10]: 有没有跑完
    if grep -qF 'booter: *** UNLOCKED' "$TRACE_FILE"; then
        ok booter "[10] EFI 阶段判定: *** UNLOCKED (SS0=88888888 SS1=8) — 进系统算/吐量复核"
    elif grep -q 'booter: RESULT PLM=' "$TRACE_FILE"; then
        warn booter "[10] 跑完但未解锁 (最后 RESULT 行):"
        detail "$(grep 'booter: RESULT PLM=' "$TRACE_FILE" | tail -1 | sed 's/^/    /')"
    else
        fail booter "未到 [10] 判定 → 疑似硬挂在某个 MMIO 读; 最后日志 (定位断点):"
        detail "$(tail -8 "$TRACE_FILE" | sed 's/^/    /')"
    fi
    return 0
}

# ---------- 快速模式 ----------
run_quick() {
    echo "CMP40HX 解锁验收  GPU=$GPU  上游=$UP  驱动=$DRV  $(date '+%F %T')"
    pull_trace
    chk_env
    chk_gen2
    chk_rebar
    chk_driver
    chk_compute
    chk_booter
    echo
    printf '总体: %s%d PASS%s  %s%d WARN%s  %s%d FAIL%s  %s%d SKIP%s\n' \
        "$C_G" "$N_PASS" "$C_Z" "$C_Y" "$N_WARN" "$C_Z" "$C_R" "$N_FAIL" "$C_Z" "$C_B" "$N_SKIP" "$C_Z"
    if [ "$N_FAIL" -gt 0 ] && [ "$N_SKIP" -eq 0 ]; then
        echo "结论: 有未通过项, DETAIL=1 重跑可看逐项原始明细"
    elif [ "$N_FAIL" -eq 0 ] && [ "$N_WARN" -eq 0 ]; then
        echo "结论: Gen2 + ReBAR 全部到位 (算力见 warn/skip 项)"
    else
        echo "结论: 基本可用, 按 WARN 提示逐项确认"
    fi
    [ "$N_FAIL" -eq 0 ]
}

# ---------- 入口 ----------
case "${1:-}" in
    -h|--help) usage ;;
    "")        run_quick ;;
    *)         usage; exit 2 ;;
esac
