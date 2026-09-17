# FAPex ユーザランド ↔ カーネルの契約（SB-TV02-WFPL / 段階 B の観察）

<!-- Open 層: (1) GPL ソース(FAPexK/FAPexU)の読解と (2) 自前クリーン Linux 上で
     ベンダ blob を走らせた strace 観察のみ。逆アセ由来の内部ロジックは置かない。
     個体識別情報は伏字。将来のクリーン実装(ird-tuner-*)はこの仕様だけを根拠に書く。 -->

Milestone 1。所有者の機器から吸い出したベンダ受信部品（`dione_server` と
FAPex `.ko`）を、OpenIRD が GPL から再ビルドした 2.6.31.6-rt19 カーネルの上で
走らせ、受信の主役が使うカーネル ABI を観察した記録。**受信ドライバをクリーン
実装するとき（段階 C）に満たすべき契約**をここに固定する。

観察は 2026-09-17、`sb-tv02-wfpl` 実機（Fujitsu MB86H61）で行った。手順は §5。

## 1. 受信部品の構成（GPL modinfo ＋ ダンプ）

FAPex は「カーネル薄層（chardev を生やす）＋ ユーザ抽象 lib（ioctl を叩く）」の
2 層。カーネル薄層のうち **GPL で配布されるのは wrapper だけ**で、実体（FAPI
core / sys）は proprietary。

| モジュール | ライセンス(modinfo) | 依存 | 役割 |
|---|---|---|---|
| `fapi_sys.ko` | Proprietary | — | FAPI システム抽象層。最初に入れる |
| `fapi_core.ko` | Proprietary | fapi_sys | FAPI コア（6.1MB＝受信の知能の大半） |
| `fapex_cfdl.ko` | GPL | fapi_core, fapi_sys | chardev 登録・共通ディスパッチ |
| `fapex_fdltsd.ko` | Proprietary | fapex_cfdl, fapi_sys | TS demux（TSD） |
| `fapex_fdli2c.ko` | Proprietary | fapex_cfdl, fapi_sys | I2C（復調・チューナ制御の足） |
| `fapex_fdlgpio.ko` | Proprietary | fapex_cfdl, fapi_sys | GPIO |
| `fapex_fdlclkpwr.ko` | Proprietary | fapex_cfdl, fapi_sys | クロック・電源 |

`slot_entry.sh`（平文）の順は `scsi/usb/musb/pl2303` → `fapex_fdl{tsd,i2c,gpio,clkpwr}`
→ `lib80211/cfg80211/mac80211` → `h57fmb.ko`（Dione）→ `dione_server`。

**全て vermagic `2.6.31.6-rt19 mod_unload ARMv6`。** OpenIRD カーネルは
`CONFIG_MODVERSIONS=n`＋この vermagic なので、パッチ 0003/0004 の構造体位置さえ
崩さなければ（`make test-kabi` で保証）そのまま insmod できる（§5 で実機確認）。

## 2. chardev ABI（`/dev/fapi/fapiNNN`）

`fapex_cfdl` が major **244** の chardev を 1 つ登録し、**minor がデバイス種別を選ぶ**。
ユーザ lib（`libfapex_abst`）は `abst_common_priv.c` で
`sprintf(pathname, "/dev/fapi/fapi%03d", id)` を開く（GPL で確認）。ノードは静的に
`fapi000`〜`fapi255` が rootfs にある（欠番あり）。

種別ごとの minor の求め方はユーザ lib 内（proprietary）だが、**ioctl コマンド番号に
種別ベース値が埋まっている**ので、開いた minor と ioctl 番号の対応は観察できる。

観察された対応（§5 のトレース）:

| 用途 | minor | 観察された ioctl（引数は fapex-trace で復号） |
|---|---|---|
| GPIO（リセット SW 読み） | `fapi186` | `nr=0x40` GPIO_OPEN(W,8B) pin=0x44・func=0x01010000 → `nr=0x41` GPIO_READBIT(R,4B) → 1 |
| CLKPWR（起動時のクロック/電源 init） | `fapi199` | `nr=0x00` OPEN → `nr=0x21` CLKPWR(W,8B) in=06 00 00 00 00 00 00 00 |

minor と種別ベース（§3 の NB 表）は対応する: 186→GPIO(0x40)、199→CLKPWR(0x20)。
I2C(0x60) / TSD(0x90) の minor は **チューナ電源が入ってから**開くので未取得（§5）。

## 3. ioctl コマンドの符号化（GPL ヘッダ）

`abst_lib.h` / `abst_lib<dev>.h`（FAPexK, GPLv2）で定義。すべて type=`0xFF`
（`FAPEX_DEV_TYPE`）の `_IO*` で、nr が「種別ベース＋機能」。arg は構造体ポインタ。

共通:

```
FAPEX_DEV_TYPE = 0xFF
_IO (0xFF, 0x00) OPEN     _IO (0xFF, 0x01) START   _IO (0xFF, 0x02) STOP
_IO (0xFF, 0x03) PAUSE    _IOW(0xFF, 0x04, u32) RESET   _IO (0xFF, 0x05) FORCE_RESET
```

種別ベース（nr の上位）: AUDEC=0x08 CLKPWR=0x20 **GPIO=0x40** **I2C=0x60** ICC=0x70
IR=0x80 **TSD=0x90** VIDEC=0xB8 VIENC=0xD0 VIOUT=0xD8 BOARDIFC=0xE0 FLASH=0xF0。

GPIO（`abst_libgpio.h`）:

```
_IOW(0xFF, 0x40, FAPEX_GPIO_OpenParamStrT{u32 pin; u32 function;})  GPIO_OPEN   (arg 8B)
_IOR(0xFF, 0x41, int32_t)                                           GPIO_READBIT(arg 4B)
_IOW(0xFF, 0x41, int32_t)                                           GPIO_WRITEBIT(arg 4B)
```

I2C（`abst_libi2c.h`）:

```
_IOW(0xFF, 0x60, FAPEX_I2C_OpenParamStrT)  I2C_OPEN
  OpenParam{u32 blockIndex; i32 speed(1=100k,2=400k); i32 protocol; u32 slaveAddress; i32 type; u32 timeOut;}
_IOW(0xFF, 0x61, int32_t)                  I2C_SET_PROTOCOL
_IOR(0xFF, 0x62, FAPEX_I2C_RWParamStrT)    I2C_READ
_IOW(0xFF, 0x62, FAPEX_I2C_RWParamStrT)    I2C_WRITE
  RWParam{u8 msSlaveAddress; u8* txDataPtr; u32 txDataLength; u8* rxDataPtr; u32 rxDataLength;}
```

`fapex_fdli2c_main.c`（GPL）の ioctl ハンドラが、この構造体を
`copy_from_user`（`FAPEXCFDLSAF_COPY_TO_DRV`）し、`txDataPtr`/`rxDataPtr` の指す
バッファも別途コピーしてから FAPI core の `Read/Write` を呼ぶ。**I2C_READ/WRITE の
実バイト列は構造体の中のポインタの先にある**ので、strace だけでは中身が出ない
（§4）。

## 4. 観察の壁と手段

- **strace は ioctl 番号と arg ポインタまで**しか出さない。I2C の送受信バイト列
  （どのスレーブ＝復調 CXD2818 / 地上 XC5200C にどの値を書くか）は
  `RWParam.txDataPtr` の先にあり、strace では読めない。
- 中身を取るには **`ioctl` を横取りする LD_PRELOAD トレーサ**（§3 の番号表で
  種別・機能を判定し、`RWParam`/`OpenParam` を辿ってバイト列を出す）を使う。
  OpenIRD の clean-room ツール `tools/fapex-trace`（実装済み）。番号表は GPL
  ヘッダ由来、ダンプ（proprietary）は動かすだけで参照しない。libc に非リンク
  （raw syscall ＋ `__errno_location` のみ）なので uClibc ツールチェーン不要、
  カーネルと同じ ARM クロス gcc で作る。`scripts/build-fapex-trace.sh`。
  GPIO の復号が GPL 仕様どおりであることを実機で確認済み（§2 の表）。

## 5. 実機での観察手順（再現用）

**前提**: OpenIRD 本番 rootfs（strace 入り）で起動、`ssh openird`。ベンダの
`rootfs.img`(mtd6) と `slot0.img`(mtd7) は所有者のダンプ。

1. **RAM だけのサンドボックスを組む**（`temp/parts` 相当を `/tmp/vendor` に置く）:
   ベンダ rootfs と slot を loop・ro で mount し、`/dev` は tmpfs に差し替えて
   **mtd/mtdblock と watchdog のノードを置かない**（flash とウォッチドッグに
   触らせない）。`fapi000`〜`fapi255`・null/zero/mem/random 等だけ作る。
   `/proc`・`/sys`・`/dev/pts` を bind。
2. `chroot /tmp/vendor/root … insmod` を §1 の順で。GPL の modinfo どおり
   `fapi_sys → fapi_core → fapex_cfdl → fapex_fdl{tsd,i2c,gpio,clkpwr}`。
   `lsmod`・`/proc/devices` に major 244 の `fapiNNN` が出る＝chardev 契約が立つ。
3. `strace -f -tt -o … chroot … dione_server <引数>` で観察:
   - `-v` … 版だけ出して exit（`01.00.0003`）。ハード非依存。
   - `-s` … `fapi186`（GPIO）を OPEN→READBIT でリセット SW を読む。
   - 無引数 … 本デーモン。CLKPWR init（fapi199）→ BoardControl スレッド →
     チューナ電源投入で `open("/dev/h57fmb0")` を試みる。

**チューナ電源は Dione(H58) 経由（重要）**: 無引数デーモンは
`WirelessDtvPower.c: start_device()` で `/dev/h57fmb0`（Dione の USB char dev）を
開いてチューナ電源を入れる。サンドボックスに Dione が無いと
`open("/dev/h57fmb0") = -1 (ENOENT)` で電源投入に失敗し、**その先の復調・チューナ
への I2C（fapi の I2C minor 0x60）に到達しない**（2026-09-17 実測、2 回）。

∴ 選局 I2C を観察するには **Dione を立ち上げる**必要がある: `usbcore`・
`musb_hdrc`・`nop-usb-xceiv`（slot 内、vermagic 一致）を insmod し、`h57fmb.ko` を
load、サンドボックス `/dev` に `/dev/h57fmb0` と実 `/dev/bus/usb` を渡す。次の
イテレーションで行う（USB/musb を本カーネルで動かす検証を伴う）。

**観察された落とし穴（重要）**:

- **ベンダデーモンはハードウェアウォッチドッグを起動する**（BoardControl）。
  無引数で起動したあとデーモンを kill すると、餌やりが止まって**十数秒後に実機が
  リセット**する（2026-09-17 実測）。観察を終えるときは正常終了させるか、リセット
  される前提で作業する。`/dev/wdtcounter` を tmpfs 側に置かなくてもハード側は動く。
- `dione_server` は mtd1（libooter env）と NVRAM を開こうとし、サンドボックスでは
  無いので «Could not open /dev/mtd1» を出すが、**続行する**（受信の主役ではない）。
- WiFi（`8192du.ko`）は本カーネルのシンボルに合わず insmod 失敗（受信に無関係）。

## 5.1 USB / Dione(H58) の立ち上げ（2026-09-18 実機）

チューナ電源が Dione(H58/USB) 経由（§5）なので、選局 I2C を見るには USB スタックを
自前カーネルで動かし、H58 を列挙させる必要がある。

**USB/MUSB は動く（達成）**: 自前カーネルは `CONFIG_USB=m`・`CONFIG_USB_MUSB_HDRC=m`・
`CONFIG_MUSB_MB86HXX=y`・`CONFIG_NOP_USB_XCEIV=m` をフルに持つ。ベンダの GPL モジュール
`usbcore` / `nop-usb-xceiv` / `musb_hdrc`（vermagic 一致）を **FAPex の後に**
insmod できる（`modules.dep`: `musb_hdrc` は `fapex_cfdl`/`fapi_core`/`fapi_sys` に
依存＝MUSB グルーが FAPex のクロック/電源を使う）。`musb_hdrc use_dma=0`。列挙結果:

| bus | デバイス | 用途 |
|---|---|---|
| 1-1 | `0bda:8171`（RTL8192DU） | WiFi |
| 2-1 | `05e3:0608`（GenesysLogic Hub, 4 port） | 内部ハブ H610-USB-2 |

**H58(`06b8`) は列挙されない＝USB-VBUS が OFF**: FAPex ボード設定
`mb86h61_dk01.c` に「`// NOTE: by default, USB-VBUS is off`」とあり、USB ポート電源は
**シフトレジスタ式ラッチ**（`GPO_SFTRG_USB_CBU0`=BIT6 / `GPO_SFTRG_USB_CBU1`=BIT7、
`SIpin/SOpin/CLKpin/LDpin` でシフト出力）で制御される。H58 はこのラッチで VBUS を
入れないとハブ配下（`2-1.x`）に現れない。ラッチを叩くのはベンダの board 初期化
（`FAPI_SYS_INIT_USE_BOARDS`）＝実運用では daemon の BoardControl。

**未解決＝daemon が sandbox で BoardControl 直後にハングする**: full daemon を
fapex-trace 下で走らせても、`Thread for BoardControl created`（board_control.c:1237）
の直後で停止し、GPIO ラッチ書き込みに到達しない（fapi199 CLKPWR init のみ）。
- ネットワークを stub しない（ベンダの `ifconfig`/`udhcpc`/`brctl` を素通し）と、daemon が
  eth0 をアプライアンス設定に切り替えて ssh が落ちる（AirTuner 独自 DHCP/bridge）。
- stub する（chroot の PATH 先頭に no-op を置く）と ssh は保てるが、上記の位置で
  ハングする＝daemon の board 初期化が stub した network 設定の結果に依存して待つ。
→ 次の一手の候補: (a) daemon を stub せず走らせ、制御をシリアル専用にして H58 が
  上がるまで待つ、(b) NVRAM(mtd3/4) を read-only で与えて daemon を正規パスに乗せる
  （mtd は read-only 限定・書き込みは絶対に通さない）、(c) ラッチのシフト手順を GPL の
  board コードから仕様化し、H58 電源投入だけを自前で行ってから daemon を動かす。

## 6. 段階 C（クリーン実装）への含意

- OpenIRD が満たすべき chardev 契約は **px4_drv 互換**（CLAUDE.md §4）。FAPex の
  `/dev/fapi/fapiNNN` はそのままでは px4_drv ではない。段階 C は、ここで観察した
  「復調・チューナへの I2C 手順」を仕様に起こし、px4_drv 互換 chardev の下に
  クリーン実装する。
- 当面はまず **I2C の実バイト列**（§4 のトレーサ）と、Dione 起動後に開かれる
  TSD の minor・ioctl 列を取る。生 TS が読める口（TSD の mmap リング）まで辿れば、
  段階 C の設計に足りる観察が揃う。

## 7. 参照

- GPL: `FAPI_FAPexK_V01L02R02_*`（カーネル薄層・ヘッダ）、`FAPI_FAPexU_V01L02R02_*`
  （ユーザ lib）。所有者が Pixela の GPL/OSS 提供で入手し `dl/` に置く（配布物には
  含めない）。
- 機種の物証・区画・侵入路は `temp/spec_SB-TV02-WFPL.md`（内部）。
