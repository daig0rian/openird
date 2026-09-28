# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD vendor-base hook for SB-TV02-WFPL.  Copy to <USB disk>/openird/rc.sh.
#
# Sourced by /etc/init.d/rcS (tools/vendor-base/rcS-insert.sh) just before the
# vendor's slot_entry.sh, with the USB disk mounted read-only on /tmp/usb.
# ASCII only (it prints to the serial console).  Never call exit here: rcS
# would end and the vendor application would never start.  Anything that
# waits goes to the background.
#
# Switches are files next to this script (create or delete them on the PC,
# or on the device between openird-rw and openird-ro):
#   noapp     do not start the vendor application (no insmod h57fmb.ko, no
#             dione_server).  eth0 gets the fixed address and the serial
#             console becomes a root shell.
#   trace     run dione_server under the FAPex ioctl tracer
#             (bin/fapex_trace.so, log in /tmp/fapex-trace.log)
#   loglevel  console log level for dmesg -n (the file holds a digit)
#
# The fixed address is for a PC on the same wire (e.g. 192.168.200.100/24).
# With the vendor application running, rcS puts it on br0 (never on eth0: the
# application bridges eth0 into br0, and an address left on eth0 kills the
# LAN).  Without the application (noapp) this script puts it on eth0.
#
# Files in bin/ are copied to /tmp/openird/bin (tmpfs), so they keep working
# with the disk unmounted.  To keep results across a power cycle:
#   /tmp/openird/bin/openird-save FILE...   -> <USB disk>/openird/log/

OPENIRD_USB=/tmp/usb/openird
OPENIRD_RUN=/tmp/openird
OPENIRD_IP=192.168.200.10
OPENIRD_MASK=255.255.255.0

mkdir -p $OPENIRD_RUN/bin $OPENIRD_RUN/wrap
echo "--- rc.sh, uptime $(cat /proc/uptime) ---" >> $OPENIRD_RUN/rc.log

if [ -d $OPENIRD_USB/bin ] ; then
	cp $OPENIRD_USB/bin/* $OPENIRD_RUN/bin/ >> $OPENIRD_RUN/rc.log 2>&1
fi

cat > $OPENIRD_RUN/bin/openird-save <<'EOF'
#!/bin/sh
# Copy files to <USB disk>/openird/log/ and put the disk back to read-only.
[ $# -gt 0 ] || { echo "usage: openird-save FILE..."; exit 1; }
mount -o remount,rw /tmp/usb || exit 1
mkdir -p /tmp/usb/openird/log
cp "$@" /tmp/usb/openird/log/
r=$?
sync
mount -o remount,ro /tmp/usb
exit $r
EOF
cat > $OPENIRD_RUN/bin/openird-rw <<'EOF'
#!/bin/sh
# Make the USB disk writable (to edit rc.sh or the switches).  Undo: openird-ro
mount -o remount,rw /tmp/usb
EOF
cat > $OPENIRD_RUN/bin/openird-ro <<'EOF'
#!/bin/sh
sync
mount -o remount,ro /tmp/usb
EOF
chmod 755 $OPENIRD_RUN/bin/*

if [ -f $OPENIRD_USB/loglevel ] ; then
	dmesg -n "$(tr -dc 0-9 < $OPENIRD_USB/loglevel)"
fi

if [ -f $OPENIRD_USB/trace ] && [ -f $OPENIRD_RUN/bin/fapex_trace.so ] ; then
	# slot_entry.sh finds dione_server through PATH, so a wrapper placed first
	# puts the tracer under the application and nothing else.
	cat > $OPENIRD_RUN/wrap/dione_server <<EOF
#!/bin/sh
LD_PRELOAD=$OPENIRD_RUN/bin/fapex_trace.so exec /slot/app/dione_server "\$@"
EOF
	chmod 755 $OPENIRD_RUN/wrap/dione_server
	export PATH=$OPENIRD_RUN/wrap:$PATH
	echo "OpenIRD: dione_server runs under the FAPex tracer (/tmp/fapex-trace.log)"
fi

if [ -f $OPENIRD_USB/noapp ] ; then
	if grep -qx 'insmod /slot/drv/h57fmb.ko' /slot/slot_entry.sh ; then
		# Everything slot_entry.sh does before the application, then return;
		# rcS then falls through to its own root shell on the console.
		sed '/^insmod \/slot\/drv\/h57fmb\.ko$/,$d' /slot/slot_entry.sh \
			> $OPENIRD_RUN/slot_entry-noapp.sh
		echo 'echo "OpenIRD: noapp, vendor application not started"' \
			>> $OPENIRD_RUN/slot_entry-noapp.sh
		chmod 755 $OPENIRD_RUN/slot_entry-noapp.sh
		mount -o bind $OPENIRD_RUN/slot_entry-noapp.sh /slot/slot_entry.sh
		ifconfig eth0 $OPENIRD_IP netmask $OPENIRD_MASK up
		echo "OpenIRD: noapp, telnet $OPENIRD_IP, serial console is a root shell"
	else
		echo "OpenIRD: noapp ignored, slot_entry.sh is not the expected one"
	fi
else
	# rcS itself puts the address on br0 once the application has built it.
	echo "OpenIRD: telnet $OPENIRD_IP once the vendor application has built br0"
fi

echo "--- rc.sh done, uptime $(cat /proc/uptime) ---" >> $OPENIRD_RUN/rc.log
