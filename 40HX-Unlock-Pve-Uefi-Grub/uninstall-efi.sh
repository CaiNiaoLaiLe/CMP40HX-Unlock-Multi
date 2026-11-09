#!/bin/sh
# uninstall-efi.sh — 完全移除 NVPermissiveEFI (回到工厂状态, 下次重启生效)
#
# 除了删引导产物, 还对称回收 install-efi.sh 写入的三处 Linux 侧配置
# (同样幂等: 有就删, 没有就跳过):
#   1) /etc/modprobe.d/nvidia-rebar.conf  (options nvidia NVreg_EnableResizableBar=1)
#   2) /etc/default/grub 的 GRUB_CMDLINE_LINUX_DEFAULT 里的 pci=realloc
#   3) /etc/default/grub 的 GRUB_DEFAULT(仅当它指向本方案条目时清除)
#      并把菜单倒计时恢复为出厂值 GRUB_TIMEOUT=5
# 注意: pci=realloc 若在你安装本方案之前就已自己配置(有独立用途), 这里同样会被
# 摘掉; 需要保留的话卸载后手动加回并重跑 update-grub。
# GRUB_DEFAULT 只要不指向本方案就不会被动(你原本设的其它默认项保留)。
set -e
ESP=${ESP:-/boot/efi}
F=${F:-/etc/grub.d/40-cmp40hx}
GRUB_DEFAULT_FILE=${GRUB_DEFAULT_FILE:-/etc/default/grub}
MODPROBE_CONF=${MODPROBE_CONF:-/etc/modprobe.d/nvidia-rebar.conf}

rm -f "$F"
rm -rf "$ESP/EFI/UNLOCK"

# 1) modprobe 配置: 仅当文件内容就是安装脚本写的那行(及其注释/空行)时才删,
#    防止误删用户自管的同名文件
if [ -f "$MODPROBE_CONF" ]; then
    REST=$(sed '/^#/d;/^$/d;/^options nvidia NVreg_EnableResizableBar=1$/d' "$MODPROBE_CONF")
    if [ -z "$REST" ]; then
        rm -f "$MODPROBE_CONF"
        echo "已移除: $MODPROBE_CONF"
        if command -v update-initramfs >/dev/null 2>&1; then
            echo "正在更新 initramfs (update-initramfs -u), 约需十几秒, 请稍候..."
            update-initramfs -u >/dev/null || echo "警告: update-initramfs 失败, 请手动执行: update-initramfs -u"
        fi
    else
        echo "警告: $MODPROBE_CONF 含本方案之外的内容, 保留未删, 多余行如下:"
        printf '%s\n' "$REST"
    fi
fi

# 2) pci=realloc: 从 GRUB_CMDLINE_LINUX_DEFAULT 行里摘掉(词首/词中/独立成值三种形态)
if [ -f "$GRUB_DEFAULT_FILE" ] && grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=.*pci=realloc' "$GRUB_DEFAULT_FILE"; then
    TMP="${GRUB_DEFAULT_FILE}.cmp40hx.tmp"
    awk '/^GRUB_CMDLINE_LINUX_DEFAULT="/{
            if ($0 ~ / pci=realloc/)       sub(/ pci=realloc/, "");
            else if ($0 ~ /pci=realloc /)  sub(/pci=realloc /, "");
            else if ($0 ~ /"pci=realloc"/) sub(/"pci=realloc"/, "\"\"");
         }
         { print }' "$GRUB_DEFAULT_FILE" > "$TMP" &&
        mv "$TMP" "$GRUB_DEFAULT_FILE" || {
        rm -f "$TMP"; echo "错误: 改写 $GRUB_DEFAULT_FILE 失败, pci=realloc 未移除"; exit 1; }
    echo "已从 $GRUB_DEFAULT_FILE 的 GRUB_CMDLINE_LINUX_DEFAULT 移除 pci=realloc"
fi

# 3) GRUB_DEFAULT: 只清除指向本方案条目的默认项(单引号/双引号/无引号三种写法都认),
#    指向其它条目的保持不动; 随后把菜单倒计时恢复为 PVE 出厂值 5 秒
if [ -f "$GRUB_DEFAULT_FILE" ]; then
    # 标题里的 + 在 ERE 里是量词, 用字符类 [+] 匹配字面加号
    if grep -qE "^GRUB_DEFAULT=['\"]?PVE [+] CMP40HX UNLOCK" "$GRUB_DEFAULT_FILE" 2>/dev/null; then
        sed -i \
            -e "/^GRUB_DEFAULT='PVE + CMP40HX UNLOCK/d" \
            -e '/^GRUB_DEFAULT="PVE + CMP40HX UNLOCK/d' \
            -e '/^GRUB_DEFAULT=PVE + CMP40HX UNLOCK/d' \
            "$GRUB_DEFAULT_FILE"
        echo "已清除指向本方案的默认启动项 (GRUB 恢复为取菜单第一条 = 普通 PVE)"
    else
        echo "默认启动项未指向本方案, 保持不动"
    fi
    sed -i '/^GRUB_TIMEOUT=/d' "$GRUB_DEFAULT_FILE"
    printf 'GRUB_TIMEOUT=5\n' >> "$GRUB_DEFAULT_FILE"
    echo "已恢复菜单倒计时: GRUB_TIMEOUT=5 (PVE 出厂值)"
    if grep -q '^GRUB_TIMEOUT_STYLE=hidden' "$GRUB_DEFAULT_FILE" 2>/dev/null; then
        echo "注意: 文件里有 GRUB_TIMEOUT_STYLE=hidden, 菜单仍会被跳过、5 秒不生效。"
        echo "      要显示菜单: sed -i 's/^GRUB_TIMEOUT_STYLE=hidden/GRUB_TIMEOUT_STYLE=menu/' $GRUB_DEFAULT_FILE && update-grub"
    fi
fi

if command -v update-grub >/dev/null 2>&1; then
    echo "正在重新生成 grub.cfg (update-grub), 请稍候..."
    update-grub
fi
echo "已移除: $F, $ESP/EFI/UNLOCK, 并已重新生成 grub.cfg"
echo "下次重启即恢复工厂状态 (寄存器由 GSP 每次启动重新初始化; 三处配置改动已一并回收)。"
