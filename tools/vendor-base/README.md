# vendor-base（SB-TV02-WFPL の観察土台）

段階 B の観察に使う**基本の像**。フラッシュをベンダの状態へそのまま戻し、rootfs の
`/etc/init.d/rcS` に 1 か所だけ差し込んで **telnet の root シェル**を足したもの。

**配布イメージには入らない。** 所有者が自分の機器のダンプから作り、自分の機器で使う。

## なぜ旧 rig（`tools/vendor-rig/`）をやめるのか

BMMPX1301 で分かった教訓：**オープンな像の上にベンダを載せるより、ベンダの像から
始めて入口だけ足す方が、純正との差が小さく、ベンダアプリが素直に動く。**
旧 rig は自前カーネル＋OpenIRD の区画表の上でベンダのユーザランドを動かしていたため、
`slot_selector` を外す・アプリの起動を削るといった差が要った。

| | 旧 rig（2026-09-20） | vendor-base |
|---|---|---|
| カーネル（mtd5） | 自前（OpenIRD の区画表） | **ベンダ**（ダンプのまま） |
| 区画表 | OpenIRD | **ベンダ** |
| rootfs | rcS を置き換え（`slot_selector` を外す） | rcS に**差し込むだけ** |
| slot0 / slot1 | slot0 を改変（アプリ起動を削る） | **ダンプのまま** |
| ベンダアプリ | 起動しない | **通常どおり起動** |
| firstaid | あり | なし（ベンダの区画表には置けない） |

slot は 1 バイトも変えられない。slot info（mtd2/mtd9）に **slot0/slot1 の 6MB 全体の
MD5** が入っているため（`docs/specs/sb-tv02-wfpl-libooter-env.md`）。

## 変えるのは 2 点だけ

1. rootfs の rcS：`/slot/slot_entry.sh` を呼ぶ行の直前に [rcS-insert.sh](rcS-insert.sh) を
   差し込む（telnetd と USB ディスクのフック。ベンダの rcS 本文はリポジトリに持たず、
   ビルド時にダンプへ差し込む）。
2. cmdline（mtd1）：ベンダの原文に戻す（`init=/bin/sh` は付けない）。

```
console=ttyAMA0,115200 root=/dev/mtdblock6 rw ip=none mac_adrs=<個体の値>
```

## ビルド

```sh
scripts/build-vendor-base.sh [--dump temp/sbtv02_fw.bin]    # WSL（ext4 の作業領域）で
```

ダンプの整合（slot info の MD5 欄＝slot の MD5）、rootfs の往復テスト、
差分が rcS だけであること、1MB 区画に収まることを検査する。出力は `temp/vendor-base/`
（commit しない）：`base-rootfs.sqfs`・`vendor-kernel.bin`・移行用の
`xfer-rootfs.bin` / `xfer-rootfs_data.bin`・`expected.md5`。

## 使い方

構成（2026-09-29 に rig 上で予行済み）:

```
PC 有線 192.168.200.100/24 ──┬── 機器 eth0（br0:9 = 192.168.200.10）
                             └── （家の LAN。つながっていればアプリが DHCP で br0 を取る）
PC ── COM3 ── 機器の基板 UART（ttyAMA0）
機器の USB-A ── USB ディスク（FAT32・直挿し。ハブは使わない）
```

- **telnet `192.168.200.10`**（パスワード無しの root）。USB ディスクの `rc.sh` が、
  ベンダアプリの作る br0 に固定の 2 つめのアドレスとして付ける（アプリが DHCP で
  取る主アドレスは毎回変わりうる。予行では `192.168.11.13`）。シェルの
  `LD_LIBRARY_PATH` / `PATH` はベンダアプリと同じ（`/slot/lib`・`/slot/app`）。
- **シリアル（COM3）はベンダアプリの工場テストシェル `-> `**
  （`docs/specs/sb-tv02-wfpl-vendor-boot.md` §5。選局は `forceinit` → `dpower 1 1` →
  `settuner …`）。
- **ベンダアプリ抜き**：USB ディスクに `openird/noapp` を置く（下記）。libooter で
  cmdline に `init=/bin/sh` を足す方法もある（rcS を通らない素の root シェル・telnet 無し）。

注意：

- `dione_server` を止めるとウォッチドッグが約 20 秒で機器をリセットする。
- telnet は認証無し。ベンダアプリが上げる Wi-Fi の AP 側からも届く。

### USB ディスク（観察の置き場・切り替えスイッチ）

YB1 の `/app/setting` にあたるもの。ベンダ配置のフラッシュには書ける領域が無いので、
USB-A に挿したディスクで代える。**抜けば「純正＋telnet」で起動する。**

- **FAT32・MBR・USB-A に直挿し。** ベンダカーネルが読めるのは vfat と MBR だけ
  （ext2/3/4・jffs2・loop・GPT は無い）。ハブを挟まない理由は下の「USB の制約」。
- rcS は起動時にディスクを **読み取り専用** で `/tmp/usb` に mount し、
  `openird/rc.sh` を source する（CR を取り除いた写しを構文検査してから）。
  直後に `slot_entry.sh` が同じディスクを rw で `/mnt` に mount して `updater.full` /
  `dionekeyEraser.full` を探すが、読み取り専用の mount がある間は EBUSY で失敗するので、
  **USB 更新経路と Dione 鍵消去経路には入らない**（予行で確認）。
- 雛形は [usb/openird/rc.sh](usb/openird/rc.sh)。ディスクの `openird/` に置く。
  `openird/bin/` に置いた道具（`fapex_trace.so`・`fapex-ts` など）は `/tmp/openird/bin` に
  写される。

| ディスクの `openird/` に置くファイル | 効果 |
|---|---|
| `rc.sh` | 起動時に source される（`exit` しないこと） |
| `noapp` | ベンダアプリを起動しない（`slot_entry.sh` の `insmod h57fmb.ko` 以降を bind mount で外す）。eth0 に `192.168.200.10`、COM3 は root シェル |
| `trace` | `dione_server` を FAPex ioctl トレーサの下で起動（`/tmp/fapex-trace.log`） |
| `loglevel` | `dmesg -n` の値（中身は数字 1 文字） |

結果を電源断の後まで残すには `/tmp/openird/bin/openird-save FILE...`
（ディスクの `openird/log/` へ写して読み取り専用に戻す）。ディスクを書き換えるときは
`openird-rw` → 編集 → `openird-ro`。

### USB の制約（実機で確かめたこと）

- **USB-A は基板上のハブのポート 3（`2-1.3`）。** Wi-Fi は別のホストに直結（`1-1`）で、
  `dione_server` が `1-1:1.0/net/wlan0` を決め打ちしているので触らない。
- **SoC の USB ホストはハードのエンドポイントが 6 本**（ep0 制御・ep1 bulk 共用・
  ep2〜5）。割り込み転送に使える受信枠は 4 本で、ハブ 1 段・PL2303・Dione が 1 本ずつ
  取る。2 チップのハブ＋PL2303 を足したら PL2303 が `-ENOSPC` で開けなかった。
  直挿しの USB ディスクなら、基板のハブ＋Dione の 2 本で済む（予行で Dione の列挙・
  選局・`there is TS_IN` まで確認）。
- **Factory Port（`dione_server` の USB シリアル）は `2-1.3:1.0/ttyUSB0` の決め打ち。**
  観察するときは USB ディスクの代わりに PL2303 を直挿しする。ベンダの `pl2303.ko`
  （2.6.31）は旧型の PL2303HX（`067b:2303`）向けで、手元の G 系（`067b:23a3`）は
  ID を知らない。

## 救助

| 壊れたもの | 戻し方 |
|---|---|
| `rc.sh`・フラグ | USB ディスクを抜いて再起動（純正＋telnet で起動する） |
| rcS の差し込み | libooter で `init=/bin/sh` → rcS を通らない root シェル |
| cmdline | libooter の `setparam` + `saveparam`（mtd1 のみ・可逆）。`mac_adrs=` を残す |
| rootfs・カーネル | SPI-NOR クリップで全ダンプを書き戻す（ダンプの mtd1 は `init=/bin/sh` 入り） |

OpenIRD の区画表へ戻るときは `init=/bin/sh` のシェルから firstaid を入れ直す
（`br-ext-chip-fujitsu/board/mb86h61/README.md` の導入順）。

## 移行手順（OpenIRD の区画表 → vendor-base）

2026-09-29 にこの手順で実機を移行し、全区画の MD5 が `expected.md5` と一致した。
そのとき踏んだこと：

- **`nc -l` の受信中はコンソールに何も打たない。** 打った文字は `nc` の標準入力から PC へ
  送られ、閉じ終えた PC 側が RST を返すと、機器側の未読データが捨てられて像が途中で
  切れる（5MB 中 4.9MB で切れた）。PC 側は送り終えたら半分閉じ、機器側が閉じるのを
  待ってから閉じる。
- **libooter は割り込みの直後に 1 回空打ちしてからコマンドを打つ。** 割り込み直後の
  最初のコマンドが `Error: unknown command ""` になることがある。libooter の改行は
  LF→CR の順。

OpenIRD の firstaid（mtd8＝0xEC0000〜1MB）から書く。書く先はベンダ配置の
0x2C0000〜0xFBFFFF（rootfs 1MB＋slot0 6MB＋slot1 6MB）とカーネル。firstaid の 1MB は
ベンダ配置では slot1 の末尾の未使用域（0xFF）なので、**最後に消去するだけ**で slot1 が
ダンプと一致する。

1. libooter で cmdline の `root=` を `/dev/mtdblock8` にして `saveparam` → 電源再投入
   （firstaid が起動する）。
2. firstaid で受け取る。PC の有線ポートは Windows では「パブリック」扱いで、機器から
   PC への TFTP は止められやすいので、**PC から機器へ送り込む**（`nc` の待ち受け）：
   ```sh
   ifconfig eth0 192.168.200.10 netmask 255.255.255.0 up
   nc -l -p 5000 > /tmp/vendor-kernel.bin        # 1 本ずつ。PC 側から送ると終わる
   nc -l -p 5000 > /tmp/xfer-rootfs.bin
   nc -l -p 5000 > /tmp/xfer-rootfs_data.bin
   md5sum /tmp/*.bin                             # files.md5 と突き合わせる
   cp /sbin/mtd /bin/busybox /tmp/               # 最後の消去は tmpfs から実行する（静的リンク）
   ```
   PC 側（各 `nc -l` に対して 1 回）：
   ```sh
   python -c "import socket,sys; s=socket.create_connection(('192.168.200.10',5000)); s.sendall(open(sys.argv[1],'rb').read()); s.close()" temp/vendor-base/<ファイル>
   ```
3. 書いて検査する（**ここまでは firstaid が無傷＝やり直せる**）：
   ```sh
   mtd write /tmp/xfer-rootfs.bin rootfs;           mtd verify /tmp/xfer-rootfs.bin rootfs
   mtd write /tmp/xfer-rootfs_data.bin rootfs_data; mtd verify /tmp/xfer-rootfs_data.bin rootfs_data
   mtd write /tmp/vendor-kernel.bin "kernel bslot"; mtd verify /tmp/vendor-kernel.bin "kernel bslot"
   ```
4. 自分の区画を消す（以後 firstaid は無い）：
   ```sh
   /tmp/mtd erase firstaid
   /tmp/busybox md5sum /dev/mtd8          # expected.md5 の openird mtd8（1MB の 0xFF）
   ```
5. 電源再投入 → libooter で cmdline をベンダの原文に戻して `saveparam` → 電源再投入。
6. USB ディスク（`openird/rc.sh` 入り）を挿したまま起動し、telnet `192.168.200.10` で
   `md5sum < /dev/mtdN` を `expected.md5` の `vendor` 行と
   突き合わせる。
