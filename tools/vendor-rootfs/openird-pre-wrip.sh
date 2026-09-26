# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
#
# SB-TV06-WRIP (POE-BR310L-SB1) 版の段階 C リグフック。
# デバイス上の /app/setting/openird-pre.sh として置く。mkshell.sh --observe で
# 作った像が /etc/init.d/rcS から setup.sh の直前に source する。/app/setting は
# yaffs2 rw で再起動を跨ぐので、実験の入切は焼き直し無しにフラグで切り替えられる。
#
# ★ この機種はカーネル console が無く（cmdline に console= 無し）、シリアルに
#   Linux は出ない。∴ noblur（ベンダアプリを起動しない）にすると telnet が唯一の口。
#   ネットワークは通常 lindberg の udhcpc が上げるので、noblur では**このフックが
#   最初に静的 IP を振る**（それが無いと telnet に入れず、シリアル救済も無い）。
#   復旧の最後の手段は fwup で純正/telnet 版コンテナを焼き直すこと。
#
# フラグ（/app/setting に置く・中身も見る）:
#   openird-noblur-once  ★安全版★ 1 回だけ noblur。読み取り時に自分で消すので、
#                     次の電源投入は通常起動＝lindberg も UPnP も戻る＝fwup で救える。
#                     **初回はこれを使う。** noblur が telnet を壊しても電源入れ直しで復帰。
#   openird-noblur    恒久 noblur（毎回）。動作確認が済んでから使う。**警告**: noblur は
#                     lindberg を起動しない＝UPnP(:49152) も上がらない＝fwup で救えない。
#                     静的 IP の telnet が唯一の口。壊すと電源リセットでも戻らない恐れ。
#   openird-ip        静的 IP（既定 192.168.11.20）。noblur のとき eth0 に振る。
#   openird-csddebug  csd.ko を CSD_DEBUG=<中身>(既定1) で自前 load。
#
# フラグを消せばその機能は無効。結果は /app/setting/openird-pre.log。

{
  echo "--- openird-pre (wrip) ---"
  cat /proc/uptime

  # 1 回だけ noblur: 読んだ時点で消す＝次の起動は通常＝救済可能。初回はこれを使う。
  NOBLUR=0
  if [ -f /app/setting/openird-noblur-once ] ; then
    rm -f /app/setting/openird-noblur-once
    NOBLUR=1
    echo "noblur ONE-SHOT (flag consumed; next boot is normal)"
  elif [ -f /app/setting/openird-noblur ] ; then
    NOBLUR=1
    echo "noblur PERSISTENT"
  fi

  # --- 1. まず telnet の生命線＝eth0 の静的 IP（noblur のときだけ） ---------
  # 何より先に振る。以降の insmod や bind mount が失敗しても telnet は残す。
  if [ "$NOBLUR" = 1 ] ; then
    IP=$(cat /app/setting/openird-ip 2>/dev/null)
    [ -n "$IP" ] || IP=192.168.11.20
    ifconfig eth0 "$IP" netmask 255.255.255.0 up && echo "eth0 = $IP (static)"
    # 保険: DHCP でも取れるようにしておく（静的が衝突しても届く口を残す）
    udhcpc -b -i eth0 -s /app/bin/udhcpc_default.sh >/dev/null 2>&1 &
  fi

  # --- 2. 裏 CPU FW と csd（noblur か csddebug のとき） --------------------
  if [ -f /app/setting/openird-csddebug ] || [ "$NOBLUR" = 1 ] ; then
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

  # --- 3. noblur: lindberg が load するモジュールを代わりに入れ、lindberg を潰す
  if [ "$NOBLUR" = 1 ] ; then
    echo "--- noblur: loading what lindberg would load ---"
    # 依存順（Used-by から）。insmod は既に入っていれば "File exists" で無害に失敗。
    for m in mem2 i2c-algo-emma i2c-emma io_controller_drv ei2c_ofdm_drv \
             fe_common_drv fe_main_drv demod_tc90522_90532_drv \
             tuner_MxLinear_MxL601RF_drv tuner_MxLinear_MxL135RF_drv \
             tuner_PHILIPS_TDA8263HN_drv val_merge ; do
      insmod /app/bin/drivers/$m.ko 2>/dev/null && echo "  $m" || echo "  $m (skip/exists)"
    done
    # setup.sh は app squashfs の普通のファイル＝bind mount で差し替えられる。
    # ベンダアプリ行だけ潰す（他＝EMMA fw/h57fmb/dione/sysctl/cmd_executor は残す）。
    sed 's|^\./bin/lindberg|#&|' /app/bin/setup.sh > /tmp/setup-noblur.sh
    chmod 755 /tmp/setup-noblur.sh
    if mount -o bind /tmp/setup-noblur.sh /app/bin/setup.sh ; then
      echo "setup.sh replaced (lindberg will not start)"
    else
      echo "bind mount FAILED (lindberg WILL start)"
    fi
  fi
  echo "--- end ---"
} > /app/setting/openird-pre.log 2>&1
