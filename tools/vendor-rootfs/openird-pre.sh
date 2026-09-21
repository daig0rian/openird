# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# Drop this on the device as /app/setting/openird-pre.sh.  The rig image built
# by mkshell.sh --observe sources it from /etc/init.d/rcS just before setup.sh
# (see docs/specs/poe-br100-yb1-reception-stack.md 18.5).  /app/setting is
# yaffs2 rw and survives a reboot, so experiments need no reflash.
#
# OpenIRD stage-B hook.  Sourced from /etc/init.d/rcS just before setup.sh.
# ASCII only: this runs on the device and writes to the serial console.
#
# Two independent switches, both opt-in by a flag file in /app/setting:
#
#   openird-csddebug   load csd.ko ourselves with CSD_DEBUG set to the
#                      contents of the flag file (default 1).  The vendor
#                      application loads csd.ko too; that insmod then fails
#                      with "File exists", which it ignores.
#
#   openird-noblur     run setup.sh with its last line (the vendor
#                      application) commented out, and load the modules the
#                      vendor application would have loaded.  Used to tell
#                      "this resource is already taken" apart from "we are
#                      asking for it wrongly".
#
# setup.sh is a regular file in the app squashfs, so a bind mount over it
# works.  (/sbin/insmod could not be shimmed that way: it is a symlink to
# busybox, so the mount would land on busybox itself.)
#
# Disable either switch by removing its flag file; no reflash needed.
# Result of this hook: /app/setting/openird-pre.log

{
  echo "--- openird-pre ---"
  cat /proc/uptime

  if [ -f /app/setting/openird-csddebug ] || [ -f /app/setting/openird-noblur ] ; then
    mkdir -p /tmp/fw5
    if mount -t squashfs -o ro /dev/mtdblock5 /tmp/fw5 ; then
      dd if=/tmp/fw5/emma3sep_driver_std_op0.mem of=/dev/mem bs=1024 seek=192
      echo "firmware written to phys 0x00030000"
      umount /tmp/fw5
    else
      echo "mtd5 mount FAILED"
    fi
    D=$(cat /app/setting/openird-csddebug 2>/dev/null)
    [ -n "$D" ] || D=1
    echo "CSD_DEBUG=$D"
    insmod /app/bin/drivers/csd.ko CSD_DEBUG=$D && echo "csd loaded"
  fi

  if [ -f /app/setting/openird-noblur ] ; then
    echo "--- noblur: loading what the vendor application would load ---"
    for m in i2c-algo-emma i2c-emma val_merge ei2c_ofdm_drv \
             io_controller_drv fe_common_drv fe_main_drv mem2 ; do
      insmod /app/bin/drivers/$m.ko && echo "  $m" || echo "  $m FAILED"
    done
    sed 's|^\./bin/blur|#&|' /app/bin/setup.sh > /tmp/setup-noblur.sh
    chmod 755 /tmp/setup-noblur.sh
    if mount -o bind /tmp/setup-noblur.sh /app/bin/setup.sh ; then
      echo "setup.sh replaced (vendor application will not start)"
    else
      echo "bind mount FAILED (vendor application WILL start)"
    fi
  fi
  echo "--- end ---"
} > /app/setting/openird-pre.log 2>&1
