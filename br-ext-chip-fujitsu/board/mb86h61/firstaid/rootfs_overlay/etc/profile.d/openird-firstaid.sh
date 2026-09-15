# OpenIRD first-aid: ログインシェル起動時の案内
[ -r /etc/openird_release ] && . /etc/openird_release
cat <<EOF

 OpenIRD first-aid rootfs ${OPENIRD_BUILD:+($OPENIRD_BUILD)}
   区画一覧   cat /proc/mtd
   書き込み   mtd write <file> <区画>     （<区画> は /dev/mtdN か /proc/mtd の名前）
   検証       mtd verify <file> <区画>
   全消去     mtd erase <区画>
   ネット     ifconfig eth0 <ip> netmask <mask> up   または   udhcpc -i eth0
   転送       tftp -g -r <file> -l /tmp/<file> <server>

EOF
