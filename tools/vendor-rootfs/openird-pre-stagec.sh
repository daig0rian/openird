# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# Stage-C rig hook as it runs on the POE-BR100-YB1: drop it on the device as
# /app/setting/openird-pre.sh (the rig image sources it from rcS just before
# setup.sh).  Kept here so the rig is reproducible; see
# docs/poe-br100-yb1-rig.md and docs/specs/poe-br100-yb1-reception-stack.md
# sections 79-82.
#
# OpenIRD hook: load csd with CSD_DEBUG and drain the kernel log, so the
# back-CPU command footprints can be compared between a vendor boot and a
# no-vendor boot.  ASCII only: this runs on the device.
#
#   openird-csddebug   contents become CSD_DEBUG (a bit set, not a level)
#   openird-noblur     also load what the vendor application would load, give
#                      eth0 a static address, and keep the application off
#   openird-nocsd-once one boot only (the flag is removed when read): do not
#                      load csd or val_merge, so the back CPU is left exactly
#                      as U-Boot and setup.sh leave it.  For observing what
#                      csd's init does to the BHIF CPU control block.
#   openird-min-once   one boot only (removed when read), with openird-noblur:
#                      load nothing of the vendor's but csd and the GPL i2c
#                      modules, and replace setup.sh with a no-op (no Dione,
#                      no Wi-Fi, no cmd_executor_server).  For checking what
#                      the receive path needs besides csd.
#
# The kernel log ring is 16 KB, so it is drained into tmpfs faster than the
# command stack fills it.  Result: /tmp/csd.log
# Hook log: /app/setting/openird-pre.log

{
  echo "--- openird-pre (csd command trace) ---"
  cat /proc/uptime

  NOCSD=0
  if [ -f /app/setting/openird-nocsd-once ] ; then
    rm -f /app/setting/openird-nocsd-once
    NOCSD=1
    echo "nocsd ONE-SHOT (flag consumed; next boot loads csd again)"
  fi
  MIN=0
  if [ -f /app/setting/openird-min-once ] ; then
    rm -f /app/setting/openird-min-once
    MIN=1
    echo "min ONE-SHOT (flag consumed): csd and GPL i2c only, setup.sh no-op"
  fi

  if [ -f /app/setting/openird-csddebug ] || [ -f /app/setting/openird-noblur ] ; then
    mkdir -p /tmp/fw5
    if mount -t squashfs -o ro /dev/mtdblock5 /tmp/fw5 ; then
      dd if=/tmp/fw5/emma3sep_driver_std_op0.mem of=/dev/mem bs=1024 seek=192
      echo "back-CPU firmware written to phys 0x00030000"
      umount /tmp/fw5
    else
      echo "mtd5 mount FAILED"
    fi
    D=$(cat /app/setting/openird-csddebug 2>/dev/null)
    [ -n "$D" ] || D=1
    echo "CSD_DEBUG=$D"
    if [ "$NOCSD" = 1 ] ; then
      echo "csd NOT loaded (nocsd)"
    else
      insmod /app/bin/drivers/csd.ko CSD_DEBUG=$D && echo "csd loaded"
    fi
  fi

  # Start draining before anything else runs, so the very first commands land
  # in the file rather than falling out of the 16 KB ring.
  rm -f /tmp/csd.log
  (
    n=0
    while [ $n -lt 30000 ] ; do
      dmesg -c >> /tmp/csd.log 2>/dev/null
      usleep 20000
      n=$((n+1))
    done
    echo "=== drain loop finished ===" >> /tmp/csd.log
  ) &
  echo "drain loop started (about 10 minutes)"

  if [ -f /app/setting/openird-noblur ] ; then
    echo "--- noblur: loading what the vendor application would load ---"
    for m in i2c-algo-emma i2c-emma val_merge ei2c_ofdm_drv \
             io_controller_drv fe_common_drv fe_main_drv mem2 ; do
      if [ "$NOCSD" = 1 ] && [ "$m" = val_merge ] ; then
        echo "  $m skipped (nocsd)"
        continue
      fi
      if [ "$MIN" = 1 ] && [ "$m" != i2c-algo-emma ] && [ "$m" != i2c-emma ] ; then
        echo "  $m skipped (min)"
        continue
      fi
      insmod /app/bin/drivers/$m.ko && echo "  $m" || echo "  $m FAILED"
    done
    echo "--- network (static) ---"
    ifconfig eth0 192.168.11.217 netmask 255.255.255.0 up && echo "eth0 up"
    route add default gw 192.168.11.1 2>/dev/null
    if [ "$MIN" = 1 ] ; then
      printf '#!/bin/sh\necho "setup.sh skipped (openird-min-once)"\n' > /tmp/setup-noblur.sh
    else
      sed 's|^\./bin/blur|#&|' /app/bin/setup.sh > /tmp/setup-noblur.sh
    fi
    chmod 755 /tmp/setup-noblur.sh
    mount -o bind /tmp/setup-noblur.sh /app/bin/setup.sh \
      && echo "setup.sh replaced" || echo "bind mount FAILED"
  fi
  echo "--- end ---"
} > /app/setting/openird-pre.log 2>&1
