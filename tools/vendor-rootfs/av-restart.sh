# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# POE-BR100-YB1 stage-C rig: start the AV subsystem over WITHOUT a power cycle.
# ASCII only: this runs on the device.  Put it in /app/setting and run it with
# sh after a stage-C trial, instead of pulling the plug.
#
# The AV subsystem keeps its state after the process exits, so a second run of
# a val_demux column used to need a power cycle (a second nr=0x01 returns -1).
# Reloading csd in its NORMAL mode makes it run the back-CPU init again
# ("Execute: USR_INIT_CODE" -> "4KE POWER-ON"), after which nr=0x01 returns 0,
# the handles are the fresh-boot ones, the PID table is rebuilt and PES flows.
# Spec: docs/specs/poe-br100-yb1-subcpu-control.md section 6.
#
# Not needed and not done here:
#   - CSD_MODE=FORCE only registers the interrupts; it does not re-run the init.
#   - rmmod csd alone does not stop the back CPU.
#
# Needs no vendor application.  The MXL683 front end is not touched, so the
# tuning survives; only the demultiplexer side has to be set up again.

set -e
D=$(cat /app/setting/openird-csddebug 2>/dev/null || true)
[ -n "$D" ] || D=0

# The CSD_SPECIAL line leaves /proc/interrupts with csd, but its count is kept
# per IRQ, so read it while csd is still in.  The back CPU answering the init
# bumps it by one.
before=$(grep CSD_SPECIAL /proc/interrupts | awk '{print $2}')
[ -n "$before" ] || before=none

if lsmod | grep -q '^val_merge ' ; then rmmod val_merge ; fi
if lsmod | grep -q '^csd ' ; then rmmod csd ; fi

mkdir -p /tmp/fw5
mount -t squashfs -o ro /dev/mtdblock5 /tmp/fw5
dd if=/tmp/fw5/emma3sep_driver_std_op0.mem of=/dev/mem bs=1024 seek=192 2>/dev/null
umount /tmp/fw5
echo "back-CPU firmware written to phys 0x00030000"

insmod /app/bin/drivers/csd.ko CSD_DEBUG=$D
n=0
while [ $n -lt 20 ] ; do
  now=$(grep CSD_SPECIAL /proc/interrupts | awk '{print $2}')
  [ "$now" != "$before" ] && break
  usleep 100000
  n=$((n+1))
done
if [ "$now" != "$before" ] ; then
  echo "back CPU answered (CSD_SPECIAL $before -> $now)"
else
  echo "back CPU did NOT answer within 2 s (power-cycle instead)"
  exit 1
fi
insmod /app/bin/drivers/val_merge.ko
echo "AV subsystem restarted; run the val_demux column again"
