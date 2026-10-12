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
イメージを書くと、戻すには u-boot コンソールの解錠（所有者のパスワード）と `tftpboot` →
`nand erase a80000 900000` → `nand write 83000000 a80000 680000` が要る。だから、書く前に
RAM の上で確かめ、書いた後は読み戻して確かめる。

1. **イメージを `/tmp` へ**（`tftp -g` か `nc`）、sha256 を照合する。巻き戻し用に、いま入っているイメージも
   `/tmp` に置く（`dd if=/dev/mtdblock3` の先頭が手元の旧イメージと一致することを先に確かめる）。
2. **事前検証（フラッシュに触れない）**: `mount -t squashfs -o ro,loop /tmp/<新>.img /tmp/nr`。
   稼働中の `/rom` とファイル一覧・md5 を比べ、差が意図どおりかを見る。新しい BusyBox
   （`/tmp/nr/bin/busybox`）で起動スクリプトを `sh -n` に通し、`board.conf` を読ませる。
3. **読み出しの予行**: `mtd verify /tmp/<旧>.img /dev/mtd3` が `Success` になること。
4. **`/rom` から読まない状態にする**: 書き始めると、まだ読んでいない `/rom` のブロックは
   新しい中身に変わる。`mtd` を `/tmp` に写し、シェルを tmpfs の BusyBox に替える
   （`mkdir /tmp/bb; for a in $(/tmp/nr/bin/busybox --list); do ln -s /tmp/nr/bin/busybox /tmp/bb/$a; done;
   export PATH=/tmp/bb; exec /tmp/bb/sh`）。dropbear と udhcpc を止める。
5. **書く**: `/tmp/mtd write /tmp/<新>.img /dev/mtd3` → `/tmp/mtd verify /tmp/<新>.img /dev/mtd3` が
   `Success` → `sync` → **`/tmp/bb/reboot -f`**（init を通さずに再起動する。古いカーネルでは
   効かないので電源を入れ直す。下記）。verify が失敗したら、動いているうちに書き直す（だめなら
   旧イメージを書き戻す）。

`mtd` は消去ブロックごとに「消去 → 書き込み」をし、不良ブロックは飛ばして先へ書く。カーネルは
mtdblock3 を詰めずに読むので、飛ばしが起きると verify が合わなくなる（＝そこで気づける）。
overlay（mtd9 の `openird/overlay`）は触らないので、設定とアドオンは残る。

カーネル（mtd2）も同じ手順で書ける（2026-10-12 に 2 回）。**rootfs → カーネルの順に書き、どちらも
verify が通ってから再起動する。** 巻き戻し用に、いまの中身を `/tmp` に取っておく
（`dd if=/dev/mtd2 bs=4096 count=512 | head -c <長さ>`）。カーネルは単一面で、起動しない
イメージを書くと復旧は上と同じ（u-boot の解錠）になる。uImage が `0x280000` を超えないことは
`openird-mkimage` が見る。

## 再起動と LED

`ird-board-emma3sep` を組み込んだカーネル（`v26.10-rc2` より後）では、`reboot` が効き、前面の
LED を `/sys/class/leds/ird:<色>:<power|status>` から点けられる（`ird-led`）。配線の出どころと
実機での確認は [docs/specs/emma3sep-airtuner-gpio.md](../../../docs/specs/emma3sep-airtuner-gpio.md)。

**それより前のカーネル（`v26.10-rc2` まで）は、電源の入れ直しでしか再起動できない**
（2026-10-12・POE-BR100-YB1 で観察）:

- `reboot -f` は再起動にならない。カーネルは `EMMA3Sx restart.` と出してリセットベクタ
  `0xbfc00000` へ飛び、そこで例外（Reserved instruction）になって `reboot` のプロセスだけが
  落ちる。系は動いたまま残る。
- そのあと `/dev/watchdog` を開いたまま放置したところ、リセットにはならず、数分のうちに
  シリアルもネットワークも無応答になった（U-Boot の出力も出ない）。ドライバはウォッチドッグを
  NMI を起こす設定にしていて、NMI の飛び先は同じベクタなので、そこで止まったとみられる。
  このドライバは変えていない＝新しいカーネルでも、ウォッチドッグは再起動の手段にならない。
- 手で再起動させるなら、自己リセット線を直接下げる:
  `devmem 0x10110d18 32 0xBFFFFFFF; devmem 0x10110d00 32 0`。
