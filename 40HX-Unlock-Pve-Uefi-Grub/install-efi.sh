#!/bin/sh
# install-efi.sh — NVPermissiveEFI (CMP40HX UEFI 解锁) 安装脚本
#
# 做三件事:
#   1) 把 3 个 .efi 拷到 ESP: /boot/efi/EFI/UNLOCK/
#   2) 生成 /etc/grub.d/40-cmp40hx —— 三个新 menuentry,
#      在启动 PVE 内核前用 chainloader + boot 运行 NVPermissiveEFI,
#      兼容 PVE 8.4 的 GRUB 2.06 与 PVE 9。
#   3) update-grub 并校验
#
# 菜单项只有三个: inspect / compute / full。
# 上游的独立 "gen2" 菜单项仍然不出条目: compute 末尾已包含 Gen2 策略 + retrain。
# 源码里的 gen2-ld 模式(上游 Link Disable 强制重训)也保留着, 但不出菜单:
# 普通重训在 CCC 对齐后已实测能到 Gen2 x16, 不再需要断链。
# 如需研究, 手改 /etc/grub.d/40-cmp40hx 里对应行的参数为 gen2-ld。
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
#   sh install-efi.sh --default=compute      装完并把默认启动项设为 compute
#   sh install-efi.sh --timeout=5            装完并把菜单停留时间设为 5 秒
#   sh install-efi.sh --build --force --default=compute --timeout=5   一条命令全自动
#
#   --default 接受: inspect | compute | full, 或 grub.cfg 里的完整标题
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

# 三个菜单标题(唯一来源: 生成菜单和 --default 校验都用这里)
T_INSPECT="PVE + CMP40HX UNLOCK (inspect, 只读诊断: 不写寄存器, 无 SBR, 零风险)"
T_COMPUTE="PVE + CMP40HX UNLOCK (compute, 最小解锁: 全套寄存器 + 1 次 SBR + Gen2 x16)"
T_FULL="PVE + CMP40HX UNLOCK (full, 对齐 Windows 完整解锁, 3 次 SBR + Gen2 x16)"

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
                        可写 inspect / compute / full, 或完整标题(逐字节匹配)
      --timeout=<秒>    安装后设置菜单停留时间(非负整数)
  -h, --help            显示本帮助

带任一选项即为全自动模式: 不询问、不进入可选菜单, 装完直接退出。
只有给了 --build 才会编译; 不给 --build(或给 --prebuilt)一律用现成的 .efi。
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

# 把 inspect/compute/full 缩写展开成完整标题
expand_title() {
    case $1 in
        inspect) echo "$T_INSPECT" ;;
        compute) echo "$T_COMPUTE" ;;
        full)    echo "$T_FULL" ;;
        *)       echo "$1" ;;
    esac
}

CUR_TIMEOUT() {
    if [ -f "$GRUB_DEFAULT_FILE" ]; then
        grep '^GRUB_TIMEOUT=' "$GRUB_DEFAULT_FILE" 2>/dev/null || echo "GRUB_TIMEOUT 未设置 (GRUB 默认)"
    else
        echo "找不到 $GRUB_DEFAULT_FILE"
    fi
}

# ---------- 参数解析 ----------
AUTO=0
OPT_FORCE=0
OPT_BUILD=0
OPT_DEFAULT=
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
# 三个产物必须齐全。上游原版对缺失文件是静默跳过, 结果 ESP 里留着上一次的旧
# .efi, 脚本照样报"安装完成"——极容易被误判成"新代码无效"。
for f in NVPermissiveEFI.efi NVPermissiveEFI-COMPUTE.efi NVPermissiveEFI-FULL.efi; do
    [ -f "$DIR/$f" ] || {
        echo "错误: 缺少 $DIR/$f"
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
for f in "$ESP/EFI/UNLOCK/NVPermissiveEFI.efi" \
         "$ESP/EFI/UNLOCK/NVPermissiveEFI-COMPUTE.efi" \
         "$ESP/EFI/UNLOCK/NVPermissiveEFI-FULL.efi"; do
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

# ---------- 1) ESP ----------
mkdir -p "$ESP/EFI/UNLOCK"
for f in NVPermissiveEFI.efi NVPermissiveEFI-COMPUTE.efi NVPermissiveEFI-FULL.efi; do
    install -m 0755 "$DIR/$f" "$ESP/EFI/UNLOCK/$f"
    # 覆盖后逐字节比对, 确认 ESP 里就是这一份而不是上一次的残留
    if command -v cmp >/dev/null 2>&1; then
        cmp -s "$DIR/$f" "$ESP/EFI/UNLOCK/$f" || {
            echo "错误: $ESP/EFI/UNLOCK/$f 与 $DIR/$f 不一致 (ESP 写入失败或只读)"
            exit 1
        }
    fi
done
echo "[1/3] ESP 安装完成: $ESP/EFI/UNLOCK/"
if [ "$MODE_BUILD" -eq 1 ]; then
    echo "      来源: 本次 make release 的编译产物"
else
    echo "      来源: 仓库内现成的 .efi (未编译)"
fi
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$ESP/EFI/UNLOCK" && sha256sum NVPermissiveEFI.efi NVPermissiveEFI-COMPUTE.efi NVPermissiveEFI-FULL.efi )
fi

# ---------- 2) GRUB menuentry ----------
PVE_BLOCK=$(awk '/^menuentry.*Proxmox/{f=1} f{print} f&&/^\}/{exit}' "$GC")
[ -n "$PVE_BLOCK" ] || { echo "错误: 在 $GC 中找不到 Proxmox menuentry, 无法生成引导项"; exit 1; }
# PVE 原条目去掉第一行 menuentry 头, 剩余部分(含结尾 })作为新条目主体
PVE_BODY=$(printf '%s\n' "$PVE_BLOCK" | sed '1d')

MENU_COUNT=0
{
    echo '#!/bin/sh'
    echo '# Auto-generated by install-efi.sh — NVPermissiveEFI CMP40HX UEFI unlock'
    echo "cat <<'EOF'"
    for M in INSPECT COMPUTE FULL; do
        case $M in
            INSPECT) MFI=NVPermissiveEFI;          ARG=inspect; TITLE=$T_INSPECT ;;
            COMPUTE) MFI=NVPermissiveEFI-COMPUTE;  ARG=compute; TITLE=$T_COMPUTE ;;
            FULL)    MFI=NVPermissiveEFI-FULL;     ARG=full;    TITLE=$T_FULL ;;
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
echo "[2/3] 已生成 $F"

# ---------- 3) update-grub ----------
if command -v update-grub >/dev/null 2>&1; then
    if update-grub > /tmp/update-grub.cmp40hx.log 2>&1; then
        # 只数 menuentry 行。不能数全文件: 设了 GRUB_DEFAULT 时 grub.cfg 里会
        # 多一行 set default="PVE + CMP40HX UNLOCK (...)", 会数错。
        N=$(grep -c '^menuentry "PVE + CMP40HX UNLOCK' "$GC" || true)
        echo "[3/3] update-grub 完成, $GC 中含 $N 个新菜单项 (应为 $MENU_COUNT)"
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
    echo "[3/3] 未找到 update-grub 命令 (测试环境?), 跳过重新生成 grub.cfg"
fi

echo
echo "安装完成。"

# ---------- 4) 全自动模式: 按参数一次做完 ----------
if [ "$AUTO" -eq 1 ]; then
    if [ -n "$OPT_DEFAULT" ]; then
        TITLE=$(expand_title "$OPT_DEFAULT")
        if printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$TITLE"; then
            sed -i '/^GRUB_DEFAULT=/d' "$GRUB_DEFAULT_FILE"
            printf "GRUB_DEFAULT='%s'\n" "$TITLE" >> "$GRUB_DEFAULT_FILE"
            echo "已设置默认启动项: $TITLE"
        else
            echo "错误: '$TITLE' 不在 $GC 里, 默认启动项未设置。"
            echo "      用 --default=inspect|compute|full 最稳妥; 也可先 grep '^menuentry' $GC 看实际标题"
            exit 1
        fi
    fi
    if [ -n "$OPT_TIMEOUT" ]; then
        sed -i '/^GRUB_TIMEOUT=/d' "$GRUB_DEFAULT_FILE"
        printf "GRUB_TIMEOUT=%s\n" "$OPT_TIMEOUT" >> "$GRUB_DEFAULT_FILE"
        echo "已设置菜单停留时间: ${OPT_TIMEOUT} 秒"
    fi
    if [ -n "$OPT_DEFAULT" ] || [ -n "$OPT_TIMEOUT" ]; then
        regen || exit 1
    fi
    echo "全部完成, 重启后生效。"
    exit 0
fi

# ---------- 5) 交互模式: 可选设置 ----------
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
            echo "输入要设为默认的启动项完整标题 (可先选 1 查看, 也可直接输 inspect/compute/full):"
            printf "标题: "
            if IFS= read -r TITLE; then :; else TITLE=; fi
            [ -n "$TITLE" ] || { echo "错误: 标题不能为空, 未做改动。"; continue; }
            TITLE=$(expand_title "$TITLE")
            if printf '%s\n' "$(grub_menu_names)" | grep -qxF -- "$TITLE"; then
                sed -i '/^GRUB_DEFAULT=/d' "$GRUB_DEFAULT_FILE"
                printf "GRUB_DEFAULT='%s'\n" "$TITLE" >> "$GRUB_DEFAULT_FILE"
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
