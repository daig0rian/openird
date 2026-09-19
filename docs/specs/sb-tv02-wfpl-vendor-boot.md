# ベンダファームの起動連鎖と工場モード（SB-TV02-WFPL）

<!-- Open 層: 所有者の実機ダンプ（mtd6 rootfs / mtd7 slot0 の squashfs）に含まれる
     シェルスクリプトの読み取りと、ELF の文字列抽出（strings）のみ。逆アセ由来の
     内部ロジック・具体アドレスは置かない。シリアルコンソールでの起動観察は所有者
     による一次観察。段階 B の観測 rig はこの仕様のみを根拠に作る。 -->

ベンダのユーザランドがどこで何を起動するかを、起動スクリプトの読みから確定した
もの。段階 B の観測 rig（ベンダアプリを起動しない状態）の設計根拠であり、
`docs/specs/sb-tv02-wfpl-stage-b-fapex.md` のサンドボックス手順に対する
**「純正はどうやっていたか」の基準**にあたる。

## 1. 起動連鎖

```
libooter → kernel(mtd5) → /sbin/init (= BusyBox init)
  ├ /etc/inittab (sysinit)
  │   mount -o remount,rw /  → mount -t proc proc /proc → mkdir -p /dev/pts
  │   → mount -a → hostname -F /etc/hostname → /etc/init.d/rcS
  │   (respawn: getty tty1/tty2、syslogd -n -m 0、klogd -n、tail -f /var/log/messages on tty3)
  └ /etc/init.d/rcS
      ├ dmesg -n 1            ← コンソールへのカーネルメッセージを事実上止める
      ├ cd /root
      ├ /proc/cmdline が root=/dev/mtdblock* を含むとき:
      │   ./slot_selector  → 終了コード 0=slot0 / 1=slot1 / その他=./start_hazard
      │   mount -t squashfs /dev/mtdblock7 /slot   (slot0)
      │   mount -t squashfs /dev/mtdblock8 /slot   (slot1)
      ├ export LD_LIBRARY_PATH=/slot/lib ; PATH=/slot/app:$PATH
      ├ /slot/slot_entry.sh                       ← ベンダアプリの入口
      └ while : ; do setsid cttyhack /bin/sh ; done
            ↑ slot_entry.sh が **返ると** コンソールに root シェルが出る
```

最後の行が重要で、**`slot_entry.sh` を正常終了させれば、追加の細工なしに
シリアルコンソールが root シェルになる**。rcS 自身がそう書かれている。

`/root`（rcS の cwd）に置かれているもの: `slot_selector`・`start_hazard`・
`gpiotool`・`revision.txt`。

## 2. `/slot/slot_entry.sh` の順序（ベンダアプリ起動まで）

| # | 内容 | 備考 |
|---|---|---|
| 1 | `rngd -b -o /dev/random -r /dev/urandom` | |
| 2 | `/slot/app/core.sh` があれば `ulimit -c unlimited`・`core_pattern`・**`telnetd -l sh`** | **開発用パッケージのみ**。製品パッケージには `core.sh` が無い |
| 3 | `modprobe scsi_mod sd_mod usbcore usb-storage musb_hdrc(use_dma=0) usbserial pl2303` | **USB を FAPex より先に**入れる |
| 4 | `sysctl -w net.ipv4.tcp_window_scaling=1` / `tcp_mem="20000000 20000000 20000000"` / `net.core.wmem_max=10000000` | TS 配信向けの受け皿 |
| 5 | `ifup lo` | |
| 6 | `modprobe fapex_fdltsd fapex_fdli2c fapex_fdlgpio fapex_fdlclkpwr` | **4 つすべて**。純正は `fdlclkpwr` を載せる |
| 7 | `modprobe lib80211 cfg80211 mac80211` | |
| 8 | cmdline が `root=/dev/mtdblock8` なら `update restore_rootfs` | フルアップデート直後の復帰経路 |
| 9 | `root=/dev/nfs` でなければ `ifconfig eth0 up`（アドレスは付けない） | |
| 10 | USB アップデート経路（下記 §3） | |
| 11 | `insmod /slot/drv/h57fmb.ko` | **Dione(H58) ドライバ** ←切断点の候補 1 |
| 12 | `while : ; do dione_server ; done`（NFS 起動なら `-n`） | **ベンダアプリ本体** ←切断点の候補 2。終了コード 3 のときだけ `ifdown ra0 / ifup ra0` して再起動 |
| 13 | （アプリ終了後）`/tmp/updater.tar` があれば `update prepare` → `exec /tmp/update` | |
| 14 | （同）`core.sh` があれば `stop_watchdog` してコアを tftp で吸い出す | 開発用のみ |

**サンドボックス（§5 of stage-b spec）との差**が 3 点ある。純正は (a) USB スタックを
FAPex より**先**に入れ、(b) `fapex_fdlclkpwr` を**載せ**、(c) `modprobe` の順は
`fdltsd → fdli2c → fdlgpio → fdlclkpwr`（modinfo の依存順ではなくこの順で足りる）。

## 3. USB アップデート経路（rig で踏まないための注意）

段階 10 は「リセット SW が押されている」または「USB メモリが挿入されている」ときだけ
入る。`dione_server -s` がリセット SW の読み取り、`update check_usb_flash` が USB メモリ
検出、`update check_just_updated` が直前のアップデート完了判定。マウント後の分岐は
2 つあり、どちらも**破壊的**:

- `/mnt/updater.full` → `dione_server -d`（復号）→ `update prepare` → `exec /tmp/update`
- `/mnt/dionekeyEraser.full` → `dione_server -d` → `insmod h57fmb.ko` → **`dione_server -e`（Dione 鍵消去）**

∴ **観測中は FAT の USB マスストレージを挿さない**。USB シリアル変換器（PL2303）は
ブロックデバイスにならないので `/sys/block/sda` が現れず、この経路には入らない。

## 4. `slot_selector` は mtd8/mtd9 を書き換える

`slot_selector` の文字列に `Slot Duplication starts` / `... end` と、
`Could not open mtd8 or mtd9`・`Could not erase a block`・`Could not write a block`・
`Could not write new tag mtd` がある。**slot1(mtd8) と slot info1(mtd9) を erase して
slot0 を複製する機能を持つ**（発火条件は逆アセを見ないので未確定）。

OpenIRD の区画表では `/dev/mtd8` = **firstaid**（唯一の救助 rootfs）なので、
**ベンダ rootfs をそのまま起動させてはいけない**。rig では rcS から
`slot_selector` を外す（§6）。

## 5. 工場モード（`nop_dbgsh`）

`dione_server` は debug shell スレッド（`nop_dbgsh_init` / `nop_dbgsh.c`）を持ち、
パスコマンド `testmode testmode` で工場テストモードに入る。

**入力チャネルは 2 系統**:

- **標準入力（`/dev/console` = シリアル）** — `dione_server` は inittab→rcS→
  `slot_entry.sh` から起動されるので stdin はコンソール。所有者がシリアルコンソールで
  「ユーザランド起動後にベンダアプリが待受けに入る」ことを実機観察している（一次観察）。
- **USB シリアル（`/dev/ttyUSB0`）** — `nop_common_USBSerialOpen`（`nop_common.c`）で
  開く補助チャネル。無ければ `wrn. not open ttyusb.` を出して続行する。関連文字列:
  ` Hello Factory Port (please input passcommand)`、
  ` receive from usbtty. but not factoryMode`、`UsbSerialReopen`、
  `/sys/bus/usb/devices/2-1.3:1.0/ttyUSB0`、`dmesg  > /dev/ttyUSB0`。
  §2 段階 3 で `usbserial`・`pl2303` を入れているのはこのため（工場治具側が
  PL2303 の USB シリアル変換器）。

### コマンド表（`dione_server` の文字列より）

| 分類 | コマンド |
|---|---|
| 基本 | `exit` `list` `ssloglev` `kloglev` `dmesg` `memr` `memw` `memdump` `getbootlog` `getsysver` |
| 電源/初期化 | `releaseforceinit` `dpower` `rstsw` `led` |
| **選局** | **`sfreq` `gfreq` `setfreq` `setfreqd` `gsignal` `stsid` `gtsid` `settuner` `chktunerstat` `gettunerber` `chktunerts`** |
| 衛星 | `slnbpwr` `glnbpwr` `glnbshort` |
| **TS/再生** | **`source` `setDd` `stopunit` `playstart` `playstop` `playsts`** |
| レジスタ直叩き | `cxdregw` `cxdregr` `xcregw` `xcregr` **`tctregw` `tctregr` `tcsregw` `tcsregr`** |
| USB | `usbhs` `echousb` `dbprusb` `usboc` `getproductid` `gettraco` `getlederr` `chkusbinsert` |
| Dione FW/版 | `getdioneTrsFwVer` `getdioneFwVer` `getdioneLdFwVer` `wdioneLdFw` |
| WiFi | `setssid` `getssid` `setwifipasswd` `chkwifipasswd` `enableap` `setwlansec` `chkwlanstat` `chkwlansignallevel` `startthroughputtest` `stopthroughputtest` `getwlanmacaddr` |
| 個体情報 | `wmacaddr` `getmacaddr` `setserno` `getserno` `getcasnum` `chkcasinsert` |
| CAS 鍵系 | `setdionekey` `chkdionekey` `chkotpstat` `deldionekey` |
| その他 | `testmode` `rtt` `tunerstatp` `tunerstatpd` |

`tct*`/`tcs*` は復調器（東芝 TC90532）の地上/衛星側レジスタに対応すると読める
（`tct` = 地上、`tcs` = 衛星。I2C slave `0x20`/`0x21` と `0x22`/`0x23` に対応）。
`xc*` はチューナ Xceive XC5200 側。→ `docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §5.5。

**CAS 鍵系（`setdionekey`/`chkdionekey`/`deldionekey`/`chkotpstat`）は扱わない。**
限定受信は永久にスコープ外（CONTRIBUTING.md §0）。存在の記録だけに留める。

### `Factory_SetTunerFreq` の内部順序（ログ文字列より）

同関数のログ文字列が、選局 1 回の手順をそのまま並べている:

```
dione_forceInitializeDBG → DtvPower_DevicePowerDBG
  → ControlPort_GetTsStreamHandle_DBG → TsStreamer_open
  → SetTempKey → StartStream / TsStreamer_start
  → DtvTuning_SetTSIDDBG
  （終了時 StopStream / TsStreamer_stop / ReleaseTempKey）
```

エラー文字列に `[ERROR] No Streamer`、`[ERROR] l-%d, TsStreamer_start() = %d`、
`Sync byte of TS stream is bad.` があり、**TS の取り出しまで工場モードで閉じている**。
`SetTempKey`/`ReleaseTempKey` は名前のみ記録する（鍵の内容には踏み込まない）。

関連する Factory_* 一覧: `Factory_EnterFactryTestMode` `Factory_CheckSystemVersion`
`Factory_CheckDione{TraConFW,FW,LoderFW}Version` `Factory_SetLed` `Factory_Ping`
`Factory_NotifyToPushResetSwitch` `Factory_SetTunerFreq` `Factory_CheckTunerLock`
`Factory_CheckTunerBER` `Factory_CheckTunerTS` `Factory_GetBootlog` ほか。

## 6. 観測 rig（ベンダアプリを起動しない状態）

**方針**: 自前カーネル（mtd5）と firstaid（mtd8）は**一切書かない**。ベンダの
rootfs と slot0 を OpenIRD の区画に置き、起動スクリプト 2 本だけを差し替える。
ベンダバイナリは 1 バイトも改変しない。

| 区画 | 中身 | 変更点 |
|---|---|---|
| mtd5 (1.5MB) | 自前カーネル | **無変更**（ベンダ .ko と ABI 一致は `make test-kabi` で検証済み） |
| mtd6 (5MB) | ベンダ rootfs squashfs | `/etc/init.d/rcS` のみ差し替え（`slot_selector` を通さず `mtdblock7` を直接 mount、`dmesg -n 7`） |
| mtd7 (7MB) | ベンダ slot0 squashfs | `slot_entry.sh` の §2 段階 11–14 を除去（＝ベンダアプリ起動を削る）＋ 静的 IP・`telnetd`・`fapex_trace.so` を追加 |
| mtd8 (1MB) | firstaid | **無変更**（救助経路） |
| cmdline | `root=/dev/mtdblock6` | **無変更**＝`mac_adrs=` を触らない |

ベンダの区画表（mtd6=1MB/mtd7=slot0 6MB/mtd8=slot1 6MB）には戻さない。slot0 の
位置が OpenIRD の mtd6 の内側に来るうえ、ベンダ mtd8 の領域が firstaid を含むため。

### 再梱包の手順（root 権限なしで元と完全一致させる）

ベンダ rootfs の squashfs には**静的デバイスノードが 451 個**入っている
（`/dev/console`・`/dev/fapi/fapi000`〜`255`（major 244）・`/dev/bus/usb/*`）。
非 root の `unsquashfs` はこれを作れないので、次の手順で復元する。

1. `unsquashfs -lls` でマニフェストを取る（種別・モード・所有者・major/minor）。
2. 展開する（デバイスノードは飛ぶ）。
3. マニフェストから **`mksquashfs -pf`（pseudo-file）** を生成してノードを復元。
4. 非 root 展開で umask に削られたモードを、マニフェストから `chmod` で戻す
   （元は全ファイル `root/root`、`/bin/busybox` は **setuid**）。
5. `mksquashfs -b 131072 -comp gzip -no-xattrs -noappend -all-root`
   （元と同じ squashfs 4.0 / gzip / 128KB ブロック。xattr は 2.6.31 が読めない）。

**検証**: 無改変で往復させてマニフェストが完全一致すること（rootfs 910 / slot0 142
エントリ）。作った像は実機の 2.6.31 squashfs ドライバで loop mount して確認する。

元イメージの諸元: rootfs = 4.0/gzip/128KB、`bytes_used` 953,223（1MB 区画）。
slot0 = 同じ、`bytes_used` 4,203,033（6MB 区画）。

### 復旧経路（2 本）

1. **firstaid**: 自前カーネルは無変更なので、libooter の `setparam` で cmdline の
   `root=` だけを `/dev/mtdblock8` に差し替えて `saveparam` → 電源再投入。
   （既存行のキーだけ差し替える。cmdline を組み直すと `mac_adrs=` が消える。）
2. **rig 自身から**: ベンダ rootfs には `telnetd` `tftp` `wget` `nc` `udhcpc`
   `ifconfig` `losetup` `chroot` `strings` があり、書き込みは
   `dione_server -w /dev/mtdN:file`（暗号ゲート無し）で行える。

## 7. 参照

- `docs/specs/sb-tv02-wfpl-stage-b-fapex.md` — FAPex chardev 契約・Dione 立ち上げ・
  復調器/チューナ同定・TS データパス。
- `docs/specs/sb-tv02-wfpl-libooter-env.md` — cmdline と MAC の在処。
- `docs/specs/sb-tv02-wfpl-usb-update.md` — `updater.full` / `updater.tar`。
- `br-ext-chip-fujitsu/board/mb86h61/README.md` — OpenIRD の区画表。
