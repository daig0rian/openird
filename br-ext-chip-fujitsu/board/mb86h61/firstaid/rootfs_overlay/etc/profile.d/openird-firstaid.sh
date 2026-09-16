# OpenIRD first-aid: hints shown on login (ASCII only - serial consoles are not UTF-8 safe)
[ -r /etc/openird_release ] && . /etc/openird_release
cat <<EOF

 OpenIRD first-aid rootfs ${OPENIRD_BUILD:+($OPENIRD_BUILD)}
   partitions  cat /proc/mtd
   write       mtd write <file> <part>   (<part> is /dev/mtdN or a name from /proc/mtd)
   verify      mtd verify <file> <part>
   erase       mtd erase <part>
   network     ifconfig eth0 <ip> netmask <mask> up   or   udhcpc -i eth0
   transfer    tftp -g -r <file> -l /tmp/<file> <server>

EOF
