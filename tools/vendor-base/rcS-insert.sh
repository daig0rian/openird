# --- OpenIRD observation base (inserted) ------------------------------------
# The only change to the vendor rcS.  Everything else in this rootfs, and in
# the kernel and the slots next to it, is the vendor's own bytes.
#
# 1) Root shell on tcp/23 without a password.  The vendor's development
#    package starts the same thing from slot_entry.sh when /slot/app/core.sh
#    exists.  Started after the two exports above, so a telnet session gets
#    the LD_LIBRARY_PATH and PATH the vendor application runs with.
/usr/sbin/telnetd -l /bin/sh
# 2) USB disk hook.  If a USB disk sits directly in the rear USB-A port,
#    mount it read-only on /tmp/usb and source openird/rc.sh from it, so
#    experiments change without rewriting this rootfs.  Without a disk the
#    only difference is that the USB storage stack is loaded a moment before
#    slot_entry.sh would load it (same modules, same order, and its own
#    modprobe calls then do nothing).  usb-storage waits 1 s instead of 5 s
#    before scanning a new disk.
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
	n=0
	while [ ! -e /sys/block/sda/sda1 ] && [ $n -lt 50 ] ; do
		usleep 100000
		n=$((n + 1))
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
