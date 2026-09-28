# ベンダファームの起動連鎖と工場モード（SB-TV02-WFPL）

<!-- openird-spec
Status: reference
As-of:  実機 SB-TV02-WFPL の起動ログと平文スクリプト
Impl:   -
Tests:  -
Note:   起動連鎖の記述。直接の実装対象ではない
-->

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

ただし、`slot_entry.sh` より前に同じディスクを**読み取り専用**で mount しておけば、
この経路の rw の mount は EBUSY で失敗し、ディスクの中身に関係なく 2 つの分岐に入らない
（2026-09-29 実機で確認・§7）。

検出は決め打ち：`update check_usb_flash` は `2-1:1.0`（基板上のハブ）を最大 3 秒待ち、
800 ms 置いて **`2-1.3:1.0`** のクラス／サブクラス／プロトコルが `08/06/50` のときだけ
「USB メモリ」とみなす。ハブを挟むと `2-1.3:1.0` はハブ（クラス 09）になり、この経路には
入らない。

## 4. `slot_selector` は mtd8/mtd9 を書き換える

`slot_selector` の文字列に `Slot Duplication starts` / `... end` と、
`Could not open mtd8 or mtd9`・`Could not erase a block`・`Could not write a block`・
`Could not write new tag mtd` がある。**slot1(mtd8) と slot info1(mtd9) を erase して
slot0 を複製する機能を持つ**（発火条件は逆アセを見ないので未確定）。

OpenIRD の区画表では `/dev/mtd8` = **firstaid**（唯一の救助 rootfs）なので、
**ベンダ rootfs をそのまま起動させてはいけない**。rig では rcS から
`slot_selector` を外す（§6）。

**起動時の出力**（2026-09-29・ベンダ像で slot0/slot1 がともにダンプと一致し、slot info の
MD5 欄とも一致している状態）：

```
New slot_selector!!
slot[0].status = 1
slot[1].status = 0
Use slot0
```

この状態では複製は起きず、mtd8/mtd9 の MD5 は起動後も変わらなかった。

## 5. 工場テストシェル（`nop_dbgsh`）

`dione_server` は debug shell スレッド（`nop_dbgsh_init` / `nop_dbgsh.c`）を持つ。
**パスコマンドは要らない。** デーモンを起動すると stdin にプロンプト `-> ` が出て、
その時点で 84 個のコマンドすべてが使える（実機確認・§5.1）。

`Hello Factory Port (please input passcommand)` は**USB シリアル側のチャネル**に
向けた案内で、stdin の可用性とは無関係。`testmode` はコマンド表の 075 番に登録された
**引数 1 個（数値）のコマンド**であって、シェルに入るための鍵ではない（実機確認）:

| 入力 | 応答 |
|---|---|
| `testmode` | `argument count mismatch!` |
| `testmode testmode` | `arg err`（値が不正） |
| `testmode 1` | `Factory_EnterFactryTestMode` を出し、**直後に daemon が exit(0)** |

`testmode 1` は stdin でも USB シリアル側でも同じ挙動で、daemon が終了するため
ウォッチドッグが約 20 秒後に実機をリセットする（§5.2）。工場テストシェルを使う作業では
**打つ必要がない**。

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

### 実機での選局手順（確定・2026-09-19）

工場テストシェルのプロンプト `-> ` で、次の 3 行で実放送波にロックし TS まで通る。

```
forceinit                      → dione_forceInitializeDBG。Dione の電源を入れ直し、
                                 USB に再列挙させる（usb 2-1.4: new high speed USB
                                 device ... / open takes 500 msec）。これを打たないと
                                 open("/dev/h57fmb0") が ENODEV(19) で失敗し続ける
dpower 1 1                     → DTV_POWER_CTRL_DEVICE: Power ON
settuner <系統> <freq Hz> 0 0  → Factory_SetTunerFreq。引数は「0 | <freq> | 0 | 0」と
                                 エコーされる
```

**`settuner` の引数**（2026-09-20 実機で確定）:

| 位置 | 意味 | 確定した値 |
|---|---|---|
| 1 | **系統セレクタ** | **`0` = 地上（`gfreq` の `source` が `T`）/ `1` = BS（`source` が `B`）** |
| 2 | **周波数（Hz 単位の IF 周波数）** | 地上はそのままの周波数、BS は LNB 後の IF（BS-1 = `1049480000`） |
| 3 | 未確定 | `0` で通る |
| 4 | 未確定 | `0` で通る |

第 1 引数の指標は `sfreq` とは**違う**（`sfreq 1 <Hz>` が地上、`settuner 0 <Hz> 0 0` が地上）。
`source` に CS（110°）の値があるかは未検証。

読み出し:

| コマンド | 出力 |
|---|---|
| `gfreq` | `current: source=T / frequency=<Hz> / segment=12 seg`、`last:` に前回値 |
| `chktunerstat` | `Factory_CheckTunerLock` / `ret=0, sync=1, cn=29, ber=0, agc=90` |
| `gettunerber` | `Factory_CheckTunerBER` / 同じ値 + `i=` |
| `gsignal` | `Sync=LOCK` `AGCLevel` `CN`(raw) `E_CN`(dB) `AntennaLevel` `BER_A/B/C` `BER_CAL_*` `Exist_A/B/C`（ISDB-T 階層） |
| `chktunerts` | `Factory_CheckTunerTS` / `there is TS_IN. <n>[msec]` または `No TS_IN. ret = -1` |

実測例:

| | 地上 ch27 | BS-1 |
|---|---|---|
| コマンド | `settuner 0 557142857 0 0` | `settuner 1 1049480000 0 0` |
| `gfreq` source | `T` | `B` |
| `Sync` | `LOCK` | `LOCK` |
| `AGCLevel` | 90 | 65 |
| `E_CN` | 29.0〜29.4 dB | 22.2 dB |
| `AntennaLevel` | 77 | 100（上限） |
| `BER_A/B/C` | 0 / 0 / 0 | 0 / 0 / 0 |
| `Exist_A/B/C` | **T / T / F** | **T / F / F** |
| `chktunerts` | `there is TS_IN. 0[msec]` | `there is TS_IN. 0[msec]` |

`Exist_*` の差は方式の違いを反映する。ISDB-T は階層 A/B/C を持つが、ISDB-S には
その階層が無いので A だけが立つ。

**LNB 電源について（誤解しやすい点）**: 上の BS 受信は `slnbpwr` を一度も叩かずに
ロックしている。これは**検証環境が共同受信で、給電が機器の外から行われているから**で
あって、LNB 電源が不要という意味ではない。個別アンテナでは機器が給電する必要があり、
**`slnbpwr` / `glnbpwr` / `glnbshort`（過電流検知）は段階 C が実装すべき機能**である。
なお未初期化の状態で `glnbpwr` / `glnbshort` を叩くと `-536870909`（Not Initialized 系）
を返す。

**`sfreq <系統> <Hz>` は復調器だけを選局する**（出力なし・即復帰。第 1 引数の指標は
`settuner` と違い、地上が `1`）。これだけでも `Sync=LOCK` になるが `chktunerts` は
`No TS_IN` のまま＝TS 経路は立たない。

### `Factory_SetTunerFreq` の呼び出し列（実機で捕捉）

`settuner 0 557142857 0 0` の実機出力。各段が `done :` で報告される。

```
Factory_SetTunerFreq
0 | 557142857 | 0 | 0
   done : dione_forceInitializeDBG
   done : DtvPower_DevicePowerDBG          （DTV_POWER_CTRL_DEVICE: Power ON）
   done : ControlPort_GetTsStreamHandle_DBG
   done : TsStreamer_open
   [ERROR] CmdReturn = 0x00020018 [ImpSendSyncCommand@WirelessSecureFirmwareCmd.c:579]
   [ERROR] SecureCmd.ulCmdReturn = 131096 [ReleaseTempKey@SecureKeys.c:632]
        ---> ReleaseTempKey() is err, But don't exit Factory_SetTunerFreq()
   done : ReleaseTempKey / SetTempKey
   done : StopStream / TsStreamer_stop
   done : DtvTuning_SetTSIDDBG
   done : DtvTuning_SetFreqDBG
   done : StartTranscodeChange / GetTranscodeMode / SetTranscodeMode / CommitTranscode
   done : TsStreamer_start / StartStream
   finish : Factory_SetTunerFreq()
```

**秘匿鍵は生 TS 経路に不要。** `ReleaseTempKey` が secure firmware から
`0x00020018` を返して失敗しても、コード自身が `don't exit` と宣言して処理を続け、
選局と TS 到達は成立する。鍵の内容・用途には踏み込まない（CONTRIBUTING.md §0）が、
「依存していない」という事実は段階 C の設計に効く。

### TS のゲートは復調器ではなく Dione の TsStreamer（2026-09-19 確定）

3 つの観察から確定した。

1. `settuner` の前は `sfreq` で何度ロックさせても `chktunerts` は `No TS_IN`
2. `settuner` の後に `sfreq` で再選局しても `there is TS_IN` は維持される
3. 復調器レジスタ 256 バイトを「TS 無し」「TS 有り」の両状態でダンプして差分を取ると
   14 個が違うが、**同一状態で 2 回読むと 14 個すべてが変動する**＝測定値であって
   設定ではない（`0xf6` を候補としたが、連続 2 回の読みで `0x00`→`0x0a` と変動して否定）

∴ **復調器は常時 TS を出しており、閉じているのは Dione(MB86H58) の TsStreamer**。
`Factory_CheckTunerTS` が見ているのも Dione の TS 入力側。

段階 C への含意: クリーン実装が必要なのは (a) 復調器・チューナの選局（mainline
`tc90522.c` / `xc5000.c` の公開意味論で書ける）と (b) **TS の取り出し口**。(b) を Dione
経由にすると proprietary な `h57fmb` に依存するため、`sb-tv02-wfpl-stage-b-fapex.md`
§5.6 の「生 TS の 2 経路」のうち **Dione を通らない側（FAPex TSD / Path H）を採る**
判断がここで裏づけられた。

### 復調器レジスタの読み出し手段

| コマンド | 対象 | 形式 |
|---|---|---|
| `tctregr <addr>` / `tctregw` | 復調器・地上側 | 引数 1 個、**16 進で解釈**（`16` も `0x10` と同じ）。出力 ` addr[0xNN] = 0xVV ` |
| `tcsregr` / `tcsregw` | 復調器・衛星側 | 同上（未検証） |
| `xcregr` / `xcregw` | チューナ（Xceive XC5200）側 | 同上（未検証） |
| `cxdregr` / `cxdregw` | 別チップ向け | 同上（未検証） |

ロック状態の 256 バイト全域ダンプを取得でき、個別読み出しで抜き取り検証した
（0x1e=0x2a、0x39=0xaa、0x73=0x45、0xb0=0xa9 が一致）。**逆アセンブルを使わずに
「ベンダの init が作った最終状態」が取れる**＝段階 C の仕様源になる。

なお 0x85〜0x9a 付近と 0x7e・0xdb・0xf6 は読むたびに変動する（測定値・カウンタ）。
設定レジスタと測定レジスタの切り分けには**同一状態での複数回読み**が必須。

関連する Factory_* 一覧: `Factory_EnterFactryTestMode` `Factory_CheckSystemVersion`
`Factory_CheckDione{TraConFW,FW,LoderFW}Version` `Factory_SetLed` `Factory_Ping`
`Factory_NotifyToPushResetSwitch` `Factory_SetTunerFreq` `Factory_CheckTunerLock`
`Factory_CheckTunerBER` `Factory_CheckTunerTS` `Factory_GetBootlog` ほか。

## 6. 観測 rig（ベンダアプリを起動しない状態）

> 2026-09-29 に §7 の vendor-base へ置き換えた。以下は経緯として残す。

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

### rig の運用（実機で踏んだ制約・2026-09-19）

- **ウォッチドッグ。** `dione_server` が動くとハードウェアウォッチドッグが走り、
  デーモンが終了すると**約 20 秒後に実機がリセット**する。製品版 `/slot/app` に
  `stop_watchdog` は無い（開発版パッケージのみ）。`/dev/wdtcounter` は存在するので、
  rig 側で餌をやる仕掛けを足すのが次の課題。
- **ネットワーク。** デーモンは `br0` を作って eth0 を配下に入れる。rig が eth0 に
  静的 IP を振ったままだと二重になって LAN が落ちる。**デーモン起動前に
  `ifconfig eth0 0.0.0.0`** を実行する（ベンダ自身の `/slot/app/d` がそうしている）。
- **コンソールを手元に残す。** デーモンをそのまま起動すると stdin＝コンソールが
  工場テストシェルに占有され、シェルコマンドが打てなくなる。stdin を FIFO にすると
  両方使える:

  ```sh
  mkfifo /tmp/dbgin                  # 複数引数の mkfifo は効かない。1 個ずつ作る
  sleep 999999 > /tmp/dbgin &
  insmod /slot/drv/h57fmb.ko
  dione_server < /tmp/dbgin > /dev/console 2>&1 &
  echo "forceinit" > /tmp/dbgin      # 以後シェルから工場シェルへ注入できる
  ```

- **ファイルの持ち込み。** ベンダ rootfs に sshd は無く、busybox に `nc` も `base64` も
  入っていない。使えるのは `wget` / `tftp` / `uudecode` / `md5sum`。デーモン起動前
  （LAN が生きている間）に `wget http://<host>:<port>/<file>` で取るのが最も早い。
- **観測ツールとの競合。** デーモンが TsStreamer を開いた状態で自前ツールが TSD を
  掴むと双方が壊れる（デーモン側に `ts_streamer.c:467 errno_=22`）。TSD を使う観察は
  `settuner` を使わず `sfreq` だけでロックさせる（＝TsStreamer 未使用）か、`playstop`
  （引数 0）で閉じてから行う。
- **偽 Factory Port。** `/dev/ttyUSB0`(188,0) は静的ノードとして存在し、レガシー BSD pty
  の slave（`/dev/ttyp0`, 3,0）を `mount -o bind` すると `nop_common_USBSerialOpen` が
  成功する（`can't open /dev/ttyUSB0` が消え、`receive from usbtty.` が出る）。master は
  `/dev/ptyp0`(2,0) でパスがあるのでネットワーク非依存に注入できる。ただしレガシー pty は
  slave を誰も開いていないと master の read が EIO になるため、slave を握る番人が必要。
  自前カーネルは `CONFIG_LEGACY_PTYS=y` / `CONFIG_LEGACY_PTY_COUNT=16`。

### 復旧経路（2 本）

1. **firstaid**: 自前カーネルは無変更なので、libooter の `setparam` で cmdline の
   `root=` だけを `/dev/mtdblock8` に差し替えて `saveparam` → 電源再投入。
   （既存行のキーだけ差し替える。cmdline を組み直すと `mac_adrs=` が消える。）
2. **rig 自身から**: ベンダ rootfs には `telnetd` `tftp` `wget` `nc` `udhcpc`
   `ifconfig` `losetup` `chroot` `strings` があり、書き込みは
   `dione_server -w /dev/mtdN:file`（暗号ゲート無し）で行える。

## 7. vendor-base（ベンダ像のまま・入口 1 か所）

2026-09-29 に §6 の rig を置き換えるために設計した観察土台。カーネル・区画表・slot は
ダンプのバイトのまま、rootfs の rcS の `/slot/slot_entry.sh` の直前に telnetd と USB
ディスクのフックだけを差し込む（`tools/vendor-base/`・`scripts/build-vendor-base.sh`）。
運用は `tools/vendor-base/README.md`。**2026-09-29 に実機へ導入し、全区画の MD5 が
想定と一致した**（カーネル・slot0/1・slot info・reserved はダンプと同一、rootfs は差し込み入りの像）。
以下はその設計の根拠になった事実。

### USB の決め打ち（`dione_server` と `update` の文字列より）

| 経路 | 対象 |
|---|---|
| `/sys/bus/usb/devices/1-1`・`1-1/devnum`・`1-1:1.0/net/wlan0/`・`1-1:1.1/net/wlan1/` | Wi-Fi（RTL8192DU）。USB ホスト 1 に直結 |
| `/sys/bus/usb/devices/2-1.3`・`2-1.3:1.0/ttyUSB0` | Factory Port（USB シリアル）。背面 USB-A＝基板上のハブのポート 3 |
| `2-1:1.0`・`2-1.3:1.0/bInterface{Class,SubClass,Protocol}` | `update check_usb_flash`（§3） |

Dione（MB86H58）は基板上のハブのポート 4（`2-1.4`・`06b8:102d`）。USB-A に何も無いとき
アプリは `err : can't open /dev/ttyUSB0 [nop_common_init@nop_common.c:84]` を出して続行する。

### SoC の USB ホスト（MUSB）のエンドポイント

GPL の板設定は `num_eps = 16` だが、実物は**各ホスト 6 本**（起動ログ）：

| hw_ep | tx | rx |
|---|---|---|
| 0 | 64（制御・共用） | |
| 1 | 512 | 512（ドライバが bulk の共用枠に予約） |
| 2 | 512 | 512 |
| 3 | 1024 | 1024 |
| 4 | 1 | 64 |
| 5 | 1 | 32 |

割り込み転送は専用の枠を要し、空きが無いと `-ENOSPC`。受信側で使えるのは ep2〜5 の
4 本で、ハブ 1 段・PL2303・Dione がそれぞれ 1 本取る。USB-A に 2 チップのハブ＋
USB ディスク＋PL2303 を挿すと、PL2303 が `pl2303_open - failed submitting interrupt urb,
error -28` で開けなかった。USB-A に USB ディスクを直挿しした構成では、ベンダアプリの
`forceinit` → `settuner 0 557142857 0 0` → `Sync = LOCK`・`E_CN = 29.0`・
`there is TS_IN` まで通った（エラー無し）。ベンダは `musb_hdrc use_dma=0`（PIO）で load する。

### ベンダカーネルが読めるもの

カーネル像（mtd5）の登録名と出荷 .config より：vfat・msdos（MBR）・squashfs・tmpfs・
NFS クライアントはある。ext2/3/4・jffs2・loop・GPT は無い。USB シリアルは
`usbserial.ko` と `pl2303.ko`（2.6.31・旧型の PL2303HX `067b:2303` 向け）だけで、
PL2303G 系（`067b:23a3`・bcdDevice `0405`）は ID を知らない。`usb-storage` の
`delay_use` は既定 5 秒。レガシー pty（`CONFIG_LEGACY_PTYS=y`・16 本）と
`/dev/ptyp*`・`/dev/ttyp*` のノードがあるので、§6 の「偽 Factory Port」も使える見込み。

### 読み取り専用の先 mount（予行・2026-09-29）

`mount -t vfat -o ro /dev/sda1 /tmp/usb` の後、`slot_entry.sh` と同じ
`mount -t vfat /dev/sda1 /mnt` は `Device or resource busy` で失敗し、先の mount を
外すと成功する。逆に rw の mount が先にあると、読み取り専用の mount が EBUSY になる。

## 8. 参照

- `docs/specs/sb-tv02-wfpl-stage-b-fapex.md` — FAPex chardev 契約・Dione 立ち上げ・
  復調器/チューナ同定・TS データパス。
- `docs/specs/sb-tv02-wfpl-libooter-env.md` — cmdline と MAC の在処。
- `docs/specs/sb-tv02-wfpl-usb-update.md` — `updater.full` / `updater.tar`。
- `br-ext-chip-fujitsu/board/mb86h61/README.md` — OpenIRD の区画表。
