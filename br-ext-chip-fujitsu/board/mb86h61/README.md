# board/mb86h61 — Fujitsu MB86H61 (MB86H610H)

ARM1176（ARMv6, VFPv2）**リトルエンディアン** / linux-2.6.31.6-rt19（PREEMPT_RT）。
- `kernel/linux.config` … カーネル設定。
- `kernel/patches/` … 自前パッチのみ。
- `firstaid/` … 区画切り直しの足場になる救助用 rootfs（`…_firstaid_defconfig`）。
- ブートローダは libooter（ロック無し・純正温存）。kernel は SPI-NOR mtd5。
- カードリーダは Dione(MB86H58A) の下＝当面 deferred（CONTRIBUTING.md §0）。

対象機: SB-TV02-WFPL。

## 到達点（2026-09-16 実機で確認）

| できること | 確認内容 |
|---|---|
| 自前カーネルの起動 | GPL 2.6.31.6-rt19 を再ビルド → RDHB 梱包 → mtd5。overlayfs / jffs2 を移植して有効化 |
| 書き込み手段の自立 | mtd ツールを持つ first-aid rootfs を mtd6/mtd8 に置き、ベンダ app 層（`dione_server`）非依存に |
| 区画の切り直し | rootfs 5MB / rootfs_data 7MB / firstaid 1MB（下表）。カーネル差し替え → 再起動だけで移行 |
| 書き込み可能な root | squashfs(mtd6) + jffs2(mtd7) の overlayfs を preinit が組み立て、`pivot_root` で本番 root に |
| リモートシェル | dropbear。DHCP でアドレス取得、`passwd` で設定した root パスワードは overlay に残る |
| 復旧手段 | firstaid 区画(mtd8) から起動して mtd6 を書き直す。libooter の cmdline は mtd1 のみ＝可逆 |

未着手: sysupgrade（現状は firstaid から手で書く）、Linux から libooter env を書く自前ツール、
チューナ（段階B/C）。

## SPI-NOR の区画（OpenIRD レイアウト）

`kernel/patches/0015-*.patch` が `arch/arm/mach-mb86hxx/mb86h61_devkit.c` の
`mb86hxx_sflash_parts[]` を差し替える。機械可読な定義は
`host/openird-mkimage/boards/sb-tv02-wfpl.json`。

| mtd | 名前 | offset | size | 内容 |
|---|---|---|---|---|
| 0 | `libooter` | 0x000000 | 1MB | ブートローダ。書かない |
| 1 | `libooter env` | 0x100000 | 64KB | cmdline / MAC。`setparam`+`saveparam` でのみ変更 |
| 2 | `slot info0` | 0x110000 | 64KB | libooter のカーネルスロット選択メタ |
| 3–4 | `nvram0/1` | 0x120000 | 64KB×2 | ベンダ NVRAM。温存 |
| 5 | `kernel bslot` | 0x140000 | 1.5MB | RDHB 梱包カーネル（現行像は 1.26MB＝80%） |
| 6 | `rootfs` | 0x2C0000 | **5MB** | squashfs（RO）。`root=/dev/mtdblock6` |
| 7 | `rootfs_data` | 0x7C0000 | **7MB** | overlayfs の upper（jffs2） |
| 8 | `firstaid` | 0xEC0000 | **1MB** | 救助用 rootfs。`root=/dev/mtdblock8` で起動 |
| 9 | `slot info1` | 0xFC0000 | 64KB | slot info0 とバイト一致の冗長面。温存 |
| 10 | `reserved` | 0xFD0000 | 192KB | 全域 0xFF＝未使用。温存 |

ベンダ配置（rootfs 1MB ＋ `slot0`/`slot1` 各 6MB）からの変更点と根拠:

- **mtd0–5 は触らない。** libooter が直接読む領域で、カーネルも 0x140000 固定。
  カーネル区画を広げるには rootfs の開始位置を動かすしかないが、それはカーネルと
  rootfs の同時書き換えを強制するので採らない。ドライバはベンダ設定の時点で
  USB/SCSI/mac80211/netfilter などが `=m`＝rootfs 側に置けるので、1.5MB で足りる。
- **rootfs の開始は 0x2C0000 のまま。** 区画表だけを差し替えた新カーネルを入れて
  再起動したとき、すでに flash にある rootfs がそのまま起動できる必要があるため。
- **`slot info1` と `reserved` は温存。** slot info1 は slot info0 とバイト単位で
  一致する冗長面（`TOLS` マジック、実データ 44B）で、libooter が読むかどうかが
  未確認。合わせて 256KB＝全体の 1.6% なので確かめるまで手を付けない。
- **`firstaid`（1MB）を置く。** squashfs は実行中に随時フラッシュを読むので、
  mtd6 で動く rootfs は自分自身を書き換えられない。区画を切り直すとベンダ app 層
  （`dione_server`）も消え、flash に書ける物が他に無くなる。別区画の救助 rootfs から
  起動して mtd6 を書く形にすれば、sysupgrade の失敗も開腹せずに戻せる。

## 実機への導入順（区画切り直し）

カーネルと rootfs は別々に書くので、**新カーネル → 再起動 → rootfs** の順に進める。
逆順にすると、旧区画表のカーネルで新しい配置の rootfs を読むことになる。

1. **新カーネルを mtd5 へ**（first-aid が動いている状態で）
   ```sh
   tftp -g -r mtd5-newparts.img -l /tmp/k.img <server>
   mtd write /tmp/k.img /dev/mtd5
   mtd verify /tmp/k.img /dev/mtd5
   reboot
   ```
   cmdline は `root=/dev/mtdblock6` のまま。mtd6 の開始位置が変わらないので
   同じ first-aid がそのまま起動する。`cat /proc/mtd` で新しい 11 区画を確認する。
2. **first-aid を firstaid 区画にも置く**
   ```sh
   tftp -g -r fa.squashfs -l /tmp/fa.squashfs <server>
   mtd write /tmp/fa.squashfs /dev/mtd8
   mtd verify /tmp/fa.squashfs /dev/mtd8
   ```
   libooter で `root=/dev/mtdblock8` にして起動できることを一度確かめておく
   （復旧経路の実地確認）。確認後は `root=/dev/mtdblock6` に戻す。
3. **本番 rootfs を mtd6 へ**（**firstaid から起動した状態で**行う。mtd6 で動いて
   いる rootfs は自分自身を書き換えられない）
   ```sh
   # libooter で root=/dev/mtdblock8 にして起動しておく
   ifconfig eth0 <ip> netmask <mask> up
   tftp -g -r openird-sb-tv02-wfpl-rootfs.squashfs -l /tmp/rootfs.squashfs <server>
   mtd write /tmp/rootfs.squashfs /dev/mtd6
   mtd verify /tmp/rootfs.squashfs /dev/mtd6
   mtd erase /dev/mtd7      # 旧 slot の残骸を消す（数十秒かかる）
   ```
   `root=/dev/mtdblock6` に戻して再起動すると、preinit が mtd7 に jffs2 を作って
   overlay root を組み立てる。**`mtd erase /dev/mtd7` は必須**: 旧 slot0/slot1 の
   squashfs が残っていると jffs2 はマウントを拒否し、tmpfs フォールバック
   （＝設定が保存されない状態）で起動してしまう。

## 本番 rootfs の起動シーケンス

```
libooter → kernel(mtd5) → squashfs(mtd6) を RO で mount → /sbin/init = openird-preinit
   ├─ jffs2(mtd7) を /overlay に mount（失敗したら tmpfs。警告を出して続行）
   ├─ overlayfs (lower=/ upper=/overlay) を /mnt に mount
   ├─ pivot_root /mnt /mnt/rom    ← 以後 / は書き込み可能、元の RO root は /rom
   └─ exec busybox init → inittab → rcS → S40network → S50dropbear
```

cmdline は区画切り直し後も変えない（`root=/dev/mtdblock6 rootfstype=squashfs ro`）。
upper を別のデバイスにするときは `openird_overlay=<device>|none` を足す
（none は RO 起動）。既定値は `rootfs_overlay/etc/openird/board.conf`。

### ⚠ cmdline の `mac_adrs=` を落とさないこと

**個体の MAC アドレスはフラッシュ上、libooter env(mtd1) に入っている cmdline 文字列の
中にしか無い**（全ダンプを検索してバイナリ形式のコピーは存在しないことを確認済み）。
カーネル側も `macadrs_setup_from_cmdline()` が cmdline の `mac_adrs=` を読むだけなので、
**`mac_adrs=` を含まない cmdline を `saveparam` するとその個体の MAC は失われる**。

- `setparam` では既存の 1 行から `root=` だけを差し替える。インストーラを書くときも同じ
  （cmdline を組み立て直さず、既存行を編集する）。
- 保険として、overlay 上の `/etc/openird/network.conf` に `MAC=xx:xx:…` を書いておくと
  S40network が起動時に復元する。ラベルの値を控えておくこと。

> `mtd write` は書いたブロックだけを消去する。区画を丸ごと消すのは `mtd erase`。
> 旧 `slot0`/`slot1` のデータは新 `rootfs_data` の領域に残っているので、
> overlay を初めて使う前に **必ず `mtd erase /dev/mtd7`**（jffs2 は他の FS が
> 残った領域へのマウントを拒否する）。
