#!/bin/sh
# install-efi.sh — NVPermissiveEFI (CMP40HX UEFI 解锁) 安装脚本
#
# 做四件事:
#   1) 幂等补齐 Linux 侧 ReBAR 两个前置配置(已存在则不动, 缺失才写):
#      a. /etc/modprobe.d/nvidia-rebar.conf → options nvidia NVreg_EnableResizableBar=1
#      b. /etc/default/grub 的 GRUB_CMDLINE_LINUX_DEFAULT 追加 pci=realloc
#      (b 改完必须先 update-grub 再烘焙菜单项, 否则 UNLOCK 条目的内核行没有该参数)
#   2) 把 2 个 .efi 拷到 ESP: /boot/efi/EFI/UNLOCK/
#   3) 生成 /etc/grub.d/40-cmp40hx —— 两个新 menuentry,
#      在启动 PVE 内核前用 chainloader + boot 运行 NVPermissiveEFI,
#      兼容 PVE 8.4 的 GRUB 2.06 与 PVE 9。
#   4) update-grub 并校验
#   5) 检查开机默认启动项: 没指向本方案则自动设为 booter(幂等, 已有不动)
#
# 菜单项有两个: inspect / booter。
# booter 是 DIRECT_SEC2 一条龙解锁(算力提权 + Gen2 + ReBAR, v70 移植):
#   走 SEC2/GSP booter 提权, 不经 core ELF, 不做 SBR; 首次实机需盯 BOOTER_TRACE.TXT 定位。
# inspect 是只读诊断(零风险)。
# 早期的 compute / full 独立模式已随源码退役: booter 一条龙已覆盖其全部效果。
#
# 已安装过本方案时: 交互模式下问一句(输入 yes 或 y 则自动卸载后重装);
# 带参数运行时直接自动卸载重装, 不问。
# 安装完成后: 交互模式进入可选菜单; 带参数运行时一次性做完, 不停顿。
#
# 用法:
#   安装方式二选一(不带参数时交互询问, 其它输入直接退出):
#     1 = 编译安装   : 先 make release, 再用刚编出来的 .efi
#     2 = 预编译安装 : 直接用仓库里现成的 .efi, 不编译
#
#   sh install-efi.sh                        交互式: 先选安装方式, 装完进入可选菜单
#   sh install-efi.sh --build                编译后安装(全自动)
#   sh install-efi.sh --prebuilt             预编译安装(全自动)
#   sh install-efi.sh --force                已安装则直接卸载重装, 不问; 用预编译的 .efi
#   sh install-efi.sh --default=booter       装完并把默认启动项设为 booter
#   sh install-efi.sh --timeout=5            装完并把菜单停留时间设为 5 秒
#   sh install-efi.sh --build --force --default=booter --timeout=5   一条命令全自动
#
#   --default 接受: inspect | booter, 或 grub.cfg 里的完整标题
#   --no-default 明确不设默认项: 开机仍进普通 PVE(不解锁)
#   不给 --default/--no-default 时, 脚本会检查默认启动项是否已指向本方案,
#   没指向则自动补为 booter(出厂的 GRUB_DEFAULT=0 也算没指向)
#   --timeout 接受: 非负整数(秒)
#   带任一参数即全自动, 中间不会有任何人工停顿;
#   只有显式给 --build 才会编译, 其余情况下(含只带 --force/--default/--timeout)
#   一律使用仓库里现成的 .efi。
#
# 注意: 本文件必须是 LF 换行。带 CRLF 时 dash 会报
#       "install-efi.sh: NN: set: Illegal option -"。
# 检查: grep -c $'\r' install-efi.sh   (必须是 0)
# 修法: sed -i 's/\r$//' install-efi.sh
set -e
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ESP=${ESP:-/boot/efi}
GC=${GC:-/boot/grub/grub.cfg}
F=${F:-/etc/grub.d/40-cmp40hx}
GRUB_DEFAULT_FILE=${GRUB_DEFAULT_FILE:-/etc/default/grub}
MODPROBE_CONF=${MODPROBE_CONF:-/etc/modprobe.d/nvidia-rebar.conf}

# 菜单标题(唯一来源: 生成菜单和 --default 校验都用这里)
T_INSPECT="PVE + CMP40HX UNLOCK (inspect, 只读诊断: 不写寄存器, 无 SBR, 零风险)"
T_BOOTER="PVE + CMP40HX UNLOCK (booter, 一条龙解锁: 算力提权 + Gen2 + ReBAR)"

usage() {
    cat <<'EOF'
用法: sh install-efi.sh [选项]

安装方式(不带参数时交互询问, 其它输入直接退出):
  1 = 编译安装    先 make release, 再用刚编出来的 .efi
  2 = 预编译安装  直接用仓库里现成的 .efi, 不编译

  (无参数)              交互式安装, 装完进入可选菜单
  -b, --build           先 make release 再安装
  -p, --prebuilt        直接用现成的 .efi 安装(默认行为)
  -f, --force           已安装时直接卸载重装, 不再询问
      --default=<标题>  安装后设置默认启动项
                        可写 inspect / booter, 或完整标题(逐字节匹配)
      --no-default      不设置也不自动补默认启动项(开机保持普通 PVE)
      --timeout=<秒>    安装后设置菜单停留时间(非负整数)
  -h, --help            显示本帮助

带任一选项即为全自动模式: 不询问、不进入可选菜单, 装完直接退出。
只有给了 --build 才会编译; 不给 --build(或给 --prebuilt)一律用现成的 .efi。
装完会检查开机默认启动项: 未指向本方案时自动设为 booter (--no-default 可关)。

安装时会幂等补齐两个 Linux 侧 ReBAR 前置(已存在则不动):
  /etc/modprobe.d/nvidia-rebar.conf → options nvidia NVreg_EnableResizableBar=1
  /etc/default/grub 的 GRUB_CMDLINE_LINUX_DEFAULT 追加 pci=realloc
uninstall-efi.sh 会对称回收这两项。
EOF
}

# 从 grub.cfg 里取出所有启动项的完整标题(不含 submenu)
grub_menu_names() {
    grep '^menuentry ' "$GC" | sed 's/^menuentry "//; s/".*$//'
}

# 重新生成 grub.cfg
regen() {
    if command -v update-grub >/dev/null 2>&1; then
        update-grub > /tmp/update-grub.cmp40hx.log 2>&1 || {
            echo "错误: update-grub 失败, 详见 /tmp/update-grub.cmp40hx.log"
            return 1
        }
    else
        echo "(未找到 update-grub 命令, 跳过重新生成 grub.cfg)"
    fi
    return 0
}

# 把 inspect/booter 缩写展开成完整标题
expand_title() {
    case $1 in
        inspect) echo "$T_INSPECT" ;;
        booter)  echo "$T_BOOTER" ;;
        *)       echo "$1" ;;
    esac
}

# 读出 GRUB_DEFAULT 的当前值(去掉成对的外层引号); 没这一行则输出空
cur_grub_default() {
    line=
    val=
    [ -f "$GRUB_DEFAULT_FILE" ] || return 0
    line=$(grep -m1 '^GRUB_DEFAULT=' "$GRUB_DEFAULT_FILE" 2>/dev/null || true)
    [ -n "$line" ] || return 0
    val=${line#GRUB_DEFAULT=}
    # 只去掉成对的外层引号, 免得把标题内部的引号误切
    if [ "${val#\'}" != "$val" ] && [ "${val%\'}" != "$val" ]; then
        val=${val#\'}; val=${val%\'}
    elif [ "${val#\"}" != "$val" ] && [ "${val%\"}" != "$val" ]; then
        val=${val#\"}; val=${val%\"}
    fi
    printf '%s' "$val"
}

# 写默认启动项: 先删旧的 GRUB_DEFAULT 行(可能有重复), 再追加一条
set_grub_default() {
    sed -i '/^GRUB_DEFAULT=/d' "$GRUB_DEFAULT_FILE"
    printf "GRUB_DEFAULT='%s'\n" "$1" >> "$GRUB_DEFAULT_FILE"
}

# 安装末尾兼容检查: 确保开机默认项落在本方案上。幂等规则:
#   - 已指向 "PVE + CMP40HX UNLOCK ..."  -> 不动(保留用户选的 inspect/booter)
#   - 指向另一个真实存在的条目         -> 尊重现有配置(故意默认不解锁的人)
#   - 没这行 / 出厂默认(0、saved、1>0 这种) -> 自动补 booter
# 返回 0 = 已改动 GRUB_DEFAULT(调用方需要 regen), 1 = 未改动
ensure_default_entry() {
    cur=$(cur_grub_default)
    echo
    echo "-------- 默认启动项检查 (幂等: 已指向本方案则不动) --------"
    case $cur in
        'PVE + CMP40HX UNLOCK'*)
            echo "  已指向本方案, 未改动: $cur"
            return 1 ;;
    esac
    if [ -n "$OPT_NO_DEFAULT" ]; then
        echo "  按 --no-default 未改动 (开机默认进普通 PVE = 不解锁)"
        echo "  以后要让开机默认走解锁: sh install-efi.sh --default=booter"
        return 1
    fi
    if [ -n "$cur" ] && printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$cur"; then
        echo "  当前指向其它条目, 尊重现有配置未改动: $cur"
        echo "  要让开机默认走解锁: sh install-efi.sh --default=booter"
        return 1
    fi
    want=$(expand_title booter)
    if ! printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$want"; then
        echo "  警告: $GC 里找不到 booter 菜单项, 默认启动项未改动"
        echo "        (通常见原因: update-grub 未执行或失败, 菜单项还没生效)"
        return 1
    fi
    set_grub_default "$want"
    echo "  未设置过默认启动项(当前值: ${cur:-无}), 已自动设为 booter:"
    echo "     $want"
    return 0
}

CUR_TIMEOUT() {
    if [ -f "$GRUB_DEFAULT_FILE" ]; then
        grep '^GRUB_TIMEOUT=' "$GRUB_DEFAULT_FILE" 2>/dev/null || echo "GRUB_TIMEOUT 未设置 (GRUB 默认)"
    else
        echo "找不到 $GRUB_DEFAULT_FILE"
    fi
}

# ---------- Linux 侧 ReBAR 前置配置(幂等) ----------
# 8 GiB BAR1 需要 Linux 侧两个开关, 缺一个 ReBAR 就落不了地:
#   ① nvidia 模块参数 NVreg_EnableResizableBar=1 —— 驱动加载时对 BAR1 做 resize
#   ② 内核命令行 pci=realloc —— 允许内核重排 BAR / 撑大上游桥窗口
# 规则: 没有就补, 已有不动; uninstall-efi.sh 对称回收。
REBAR_OPT='options nvidia NVreg_EnableResizableBar=1'

ensure_rebar_modprobe() {
    if grep -qs '^options nvidia .*NVreg_EnableResizableBar=1' /etc/modprobe.d/*.conf; then
        echo "  ① 已存在, 未改动: /etc/modprobe.d 已设 NVreg_EnableResizableBar=1"
        return 0
    fi
    echo "  ① 正在写入 $MODPROBE_CONF ..."
    printf '# 由 install-efi.sh 写入: ReBAR 前置, 驱动加载时对 BAR1 做 resize\n%s\n' "$REBAR_OPT" > "$MODPROBE_CONF"
    echo "     写入完成"
    if command -v update-initramfs >/dev/null 2>&1; then
        echo "     正在更新 initramfs (update-initramfs -u), 约需十几秒, 请稍候..."
        if update-initramfs -u >/dev/null; then
            echo "     initramfs 更新完成"
        else
            echo "     警告: update-initramfs 失败, 请手动执行: update-initramfs -u"
        fi
    fi
}

ensure_pci_realloc() {
    if [ ! -f "$GRUB_DEFAULT_FILE" ]; then
        echo "  ② 警告: 找不到 $GRUB_DEFAULT_FILE, pci=realloc 未处理;"
        echo "         请自行把 pci=realloc 加进内核 cmdline 后重跑本脚本"
        return 0
    fi
    if grep -q 'pci=realloc' "$GRUB_DEFAULT_FILE"; then
        echo "  ② 已存在, 未改动: $GRUB_DEFAULT_FILE 已含 pci=realloc"
    else
        TMP_GRUB="${GRUB_DEFAULT_FILE}.cmp40hx.tmp"
        echo "  ② 正在写入 $GRUB_DEFAULT_FILE (追加 pci=realloc) ..."
        if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT="' "$GRUB_DEFAULT_FILE"; then
            awk '!done && /^GRUB_CMDLINE_LINUX_DEFAULT="/ { sub(/"$/, " pci=realloc\""); done=1 }
                 { print }' "$GRUB_DEFAULT_FILE" > "$TMP_GRUB" &&
                mv "$TMP_GRUB" "$GRUB_DEFAULT_FILE" || {
                rm -f "$TMP_GRUB"; echo "     错误: 写入 $GRUB_DEFAULT_FILE 失败"; return 1; }
            echo "     写入完成: GRUB_CMDLINE_LINUX_DEFAULT 已追加 pci=realloc"
        else
            printf 'GRUB_CMDLINE_LINUX_DEFAULT="pci=realloc"\n' >> "$GRUB_DEFAULT_FILE"
            echo "     写入完成: 没有 GRUB_CMDLINE_LINUX_DEFAULT 行, 已另起一行追加"
        fi
    fi
    # UNLOCK 条目正文是从 grub.cfg 烘焙的快照: 必须先把 /etc/default/grub 的改动
    # update-grub 落进 grub.cfg 再烘焙, 否则实际开机的内核行里没有 pci=realloc。
    # 即使本次未改动也跑一次: 防用户手改了文件却没 update-grub, 烘焙到的仍是旧快照。
    echo "     正在刷新 grub.cfg (update-grub), 请稍候..."
    regen || { echo "     错误: update-grub 失败"; return 1; }
    echo "     grub.cfg 刷新完成"
}

# ---------- 参数解析 ----------
AUTO=0
OPT_FORCE=0
OPT_BUILD=0
OPT_DEFAULT=
OPT_NO_DEFAULT=
OPT_TIMEOUT=
while [ $# -gt 0 ]; do
    case $1 in
        -h|--help)
            usage; exit 0 ;;
        -b|--build)
            AUTO=1; OPT_BUILD=1 ;;
        -p|--prebuilt)
            AUTO=1; OPT_BUILD=0 ;;
        -f|--force)
            AUTO=1; OPT_FORCE=1 ;;
        --default=*)
            AUTO=1; OPT_DEFAULT=${1#*=} ;;
        --default)
            AUTO=1
            shift
            [ $# -gt 0 ] || { echo "错误: --default 后面要跟标题"; exit 2; }
            OPT_DEFAULT=$1 ;;
        --no-default)
            AUTO=1; OPT_NO_DEFAULT=1 ;;
        --timeout=*)
            AUTO=1; OPT_TIMEOUT=${1#*=} ;;
        --timeout)
            AUTO=1
            shift
            [ $# -gt 0 ] || { echo "错误: --timeout 后面要跟秒数"; exit 2; }
            OPT_TIMEOUT=$1 ;;
        *)
            echo "错误: 未知参数: $1"
            echo
            usage
            exit 2 ;;
    esac
    shift
done

# 参数先做静态校验, 不要在装完才发现写不进去
if [ -n "$OPT_TIMEOUT" ]; then
    case $OPT_TIMEOUT in
        ''|*[!0-9]*) echo "错误: --timeout 必须是非负整数秒, 收到: $OPT_TIMEOUT"; exit 2 ;;
    esac
fi

# ---------- 前置检查 ----------
if [ -z "$SKIP_EFI_CHECK" ]; then
    [ -d /sys/firmware/efi ] || { echo "错误: 本机不是 UEFI 启动, EFI 方案不适用"; exit 1; }
fi
[ -d "$ESP" ] || { echo "错误: ESP 不在 $ESP, 运行时用 ESP=<挂载点> $0 覆盖"; exit 1; }
[ -f "$GC" ] || { echo "错误: 找不到 $GC, 运行时用 GC=<路径> $0 覆盖"; exit 1; }

if command -v mokutil >/dev/null 2>&1; then
    SB=$(mokutil --sb-state 2>/dev/null || true)
    echo "Secure Boot: $SB"
    case "$SB" in
        *"SecureBoot enabled"*)
            echo "错误: Secure Boot 开启会拒绝未签名的 .efi。先断电进 BIOS 关闭 Secure Boot 再运行本脚本。"
            exit 1 ;;
    esac
fi

# ---------- 选择安装方式 ----------
# 带参数时由 --build / --prebuilt 决定(默认预编译, 不编译);
# 不带参数时交互询问, 输入 1/2 以外的任何东西都直接退出、不改动文件。
MODE_BUILD=$OPT_BUILD
if [ "$AUTO" -eq 0 ]; then
    echo
    echo "请选择安装方式:"
    echo "  1) 编译安装   : 先 make release, 再用刚编出来的 .efi"
    echo "  2) 预编译安装 : 直接用仓库里现成的 .efi, 不编译"
    printf "请输入 1 或 2 (其它输入退出): "
    if IFS= read -r PICK; then :; else PICK=; fi
    case "$PICK" in
        1) MODE_BUILD=1 ;;
        2) MODE_BUILD=0 ;;
        *)
            echo "已退出, 未改动任何文件。"
            exit 0 ;;
    esac
fi

# ---------- 编译 ----------
if [ "$MODE_BUILD" -eq 1 ]; then
    echo
    echo "-------- 编译 (make release) --------"
    [ -f "$DIR/Makefile" ] || { echo "错误: 找不到 $DIR/Makefile"; exit 1; }
    command -v make >/dev/null 2>&1 || {
        echo "错误: 找不到 make。先装依赖: apt install -y build-essential binutils python3"
        exit 1
    }
    for t in gcc ld objcopy python3; do
        command -v "$t" >/dev/null 2>&1 || {
            echo "错误: 找不到 $t。先装依赖: apt install -y build-essential binutils python3"
            exit 1
        }
    done
    if ! ( cd "$DIR" && make release ); then
        echo
        echo "错误: make release 失败, 未做任何安装改动。"
        echo "      若报 'unrecognised emulation mode: i386pep', 说明 ld 缺 PE 后端,"
        echo "      改用 mingw 交叉工具链:"
        echo "      make release CC=x86_64-w64-mingw32-gcc LD=x86_64-w64-mingw32-ld OBJCOPY=x86_64-w64-mingw32-objcopy"
        exit 1
    fi
    echo "-------- 编译完成 --------"
    echo
fi

# ---------- 产物检查 ----------
# 产物统一落在 build/efi/ (make release 生成)。必须齐全。上游原版对缺失文件是
# 静默跳过, 结果 ESP 里留着上一次的旧 .efi, 脚本照样报"安装完成"——极容易被误判成"新代码无效"。
EFI_OUT=$DIR/build/efi
EFI_FILES='NVPermissiveEFI-INSPECT.efi NVPermissiveEFI-BOOTER.efi'
for f in $EFI_FILES; do
    [ -f "$EFI_OUT/$f" ] || {
        echo "错误: 缺少 $EFI_OUT/$f"
        if [ "$MODE_BUILD" -eq 1 ]; then
            echo "      编译没有产出这个文件, 请看上面的 make 输出"
        else
            echo "      选 1) 编译安装, 或先手动跑 make release 再重跑本脚本"
        fi
        exit 1
    }
done

# ---------- 0) 已安装检测 ----------
FOUND=0
if [ -f "$F" ]; then
    echo "  已存在: $F"
    FOUND=1
fi
for f in "$ESP/EFI/UNLOCK/NVPermissiveEFI-INSPECT.efi" \
         "$ESP/EFI/UNLOCK/NVPermissiveEFI-BOOTER.efi" \
         "$ESP/EFI/UNLOCK/NVPermissiveEFI.efi"; do
    # 最后一个是旧版 inspect 的文件名: 只要 ESP 里还有本方案任何东西就算已装,
    # 走重装流程后 uninstall-efi.sh 会整个移除 EFI/UNLOCK, 旧文件不会残留
    if [ -f "$f" ]; then
        echo "  已存在: $f"
        FOUND=1
    fi
done

if [ "$FOUND" -ne 0 ]; then
    if [ "$AUTO" -eq 1 ]; then
        echo
        echo "检测到已安装, 全自动模式: 直接卸载后重装。"
    else
        echo
        echo "检测到本机已经安装过 NVPermissiveEFI。"
        echo "输入 yes 或 y: 先卸载, 再重新安装"
        echo "回车或其它: 退出 (不改动任何文件)"
        printf "请输入: "
        read -r ANS || ANS=
        case "$ANS" in
            yes|YES|Yes|y|Y) ;;
            *)
                echo "已取消安装, 未改动任何文件。"
                exit 0 ;;
        esac
    fi
    [ -f "$DIR/uninstall-efi.sh" ] || { echo "错误: 找不到 $DIR/uninstall-efi.sh, 无法自动卸载"; exit 1; }
    echo
    echo "-------- 执行卸载 --------"
    sh "$DIR/uninstall-efi.sh"
    echo "-------- 卸载完成, 开始安装 --------"
    echo
fi

# ---------- 1) Linux 侧 ReBAR 前置配置 ----------
echo
echo "-------- Linux 侧 ReBAR 前置配置 (幂等: 已有不动, 缺失补齐) --------"
ensure_rebar_modprobe
ensure_pci_realloc || { echo "错误: update-grub 失败, pci=realloc 无法进入 UNLOCK 条目"; exit 1; }
echo "[1/4] ReBAR 前置配置就绪"

# ---------- 2) ESP ----------
mkdir -p "$ESP/EFI/UNLOCK"
for f in $EFI_FILES; do
    install -m 0755 "$EFI_OUT/$f" "$ESP/EFI/UNLOCK/$f"
    # 覆盖后逐字节比对, 确认 ESP 里就是这一份而不是上一次的残留
    if command -v cmp >/dev/null 2>&1; then
        cmp -s "$EFI_OUT/$f" "$ESP/EFI/UNLOCK/$f" || {
            echo "错误: $ESP/EFI/UNLOCK/$f 与 $EFI_OUT/$f 不一致 (ESP 写入失败或只读)"
            exit 1
        }
    fi
done
echo "[2/4] ESP 安装完成: $ESP/EFI/UNLOCK/"
if [ "$MODE_BUILD" -eq 1 ]; then
    echo "      来源: 本次 make release 的编译产物"
else
    echo "      来源: build/efi/ 里现成的 .efi (未编译)"
fi
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$ESP/EFI/UNLOCK" && sha256sum $EFI_FILES )
fi

# ---------- 3) GRUB menuentry ----------
PVE_BLOCK=$(awk '/^menuentry.*Proxmox/{f=1} f{print} f&&/^\}/{exit}' "$GC")
[ -n "$PVE_BLOCK" ] || { echo "错误: 在 $GC 中找不到 Proxmox menuentry, 无法生成引导项"; exit 1; }
# PVE 原条目去掉第一行 menuentry 头, 剩余部分(含结尾 })作为新条目主体
PVE_BODY=$(printf '%s\n' "$PVE_BLOCK" | sed '1d')

MENU_COUNT=0
{
    echo '#!/bin/sh'
    echo '# Auto-generated by install-efi.sh — NVPermissiveEFI CMP40HX UEFI unlock'
    echo "cat <<'EOF'"
    for M in INSPECT BOOTER; do
        case $M in
            INSPECT) MFI=NVPermissiveEFI-INSPECT;  ARG=inspect; TITLE=$T_INSPECT ;;
            BOOTER)  MFI=NVPermissiveEFI-BOOTER;   ARG=booter;  TITLE=$T_BOOTER ;;
        esac
        MENU_COUNT=$((MENU_COUNT + 1))
        echo "menuentry \"$TITLE\" {"
        echo '    insmod chain'
        echo '    if search --no-floppy --set=unlockroot --file /EFI/UNLOCK/'$MFI'.efi; then'
        echo '        set save_root=$root'
        echo '        set root=$unlockroot'
        echo '        echo "NVPermissiveEFI: '"$M"' ..."'
        echo '        chainloader /EFI/UNLOCK/'$MFI'.efi '$ARG
        echo '        boot'
        echo '        set root=$save_root'
        echo '    else'
        echo '        echo "ERROR: /EFI/UNLOCK/'$MFI'.efi not found, normal boot"'
        echo '    fi'
        printf '%s\n' "$PVE_BODY"
        echo ''
    done
    echo 'EOF'
} > "$F"
chmod 0755 "$F"
echo "[3/4] 已生成 $F"

# ---------- 4) update-grub ----------
if command -v update-grub >/dev/null 2>&1; then
    echo "正在刷新启动菜单 (update-grub), 请稍候..."
    if update-grub > /tmp/update-grub.cmp40hx.log 2>&1; then
        # 只数 menuentry 行。不能数全文件: 设了 GRUB_DEFAULT 时 grub.cfg 里会
        # 多一行 set default="PVE + CMP40HX UNLOCK (...)", 会数错。
        N=$(grep -c '^menuentry "PVE + CMP40HX UNLOCK' "$GC" || true)
        echo "[4/4] update-grub 完成, $GC 中含 $N 个新菜单项 (应为 $MENU_COUNT)"
        if [ "$N" -ne "$MENU_COUNT" ]; then
            echo "警告: 菜单项数量不对, 多半是 /etc/grub.d 里还有老版本脚本留下的可执行备份在重复执行。"
            echo "      查看: ls -l /etc/grub.d | grep -i cmp40"
            echo "      修法: sudo mv /etc/grub.d/*.bak /root/grub-backup/ && sudo update-grub"
            ls -l /etc/grub.d | grep -i cmp40 || true
        fi
    else
        echo "错误: update-grub 失败, 详见:"
        cat /tmp/update-grub.cmp40hx.log
        exit 1
    fi
else
    echo "[4/4] 未找到 update-grub 命令 (测试环境?), 跳过重新生成 grub.cfg"
fi

echo
echo "安装完成。"

# ---------- 4) 全自动模式: 按参数一次做完 ----------
if [ "$AUTO" -eq 1 ]; then
    CHANGED=0
    if [ -n "$OPT_DEFAULT" ]; then
        TITLE=$(expand_title "$OPT_DEFAULT")
        if printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$TITLE"; then
            set_grub_default "$TITLE"
            echo "已设置默认启动项: $TITLE"
            CHANGED=1
        else
            echo "错误: '$TITLE' 不在 $GC 里, 默认启动项未设置。"
            echo "      用 --default=inspect|booter 最稳妥; 也可先 grep '^menuentry' $GC 看实际标题"
            exit 1
        fi
    elif ensure_default_entry; then
        CHANGED=1
    fi
    if [ -n "$OPT_TIMEOUT" ]; then
        sed -i '/^GRUB_TIMEOUT=/d' "$GRUB_DEFAULT_FILE"
        printf "GRUB_TIMEOUT=%s\n" "$OPT_TIMEOUT" >> "$GRUB_DEFAULT_FILE"
        echo "已设置菜单停留时间: ${OPT_TIMEOUT} 秒"
        CHANGED=1
    fi
    if [ "$CHANGED" -ne 0 ]; then
        regen || exit 1
    fi
    echo "全部完成, 重启后生效。"
    exit 0
fi

# ---------- 5) 交互模式: 先保证默认项, 再进可选菜单 ----------
if ensure_default_entry; then
    regen || exit 1
fi

# ---------- 6) 交互模式: 可选设置 ----------
while :; do
    echo
    echo "可选操作:"
    echo "  1) 查看当前 GRUB 启动项名称"
    echo "  2) 设置默认启动项"
    echo "  3) 设置菜单停留时间"
    echo "  4) 查看当前菜单停留时间"
    echo "  5) 清空默认启动项"
    echo "  0) 退出"
    printf "请选择: "
    if IFS= read -r ANS; then :; else
        echo
        echo "输入已结束, 退出。"
        break
    fi
    case "$ANS" in
        1)
            echo
            echo "当前 GRUB 启动项 (完整标题, 设置默认项时要一字不差地输入):"
            grub_menu_names | nl -ba -w2 -s'. '
            ;;
        2)
            echo
            echo "输入要设为默认的启动项完整标题 (可先选 1 查看, 也可直接输 inspect/booter):"
            printf "标题: "
            if IFS= read -r TITLE; then :; else TITLE=; fi
            [ -n "$TITLE" ] || { echo "错误: 标题不能为空, 未做改动。"; continue; }
            TITLE=$(expand_title "$TITLE")
            if printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$TITLE"; then
                set_grub_default "$TITLE"
                regen || continue
                echo "已设置默认启动项: $TITLE"
            else
                echo "错误: '$TITLE' 不是当前 grub.cfg 里的启动项标题, 未做改动。"
                echo "      常见原因: 少了空格/括号, 或括号用了全角 （ ）"
            fi
            ;;
        3)
            echo
            echo "输入菜单停留秒数 (例如 5):"
            printf "秒数: "
            if IFS= read -r SEC; then :; else SEC=; fi
            case "$SEC" in
                ''|*[!0-9]*) echo "错误: '$SEC' 不是非负整数, 未做改动。"; continue ;;
            esac
            sed -i '/^GRUB_TIMEOUT=/d' "$GRUB_DEFAULT_FILE"
            printf "GRUB_TIMEOUT=%s\n" "$SEC" >> "$GRUB_DEFAULT_FILE"
            regen || continue
            echo "已设置菜单停留时间: ${SEC} 秒"
            if grep -q '^GRUB_TIMEOUT_STYLE=hidden' "$GRUB_DEFAULT_FILE" 2>/dev/null; then
                echo "注意: $GRUB_DEFAULT_FILE 里有 GRUB_TIMEOUT_STYLE=hidden, 菜单会被跳过, 上面的秒数不生效。"
                echo "      需要显示菜单: sed -i 's/^GRUB_TIMEOUT_STYLE=hidden/GRUB_TIMEOUT_STYLE=menu/' $GRUB_DEFAULT_FILE && update-grub"
            fi
            ;;
        4)
            echo
            echo "当前设置:"
            CUR_TIMEOUT
            echo "默认启动项:"
            if [ -f "$GRUB_DEFAULT_FILE" ]; then
                grep '^GRUB_DEFAULT=' "$GRUB_DEFAULT_FILE" 2>/dev/null || echo "  GRUB_DEFAULT 未设置 (GRUB 默认取第一条)"
            fi
            if grep -q '^GRUB_TIMEOUT_STYLE=hidden' "$GRUB_DEFAULT_FILE" 2>/dev/null; then
                echo "提示: GRUB_TIMEOUT_STYLE=hidden, 菜单会被跳过, 停留时间不生效。"
            fi
            ;;
        5)
            echo
            if [ -f "$GRUB_DEFAULT_FILE" ] && grep -q '^GRUB_DEFAULT=' "$GRUB_DEFAULT_FILE"; then
                sed -i '/^GRUB_DEFAULT=/d' "$GRUB_DEFAULT_FILE"
                regen || continue
                echo "已清空默认启动项 (GRUB 恢复为取菜单第一条, 即原来的 PVE 条目 = 不解锁)"
                echo "提醒: 下次重跑 install-efi.sh 时会被自动补回 booter; 想保持不解锁请用 --no-default"
            else
                echo "当前本来就没有设置 GRUB_DEFAULT, 无需清空。"
            fi
            ;;
        0)
            echo "退出。"
            break
            ;;
        *)
            echo "无效输入: '$ANS', 请重新选择 (0 退出)。"
            ;;
    esac
done
