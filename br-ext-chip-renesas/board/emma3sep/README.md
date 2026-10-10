# board/emma3sep — Renesas EMMA3SEP

MIPS32r2 **ビッグエンディアン** / linux-2.6.27.39（GPL から再ビルド・非 DT）。
- `kernel/linux.config` … カーネル設定（GPL .config を出発点に）。
- `kernel/patches/` … 自前パッチのみ（pristine tarball は dl/、`*.hash` で検証）。
- uImage: Load/Entry `0x82280000` / name `emma3se`（CLAUDE.md §7）。
- u-boot は純正温存（UPnP 経路では触らない）。

対象機: POE-BR100-YB1（BMMPX1301）, POE-BR310L-SB1（SB-TV06-WRIP, 板サポート GPL 未収録）。

## rootfs（mtd3）の更新 — OpenIRD が動いている実機

OpenIRD が動いている間は、純正の更新経路（UPnP → u-boot の env）が使えない。rootfs は、
動いている系から OpenWrt の `mtd` で書ける（2026-10-11・POE-BR100-YB1 で実施）。

**mtd3 は単一面で、救助 initramfs はいまカーネルに積んでいない。** 書き損じるか、起動しない
像を書くと、戻すには u-boot コンソールの解錠（所有者のパスワード）と `tftpboot` →
`nand erase a80000 900000` → `nand write 83000000 a80000 680000` が要る。だから、書く前に
RAM の上で確かめ、書いた後は読み戻して確かめる。

1. **像を `/tmp` へ**（`tftp -g` か `nc`）、sha256 を照合する。巻き戻し用に、いま入っている像も
   `/tmp` に置く（`dd if=/dev/mtdblock3` の先頭が手元の旧像と一致することを先に確かめる）。
2. **事前検証（フラッシュに触れない）**: `mount -t squashfs -o ro,loop /tmp/<新>.img /tmp/nr`。
   稼働中の `/rom` とファイル一覧・md5 を比べ、差が意図どおりかを見る。新しい BusyBox
   （`/tmp/nr/bin/busybox`）で起動スクリプトを `sh -n` に通し、`board.conf` を読ませる。
3. **読み出しの予行**: `mtd verify /tmp/<旧>.img /dev/mtd3` が `Success` になること。
4. **`/rom` から読まない状態にする**: 書き始めると、まだ読んでいない `/rom` のブロックは
   新しい中身に変わる。`mtd` を `/tmp` に写し、シェルを tmpfs の BusyBox に替える
   （`mkdir /tmp/bb; for a in $(/tmp/nr/bin/busybox --list); do ln -s /tmp/nr/bin/busybox /tmp/bb/$a; done;
   export PATH=/tmp/bb; exec /tmp/bb/sh`）。dropbear と udhcpc を止める。
5. **書く**: `/tmp/mtd write /tmp/<新>.img /dev/mtd3` → `/tmp/mtd verify /tmp/<新>.img /dev/mtd3` が
   `Success` → `sync` → **電源を入れ直す**（`reboot` は使わない）。verify が失敗したら、動いて
   いるうちに書き直す（だめなら旧像を書き戻す）。

`mtd` は消去ブロックごとに「消去 → 書き込み」をし、不良ブロックは飛ばして先へ書く。カーネルは
mtdblock3 を詰めずに読むので、飛ばしが起きると verify が合わなくなる（＝そこで気づける）。
overlay（mtd9 の `openird/overlay`）は触らないので、設定とアドオンは残る。
