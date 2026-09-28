# --- OpenIRD observation base (inserted) ------------------------------------
# The only change to the vendor rcS.  Everything else in this rootfs, and in
# the kernel and the slots next to it, is the vendor's own bytes.
#
# 1) Root shell on tcp/23 without a password.  The vendor's development
#    package starts the same thing from slot_entry.sh when /slot/app/core.sh
#    exists.  Started after the two exports above, so a telnet session gets
#    the LD_LIBRARY_PATH and PATH the vendor application runs with.
/usr/sbin/telnetd -l /bin/sh
# 2) A fixed address for telnet that does not depend on the USB disk.  The
#    vendor application builds br0 and bridges eth0 into it; its own address
#    comes from DHCP and changes.  Once br0 holds eth0, add 192.168.200.10/24
#    as br0:9 (for a PC on the same wire at 192.168.200.x) and put it back if
#    it disappears.  Never on eth0 itself: an address left on a bridge port
#    kills the LAN.  Without the application (noapp) rc.sh sets eth0 instead.
( while : ; do
	if [ -e /sys/class/net/br0/brif/eth0 ] ; then
		ifconfig br0:9 2>/dev/null | grep -q "addr:192.168.200.10 " \
			|| ifconfig br0:9 192.168.200.10 netmask 255.255.255.0 up
	fi
	sleep 5
  done ) < /dev/null > /dev/null 2>&1 &
# 3) USB disk hook.  If a USB disk sits directly in the rear USB-A port,
#    mount it read-only on /tmp/usb and source openird/rc.sh from it, so
#    experiments change without rewriting this rootfs.  Without a disk the
#    only difference is that the USB storage stack is loaded a moment before
#    slot_entry.sh would load it (same modules, same order, and its own
#    modprobe calls then do nothing).  usb-storage waits 1 s instead of 5 s
#    before scanning a new disk.  A disk is waited for up to 60 s: a USB SSD
#    took 25-30 s to show up after a cold power-on (2026-09-29), longer than
#    the vendor's own 30 x 1 s mount loop that follows.
#    Read-only on purpose: slot_entry.sh later mounts the same disk read-write
#    on /mnt to look for updater.full and dionekeyEraser.full, and that mount
#    fails (EBUSY) while ours is in place, so neither path can run.
#    Plug the disk in directly.  The vendor's check reused below only looks
#    at port 3 of the on-board hub, and the SoC's USB host has too few
#    endpoints for another hub next to Dione.
#    rc.sh is run from a copy with CR stripped (edited on Windows is fine)
#    and only if it passes a syntax check.  It runs after telnetd, so a hook
#    that fails or hangs still leaves a way in (or pull the disk and reboot).
modprobe scsi_mod
modprobe sd_mod
modprobe usbcore
modprobe usb-storage delay_use=1
modprobe musb_hdrc use_dma=0
if update check_usb_flash > /dev/null ; then
	n=0 ; m=0
	while [ ! -e /sys/block/sda/sda1 ] && [ $n -lt 600 ] && [ $m -lt 20 ] ; do
		usleep 100000
		n=$((n + 1))
		[ -e /sys/block/sda ] && m=$((m + 1))
	done
	mkdir -p /tmp/usb
	if mount -t vfat -o ro /dev/sda1 /tmp/usb || mount -t vfat -o ro /dev/sda /tmp/usb ; then
		echo "OpenIRD: USB disk mounted read-only on /tmp/usb"
		if [ -f /tmp/usb/openird/rc.sh ] ; then
			tr -d '\r' < /tmp/usb/openird/rc.sh > /tmp/openird-rc.sh
			if sh -n /tmp/openird-rc.sh ; then
				. /tmp/openird-rc.sh
			else
				echo "OpenIRD: openird/rc.sh has a syntax error, not run"
			fi
		fi
	fi
fi
# --- end OpenIRD --------------------------------------------------------------
