# first-aid rootfs（SB-TV02-WFPL）

区画を OpenWrt 流に切り直すための**足場**。ベンダ app 層（`dione_server`）に頼らず、
自分で MTD を書ける最小の rootfs を旧 mtd6（1MB）に置くのが目的。

```
make BOARD=mb86h61_softbank_sb-tv02-wfpl_firstaid
→ output/<BOARD>/images/openird-firstaid-sb-tv02-wfpl-rootfs.squashfs
```

## 何が入っていて、何が入っていないか

| 入っている | 理由 |
|---|---|
| BusyBox（ash, tftp, nc, flashcp, ifconfig, udhcpc, dd, md5sum …） | 転送と最低限の操作 |
| `mtd`（OpenWrt 由来・静的リンク） | 区画の write/erase/verify。これが本命 |
| 静的 `/dev`（mtd0–mtd15, mtdblock0–15, ttyAMA0 …） | 2.6.31 に devtmpfs は無い |

**入れていないもの＝ MTD に触る自動処理。** 起動時に走るのは擬似 FS のマウントだけで、
`/slot` の類は一切マウントしない。これは第3段階（区画切り直し）で、**mtd6 の先頭から
新しい rootfs が始まる**新区画表のカーネルに差し替えたあと、mtd7 以降の内容が
旧区画表のままでも安全に起動できるようにするため。区画の読み書きは人が手で行う。

ネットワークも自動では上げない（`ifconfig` / `udhcpc` を手で叩く）。

## 実機手順（第2段階）

安全側を採り、**まず mtd7 に置いて起動確認**してから mtd6 へ移す。失敗しても
mtd6 の純正 rootfs が残っているので、cmdline を戻すだけで元に戻る。

### 1. イメージを転送する（booter-root の `init=/bin/sh` で）

PC 側 TFTP サーバのルートに `openird-firstaid-sb-tv02-wfpl-rootfs.squashfs` を置く。

```sh
mount -t proc proc /proc
mount -t tmpfs tmpfs /tmp
ifconfig eth0 192.168.11.10 netmask 255.255.255.0 up
tftp -g -r openird-firstaid-sb-tv02-wfpl-rootfs.squashfs -l /tmp/fa.squashfs 192.168.11.8
md5sum /tmp/fa.squashfs      # PC 側の sha256/md5 と突き合わせる
```

### 2. mtd7 へ書く（`dione_server` は mtd8 側のスロットから使う）

```sh
mount -t tmpfs tmpfs /mnt
mkdir -p /mnt/slot
mount -t squashfs /dev/mtdblock8 /mnt/slot
export LD_LIBRARY_PATH=/mnt/slot/lib
export PATH=/mnt/slot/app:$PATH
dione_server -w /dev/mtd7:/tmp/fa.squashfs
```

### 3. cmdline を mtd7 起動に変える → 再起動

libooter の `setparam` + `saveparam`（mtd1 のみ・可逆）で、`init=/bin/sh` を外し
root を mtd7 にする。`mac_adrs=` は個体の値をそのまま残す。

```
console=ttyAMA0,115200 root=/dev/mtdblock7 rootfstype=squashfs ro ip=none mac_adrs=<個体の値>
```

起動したら `cat /etc/openird_release` でビルドを確認し、`cat /proc/mtd`・
`mtd` のヘルプが出ること、`ifconfig`/`tftp` が通ることを見る。

### 4. 自分自身を mtd6 へ書く（first-aid の `mtd` で）

```sh
ifconfig eth0 192.168.11.10 netmask 255.255.255.0 up
tftp -g -r openird-firstaid-sb-tv02-wfpl-rootfs.squashfs -l /tmp/fa.squashfs 192.168.11.8
mtd write /tmp/fa.squashfs /dev/mtd6
mtd verify /tmp/fa.squashfs /dev/mtd6
```

`root=/dev/mtdblock6` に戻して再起動。ここまでで「ベンダ app 層に依存せず
MTD を書ける rootfs が mtd6 にある」状態になり、第3段階（区画切り直し）に進める。

**注意**: `mtd write` は書いたブロックだけを消去する。区画を丸ごと消したいときは
`mtd erase <区画>`。区画は `/dev/mtdN` か `/proc/mtd` の名前（`"rootfs"` など）で指定する。

## 復旧

- cmdline は mtd1 のみを触るので、libooter から元の 1 行に戻せば純正 rootfs（mtd6）で起動する。
- mtd6 を潰してしまった場合は、booter-root（`init=/bin/sh`）→ mtd8 スロットの
  `dione_server` で書き戻す。mtd0/mtd1 を書かない限り libooter は無傷。
- 最終手段は SPI-NOR クリップ（`docs/` の復旧手順）。
