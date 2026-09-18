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
  （どのスレーブ＝復調 東芝 TC90532〔I2C0 slave 0x20〕/ 地上チューナ Xceive XC5200 にどの値を書くか。§5.5 で確定）は
  `RWParam.txDataPtr` の先にあり、strace では読めない。
- 中身を取るには **`ioctl` を横取りする LD_PRELOAD トレーサ**（§3 の番号表で
  種別・機能を判定し、`RWParam`/`OpenParam` を辿ってバイト列を出す）を使う。
  OpenIRD の clean-room ツール `tools/fapex-trace`（実装済み）。番号表は GPL
  ヘッダ由来、ダンプ（proprietary）は動かすだけで参照しない。libc に非リンク
  （raw syscall ＋ `__errno_location` のみ）なので uClibc ツールチェーン不要、
  カーネルと同じ ARM クロス gcc で作る。`scripts/build-fapex-trace.sh`。
  GPIO の復号が GPL 仕様どおりであることを実機で確認済み（§2 の表）。
- **注（§5.4 で確定）**: 復調器/チューナの**制御 I2C は FAPex I2C（`RWParam`, fapi `0x60`）で
  正しい**（当初の想定どおり）。実測で demod は **I2C ch0・slave `0x20`**（`fe_i2c.c`）。h57fmb0
  の USB トンネル（`fmb_hw_usb_ep_control_i2c`）は **Dione のファーム転送・TS 面**であって復調器
  レジスタ I2C ではない。トレーサは FAPex 0xFF の I2C 復号（既存）で demod bytes を拾える。h57fmb0
  の ioctl も拾えるよう拡張済み（ファーム転送の観察用）。詳細は §5.4。

## 5. 実機での観察手順（再現用）

> **確定サマリ（§5.1–§5.4 の結論。経緯と根拠は各節）**
> - **USB/MUSB は自前カーネルで動く**。ベンダ GPL の `usbcore`/`nop-usb-xceiv`/`musb_hdrc` を
>   FAPex の後に逐次 insmod（`use_dma=0`）。bus1=WiFi(`0bda:8171`)、bus2=内蔵ハブ(`05e3:0608`)。
> - **Dione H58 の電源投入はクリーンな GPIO で行う**（daemon 駆動は不安定）。ツール `tools/fapex-gpio`、
>   FAPex GPIO chardev(`fapi186`)。電源＝**77/78/81(DIONE 1.2/1.8/3.3V)＋48/49(復調器 1.2/3.3V)＋
>   74(reset)**。手順: 全 OFF →`77/81/78/49/48` ON → ~50ms → `74` 解除。→ H58 が `2-1.4=06b8:102d`
>   で列挙、`h57fmb` bind、`/dev/h57fmb0`(180:248)。(§5.3)
> - **FMB(h57fmb0, type=`0xae`)**: GET_INFO=`0x08`、GET_FACTOR=`0x11`。bootup で h57fmb0 ioctl
>   約 1500 回＝Dione ファーム(`mb86h57_h58_aac_*.bin`)転送。(§5.4)
> - **I2C**: `fapi174`=SoC I2C0、`fapi175`=SoC I2C1。**復調器の制御 I2C は SoC FAPex I2C(fapi `0x60`・
>   I2C ch0・slave `0x20`)経由**（h57fmb0 トンネルではない）。(§5.4)
> - **init 構造**: FW load → I2C0/1 open → GPIO で reset 解除(pin8=復調器/7=衛星/11=地デジ) → 復調器へ I2C。
> - **復調器アクセス＝解決（§5.5、実機確定）**: 復調器は I2C0/block0・slave `0x20`。非 ACK の真因は
>   **CLKPWR/クロックではなく Dione ゲート**だった。`fapex_fdlclkpwr` を**載せない**＝BoardControl
>   (setLed=SET_PWM 0x24) クラッシュ回避（§5.1 の壁が消滅）→ daemon が Dione を列挙（2-1.4 06b8:102d）
>   → **block0 が立ち 0x20 が read/write とも ACK**（実レジスタ値あり）。手段: 自作 `tools/fapex-i2c`。
> - **残り**: demod init レジスタ列の捕捉（tune 要求→`demod_WriteSomeRegs` を I2C pre-log）。§5.4(b)。
>   手動 GPIO 単独での Dione 起動は flaky（daemon の board init を使えば確実）。(§5.4/§5.5)

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
   （**`fapex_fdlclkpwr` を載せると daemon が BoardControl(setLed=SET_PWM) でクラッシュする**が、
   §5.5 で **復調器(MB86A22)は CLKPWR 非依存＝fdlclkpwr は不要**と判明したので、載せない＝
   クラッシュ回避で問題ない。Dione bring-up では `gpio`＋`i2c` のみ載せる＝§5.4/§5.5。）
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
load、サンドボックス `/dev` に `/dev/h57fmb0` と実 `/dev/bus/usb` を渡す。
**→ §5.1〜§5.3 で達成**（USB/musb は本カーネルで動作、Dione は自前 GPIO で電源投入、
`/dev/h57fmb0` open まで到達）。上の I2C minor 0x60 の想定は §4 の注・§5.3 参照。

**観察された落とし穴（重要）**:

- **ベンダデーモンはハードウェアウォッチドッグを起動する**（BoardControl）。
  無引数で起動したあとデーモンを kill すると、餌やりが止まって**十数秒後に実機が
  リセット**する（2026-09-17 実測）。観察を終えるときは正常終了させるか、リセット
  される前提で作業する。`/dev/wdtcounter` を tmpfs 側に置かなくてもハード側は動く。
- `dione_server` は mtd1（libooter env）と NVRAM を開こうとし、サンドボックスでは
  無いので «Could not open /dev/mtd1» を出すが、**続行する**（受信の主役ではない）。
- WiFi（`8192du.ko`）は本カーネルのシンボルに合わず insmod 失敗（受信に無関係）。

## 5.1 USB / Dione(H58) の立ち上げ（2026-09-18 実機）

> **訂正済み**: 本節は立ち上げ途中の推定を含み、一部（VBUS の原因＝シフトレジスタ・ラッチ
> という説明）は後の実機観察で**否定された**。確定した電源手順・結論は **§5.3** を正とする。
> 本節は「USB/MUSB が自前カーネルで動く」「daemon 駆動は不安定」という到達点として残す。

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

> **訂正（§5.3）**: この「VBUS がシフトレジスタ・ラッチで OFF だから列挙しない」という
> 原因説明は**誤り**だった。シフトレジスタ・ラッチ（`GPO_SFTRG_USB_CBU0/1`、shift pin
> `GP_XSI=11/XSO=38/XCK=39/XLD=40`）は **dev kit(dk01) 用**で、我々の `PCB_POE_BA300W_SB1`
> では pin 11 は地デジチューナ reset・38-42 は pull 固定＝**ラッチは使わない**。H58 が現れ
> なかった真因は **復調器電源 48/49 が OFF** だったこと（§5.3）。H58 は正しい電源手順を踏め
> ば VBUS ラッチ無しで列挙する。

**未解決＝USB を載せると daemon が board 初期化で間欠クラッシュ（2026-09-18）**:
full daemon はチューナ電源(§5)まで進めば H58 を上げるはずだが、次の板挟みがある。
- **USB モジュール未ロード**（2026-09-17 の最初の run）だと daemon は BoardControl を越えて
  `WirelessDtvPower`＝`/dev/h57fmb0` open まで進む。しかし h57fmb 未ロード＝H58 も未電源で
  ノードが無く、tuner 電源投入に失敗してそこで止まる。
- **USB モジュールをロード**（H58 に必要）すると、daemon の BoardControl/tuner 初期化が
  **タイミング競合でクラッシュ**する（`Thread for BoardControl created` 直後にプロセス消滅、
  fapi199 CLKPWR init のみ記録）。strace のオーバーヘッドを噛ませると**時々**越えて
  network 設定段まで進むが不安定で、越えても間もなく死ぬ。＝**ベンダの board/tuner 初期化
  コードが自前カーネルの USB スタックと相性が悪く、間欠的に落ちる**。
- 併発する厄介事: daemon は eth0 を予測不能に再設定する（bridge br0＝DHCP Server Mode で
  192.168.200.1/24＋udhcpd 192.168.200.2-254、または別アドレス）。観察は
  **実機⇄有線LAN⇄PC 直結＋PC 静的 192.168.200.100 で回避**でき、ssh は br0(192.168.200.1)へ
  張り直せる（`~/.ssh/config` の `openird-ap`）。ただし daemon が死ぬと観察が続かない。
- **ウォッチドッグ**: sandbox に `/dev/wdtcounter` を置かないと daemon は HW watchdog を
  arm できず（open ENOENT）、kill してもリセットしない（当初の「kill でリセット」は
  wdtcounter があった時の話）。

→ **daemon 駆動での H58 電源投入は不安定すぎる**（2026-09-18 の結論）。採った次の一手は
  **(c) H58 の電源投入だけを自前の小コードで行う**（→ §5.3 で達成。電源投入は VBUS ラッチでなく
  FAPex GPIO chardev で直接 GPIO を叩く）。daemon のクラッシュ・network 乗っ取りを避けられ、
  clean-room 的にも素直。H58 が上がって h57fmb が bind したら `/dev/h57fmb0` を作り、そこで
  ベンダ部品を最小限走らせて観察する。
  代替 (b): NVRAM(mtd3/4) を read-only で与えて daemon を正規パスに乗せる案は、上記の
  USB 相性クラッシュには効かない見込み（クラッシュは network/NVRAM でなく USB/tuner 初期化）。

## 5.2 電源/リセット GPIO マップ（GPL board コード由来・方式 C の設計図）

> **一部訂正済み**: 本節は GPL 静的解析による phase 1 の設計図。GPIO の意味づけ（表）は正しい
> が、**Dione 電源手順の推定は不完全**で（復調器 48/49 が抜けていた）、実機で **§5.3** に訂正
> された。確定手順は §5.3 を正とする。

方式 (c) の phase 1。GPL の FAPexK `config/boards/mb86h61_pdkv1/mb86h61_pdkv1_gpio.c`
（板 Model=`PIX-WLANTUNER`＝我々の板）の `BOARD_FMC_H61PDKV1_GpioConfig[]` に、板の
電源・リセット GPIO が**日本語コメント付き**で全部ある（shift_jis）。VBUS はシフト
レジスタ・ラッチではなく、**直接 GPIO** で制御される。

> **訂正（§5.3）**: 「daemon の `CtrlUsbPhy` は no-op」は誤り。`CtrlUsbPhy` は直接 GPIO
> `41/42`（`GPIO_USB_CBU0/1`）とラッチ bit を叩く。ただし **41/42 も 75 も Dione 列挙には不要**
> で、Dione 電源は `77/78/81 + 48/49 + 74` だった（§5.3）。

**重要**: `mb86h61_pdkv1_gpio.c` の GPIO テーブルは PCB 変種を `#if` で切り替える
（`PCB_DT270` / `PCB_POE_BA300W_SB1` / `PCB_POE_BA300W_SB1_SR1` / `#else`）。我々の
SB-TV02-WFPL（Wi-Fi＋Dione＋USB ハブ＋Ether、地デジ単）は **`PCB_POE_BA300W_SB1` 系の
マップと最も一致**する（`#else` は smart-card/SDIO の汎用 devkit で不一致）。ただし
Pixela が SB-TV02-WFPL 用にどの `#define` でコンパイルしたかは**未確定**＝実装前に
実機で 1 本ずつ確かめる（下記 phase 2）。

`PCB_POE_BA300W_SB1` マップの USB/Dione 関連（`FAPI_GPIO_OUT_1`=出力 High）:

| GPIO | 初期値 | 意味（コメント訳） |
|---|---|---|
| 5  | H | チューナ 3.3V（I2C1 が XC5200 に引かれるため H） |
| 36 | H | USB ハブ リセット解除 |
| 66 | H | イーサネット PHY リセット解除 |
| **72** | **H** | **USB ハイサイドスイッチ イネーブル**（USB 電源） |
| 74 | L | **DIONE リセット（L=リセット保持）** |
| 75 | H | DIONE FW 書き込みイネーブル |
| 77 | L | **DIONE 1.2V（初期 OFF）** |
| 78 | L | **DIONE 1.8V（初期 OFF）** |
| 81 | L | **DIONE 3.3V（初期 OFF）** |
| 48/49 | L | 復調器 1.2V / 3.3V（初期 OFF） |
| 7/8/11 | L | 衛星チューナ/復調器/地デジチューナ リセット（L=保持。地デジは 8/11） |

∴ **板初期化の時点では Dione 電源(77/78/81)は OFF・Dione リセット(74)は保持**。USB
ハブ(36)とハイサイドスイッチ(72)は ON なので、USB バス自体は生きる（実測で bus1 WiFi・
bus2 GenesysLogic ハブが列挙されたのと整合）。**H58 が現れないのは Dione の電源(77/78/81)
が OFF＋リセット(74)保持だから**。daemon の `WirelessDtvPower`（proprietary・GPL 非収録）
が後から電源 ON・リセット解除を行う。

> **訂正（§5.3）**: この「77/78/81→H、74→H を順に」という推定は**不完全**だった。実機で
> `WirelessDtvPower` の GPIO を fapex-trace で観察した確定手順は、**77/78/81 に加えて復調器
> 48/49 も ON**（全 OFF → `77→81→78→49→48` → ~50ms → 74 解除）。詳細は §5.3。

**phase 2（実装・達成 → §5.3）**: 上記 GPIO を自前で叩いて H58 を電源投入する。
- カーネルは **gpiolib/sysfs 非搭載**（`/sys/class/gpio` 無し、`CONFIG_GPIO_SYSFS` 無し）。
  → 叩く手段は (i) **FAPex GPIO chardev**（`GPIO_OPEN{pin,function=OUT}`＋`GPIO_WRITEBIT`。
  daemon が実際に使う経路。fapex-trace で確認済みだが GPIO_OPEN の function 符号化＝
  `FAPI_GPIO_OUT_1` の値と GPIO fapi minor の確定が要る）、または (ii) **/dev/mem＋GPL の
  GPIO レジスタマップ**（`arch/arm/mach-mb86hxx` / `reg_gpio.h`。vendor モジュール不要で
  最もクリーン）。
- 手順: FAPex GPIO を使うなら fapi_sys/core/cfdl/fdlgpio を load（USB/tuner に触れないので
  §5.1 のクラッシュは避けられる見込み）→ 72/36/5 確認 → 77/78/81→H → 74→H → H58 列挙を
  dmesg で確認 → h57fmb bind → `/dev/h57fmb0` を mknod → 選局 I2C 観察（fapex-trace）。
- **1 本ずつ実機で確かめる**（板変種未確定のため。誤った pin を叩かない）。

> **訂正（§5.3）**: 採ったのは **(i) FAPex GPIO chardev**。(ii) /dev/mem は「最もクリーン」で
> はなかった＝カーネル GPL の gpio.c はレジスタ構造しか与えず、「素の GPIO 出力 High/Low」の
> function 符号化は proprietary な `fapi_core`（`reg_gpio.h` は GPL 非収録）にあり、reverse せ
> ずには書けない。(i) はベンダの検証済み経路を使うので出力符号化を reverse せずに済み、clean-room
> 的にもこちらが素直。GPIO fapi minor は `fapi186`、function 値は GPL `abst_libgpio.h` から確定
> （OUT_1=0x0101 等、§5.3）。手順の pin は上の訂正どおり 48/49 を含む。

## 5.3 phase 2 実施結果（2026-09-18 実機）— Dione H58 をクリーンに立ち上げ

方式 (i)（FAPex GPIO chardev）で **H58 を自前カーネルにクリーンに列挙**させ、
`h57fmb` を bind、`/dev/h57fmb0` まで到達した。ツールは `tools/fapex-gpio`
（FAPex GPIO chardev を GPL の ABI で叩く自作 clean ツール。ベンダコード非依存）。

- **ツール要点**: minor は `fapi186`（major 244）。`GPIO_OPEN=_IOW(0xFF,0x40,{u32 pin;u32 fn})
  =0x4008FF40`、`READBIT=0x8004FF41`、`WRITEBIT=0x4004FF41`。fn: 出力 `OUT_0=0x0100`/
  `OUT_1=0x0101`、入力 `IN_GENERIC_SW=0x01010000`。すべて GPL `abst_libgpio.h`/`abst_lib.h`。
- **出力は board init 無しで効く（実機確認）**: `fdlgpio`（＋deps `fapi_sys/fapi_core/
  fapex_cfdl`）だけ load すれば任意 pin を OPEN で駆動できる（BoardIfc_Register は不要）。
  検証: pin 66（Ether PHY reset）を pulse すると `fmac3h Link DOWN`→`UP`＝出力が物理 pad に
  効く。副産物として **pin 66 がテーブルどおり＝我々は `PCB_POE_BA300W_SB1` 変種**と確認。
- **§5.2 の推定は不完全だった**。当初 77/78/81/74（＋41/42・75）を叩いても H58 が現れず。
  **fapex-trace で daemon の `WirelessDtvPower` の実 GPIO シーケンスを観察**して正解を確定:
  - 対象 pin = **77/78/81（DIONE 1.2/1.8/3.3V）＋ 48/49（復調器 1.2/3.3V）＋ 74（DIONE reset）**。
    **41/42（USB CBU）と 75 は不使用**。＝欠けていたのは **48/49 の復調器電源**。
  - 手順（すべて fapi186、`OPEN` は fn=`OUT_0` で開き `WRITEBIT` で値設定）:
    OFF 相 81→0,78→0,77→0,49→0,48→0,74→0(reset assert) → ON 相 **77→1,81→1,78→1,49→1,48→1**
    → ~50ms → **74→1(reset 解除)**。
  - **`OPEN` の fd を close しても pad 状態は保持**される（daemon は pin ごとに open/set/close を
    繰り返すが電源は入ったまま）。本ツールは `hold`/`seq` で fd を保持して明示的に維持する。
- **再現コマンド**（USB は `usbcore`/`nop-usb-xceiv`/`musb_hdrc use_dma=0` を逐次 load 済み前提）:
  ```
  fapex-gpio /dev/fapi/fapi186 seq \
    81:0 78:0 77:0 49:0 48:0 74:0 \
    w:77:1 w:81:1 w:78:1 w:49:1 w:48:1 s:50 w:74:1
  ```
  → **H58 が内蔵ハブ配下 `2-1.4 = 06b8:102d` で列挙**（h57fmb の bind ID の一つ。もう一方は
  `1221:201e`）→ `h57fmb` が interface bind → `/sys/class/usb/h57fmb0` の dev＝**major 180,
  minor 248** を読んで `mknod /dev/h57fmb0` → open OK。
- **ベンダスタックが Dione を認識**: daemon を（Dione 列挙済みの状態で）trace 下に再実行すると
  `Dione : VendorID=0x6b8` まで到達（`/dev/h57fmb0` 経由）。以前（USB 未ロード）は h57fmb0 が
  ENOENT で停止していた地点を越えた。§5.1 の「USB を載せると daemon がクラッシュ」は board/tuner を
  daemon 自身に初期化させた場合の話で、**GPIO で先に Dione を上げておけば回避できる**。

**次フェーズ（未了）= Dione FW ロード → フル bootup → 選局 I2C 観察**:
- Dione FW は slot 内 `/slot/firm/mb86h57_h58_aac_{idle,trc}.bin`（＋`spirom_writer_usb_pixela_102Dh.bin`
  ＝PID 102D）。daemon の `bootup()` がこれを Dione に流す。
- 現状 daemon の `bootup()` は `FMB_API_GET_FACTOR err=-1 errno=71(EPROTO)` で失敗。原因は
  (a) FW 未ロードの cold 状態、(b) daemon 自身の `WirelessDtvPower` が再度 74 を叩いて Dione を
  再列挙（`USB disconnect address3 → new address4`）させ、open 済み h57fmb0 fd が stale 化。
- **選局のトリガ**は Factory Port の passcommand 方針だが passcommand 未回収＝別途。
- **↑ この節の想定は §5.4 で更新**: 復調器の制御 I2C は h57fmb0 トンネルではなく **SoC の
  FAPex I2C（fapi 0x60・slave 0x20）**経由と実測で判明。h57fmb0 は Dione ファーム転送・TS 面。
  bootup 失敗の主因も (a)/(b) に加え「GET_FACTOR をリセット解除の ~10ms 後に発行＝ファーム
  未起動」であることが分かった。詳細は §5.4。

## 5.4 phase 2 続き（2026-09-18 実機）— Dione bootup とチューナ init I2C の観察

§5.3 で H58 を上げた後、ベンダ daemon を fapex-trace 下で走らせて **Dione の bootup と
チューナ初期化の構造**まで観察した。要点と、§5.3 で誤っていた I2C 経路の訂正。

**fapex-trace の拡張**（すべて自前・観察用、`tools/fapex-trace`）:
- **h57fmb0 の ioctl を黒箱ログ**（h57fmb は GPL 非収録＝proprietary。type/nr/dir/size と
  arg を `_IOC_SIZE` 分ダンプ）。
- **EPROTO retry シム**（既定 off、`/tmp/fapex-h57-retry` が **chroot 内**に在るとき有効。
  Dione ファーム起動待ちの EPROTO を吸収）。
- **FAPex I2C write を syscall 前に pre-log**（write は非 ACK slave でハングし得るので、
  ハングしても register bytes が残るよう `[pre-call]` 行を先に出す）。

**FMB ioctl（h57fmb0, type=`0xae`）**:
- `nr=0x08` GET_INFO（12B read）= `b8 06 / 2d 10 / 03`（VID 0x06b8, PID 0x102d, index 3）。
- `nr=0x11` GET_FACTOR（20B read）。**Dione ファームがリセット解除後に起動するまで EPROTO(-71)**。
  daemon は 74→1 の **~10ms 後**に問い合わせるので、device が安定していないと失敗する。
- bootup 成功時、**h57fmb0 の ioctl 約 1500 回で Dione ファームを流す**（slot 内
  `mb86h57_h58_aac_{idle,trc}.bin`）。

**I2C バス対応（実測）**: **`fapi174` = SoC I2C0、`fapi175` = SoC I2C1**。I2C_OPEN（`nr=0x60`,
24B）の OpenParam = blockIndex, speed(1=100k), …, timeout=0x1f4(500)。

**★ §5.3 の I2C 経路想定を訂正**: 復調器/チューナの制御 I2C は **h57fmb0 の USB トンネルでは
なく、SoC の FAPex I2C（`fe_i2c.c` → fapi `0x60`）経由**だった。daemon の `TunerInitialize`
→ `demod_WriteSomeRegs` が **I2C ch0・slave `0x20`・subaddr 0x01**（＝復調器レジスタ）へ書く。
＝fapex-trace の既存 I2C 復号（RWParam の tx/rx 追跡）でそのまま実バイトが取れる口。h57fmb0 は
Dione のファーム/TS 面、復調器レジスタは SoC I2C、と役割が分かれている。

**チューナ初期化の構造（観察）**: FW load（h57fmb0）→ I2C0/1 open（fapi174/175）→ GPIO で
reset 解除（pin 8=復調器, 7=衛星, 11=地デジ）→ 復調器へ I2C 書き込み（slave 0x20）。

**未解決の壁（次フェーズ）**:
1. **demod への I2C write がハング**（slave 0x20 が ACK しない）。当初クロック不足を疑ったが、
   **§5.5 の分析で復調器(MB86A22)は CLKPWR 非依存＝クロック無関係**と判明。真因は
   reset/バス/電源レール/タイミング、または front-end の Dione 経由給電（§5.5 で切り分け）。
2. **`fapex_fdlclkpwr` を load すると daemon が `BoardControl_setLed(CLKPWR)` でクラッシュ**
   （§5.1 の不安定性）。→ **§5.5 でジレンマは解消**: `setLed` = SET_PWM(`0x24`) はクロック
   START(`0x20`) とは別 ioctl で、復調器は CLKPWR 不要。∴ **`fdlclkpwr` を載せない＝クラッシュ
   回避、復調器に影響なし**。`fdli2c` だけで daemon は TunerInitialize まで進む。
3. **daemon の bootup が run 毎に不安定**: USB 再列挙のタイミングと DrvOpen の競合で、
   daemon が過渡状態の h57fmb0 を open→ **stale fd**（retry シムは「接続中で未起動」には効くが
   stale fd には無効）。hotplug 連動 node（`/tmp/hotplug.sh` を `/proc/sys/kernel/hotplug`）と
   retry で緩和するが、なお flaky。
- 診断ツール: `h57probe`（scratchpad、任意の type/nr/size/dir の ioctl を叩く）。安定した Dione
  では GET_INFO/GET_FACTOR とも成功する＝USB スタック自体は健全、を確認済み。
- **次の一手候補**: (a) 復調器クロック源を GPL/回路から特定し CLKPWR 依存なら fdlclkpwr の
  クラッシュ回避を探す **→ §5.5 で完了**（MB86A22＝CLKPWR 非依存、fdlclkpwr は不要と判明）、
  (b) demod write の pre-log で少なくとも init レジスタ列（意図）を捕捉、
  (c) daemon を介さず自前で FMB(type 0xae)＋FAPex I2C を叩いて init を再現（protocol は
  observe した bytes からの再構成＝clean-room）。

## 5.5 復調器のクロック源と fdlclkpwr クラッシュの分析（2026-09-18・GPL/回路）

§5.4 の壁 1/2（「復調器が非 ACK＝クロック不足の疑い」「CLKPWR を載せるとクラッシュ」の
ジレンマ）を、GPL ソース（FAPex U/K・カーネル board ファイル）と ioctl ABI だけから分析した。
**ベンダバイナリの逆アセは見ていない**（clean-room の「仕様化」ステップ）。

### 復調器の同定（★実機シルク写真で確定＝東芝 TC90532）
基板上の IC 刻印から確定した（当初の GPL 検出順からの MB86A22 推定は**誤り**だった）:
- **復調器 = 東芝 `TC90532XBG`**（ISDB-T OFDM ＋ ISDB-S 8PSK の復調器。TC90522 系）。
  **観測の 0x20〜0x23 の 4 アドレス ACK は、TC90522 系がサブモジュール（2×ISDB-T / 2×ISDB-S）ごとに
  別 I2C アドレスを持つ構造と一致**。daemon が書く slave `0x20` は地上サブモジュール。
  ＝reg `0x00`=`0x00`（MB86A22 の期待 `0x23` と不一致）だった理由＝そもそも MB86A22 ではない。
- **地上波チューナ = Xceive `XC5200`（XC5000 系シリコンチューナ）**。
- `D2818`（小型 QFN）は未確定だが **ISDB-S（BS/CS）RF チューナの公算**。
- GPL 板 `mb86h61_pdkv1` の FE 検出が `MB86A22_Detect` を最初に試すのは、単に空振りしていただけ
  （この板 GPL は我々の実基板の front-end とは別構成）。
- **★段階C への含意**: TC90532（≒`TC90522`）も XC5000 も **mainline Linux に GPL ドライバがある**
  （`drivers/media/dvb-frontends/tc90522.c`＝`CONFIG_DVB_TC90522`、`drivers/media/tuners/xc5000.c`）。
  日本の ISDB エコシステム（px4_drv / earthsoft PT3 系が TC90522 を使用）とも地続き。∴ クリーン実装は
  **ベンダコードでなく mainline GPL ドライバを仕様の出発点にできる**（clean-room に極めて有利）。

### TC90522/TC90532 サブアドレス方式の実機確認（2026-09-18）
mainline `tc90522.c` の `TC90522_MODULE_IDX(addr) = (addr & 0x02) >> 1`（**I2C アドレスの bit1 で
ISDB-T / ISDB-S サブモジュールを選ぶ**）を実機で検証した（Dione 起動状態で `fapex-i2c` により
0x20〜0x23 のレジスタ signature を比較）:
- **0x20 と 0x21 は同一 signature**（reg04=`02`, 0a=`04`, 0b=`44`, 0d=`a0`, 0e=`77`, 0f=`10`）
  ＝**モジュール0**。daemon が地上初期化で書いた slave `0x20` と一致＝**ISDB-T（OFDM）サブモジュール**。
- **0x22 と 0x23 は別の同一 signature**（reg01=`10`, 08=`10`, 0a=`ff`, 0b=`17`, 0d=`f2`, 0e=`f0`,
  0f=`50`）＝**モジュール1＝ISDB-S（8PSK）サブモジュール**。
- ＝bit1 がモジュール選択（bit0 は同一モジュール内）で tc90522.c と完全一致。
- **チューナは復調器の I2C ゲートウェイ（reg `0xfe`＝`TC90522_I2C_THRU_REG`）の奥**にある
  （直接スキャンで 0x20〜0x23 以外が出ない理由）。地上チューナ Xceive `XC5200` は 0x20 モジュールの、
  衛星チューナ `D2818`（ISDB-S 用と推定・正確な型番は公開情報に無し）は 0x22 モジュールのゲートウェイ経由。
- `fapi175`（SoC I2C1）は空（0 デバイス）。復調器は I2C0（`fapi174`）のみ。
- **段階C 手順**: `tc90522.c` を移植する際、地上=I2C0 slave `0x20`（0xfe ゲートウェイの奥に XC5000）、
  衛星=I2C0 slave `0x22`（同・D2818）。tuner init は `0xfe` 経由（`xc5000.c` はゲートウェイ i2c_adapter
  越しに叩く）。実機検証は `fapex-i2c`（`xfer` モードでゲートウェイ read 対応済み）。

### tuner ゲートウェイの実機確認（2026-09-18）— `fapex-i2c xfer` で 0xfe 越し
`fapex-i2c` に `xfer SLAVE RXLEN [TXBYTE...]`（write-then-read／pure write／pure read）を追加し、
TC90522 の 0xfe ゲートウェイ protocol（`tc90522_master_xfer`: tuner read = 復調器へ
`[0xfe, taddr<<1|1]` write → 復調器から N バイト read。要 repeated-start＝`--restart`）を実機検証:
- **衛星チューナ（D2818）= I2C 7-bit `0x60`、復調器 ISDB-S サブモジュール `0x22` のゲートウェイの奥で
  応答**（`xfer 0x22 … 0xfe 0xC1` → `05 26`）。レジスタ署名（前段電源 pin5 ON＋reset 解除で読取）:
  reg00=`05`, 01=`26`, 02=`40`, 03=`00`, 08=`df`, 0a=`10`, 10=`0f`。
  **Sharp QM1D1C0042/0045 ではない**（QM1D1C004x は reg00=`0x48`/`0x68`。実機は `0x05`）。正確な型番は
  silk `D2818` のまま未確定だが、**I2C 0x60・上記署名**まで確定。
- **地上チューナ（XC5200／XC5000 系）は 0x20 ゲートウェイの奥で未応答**（0x08〜0x77 を bare read も
  XC5000 流の reg-ptr read も gateway scan しても出ない）。追加で:
  - **ISDB-T サブモジュール自体は制御可**: `tc90522.c` の wakeup（reg `0x01`=`0x00` で ter reset 解除、
    reg `0x03`=`0x80` で ter wakeup）を fresh `fapex-i2c` で書くと reg03 が `0x00`→`0x80` に変わる
    （demod 0x20 は read/write とも可）。＝daemon がハングした「0x20 reg 0x01=0x00」は ter reset 解除で
    tc90522 の wakeup 前段と整合。
  - **それでも 0x20 ゲートウェイの奥に tuner が出ない**＝**XC5000 が未給電**。`pin5`(tuner 3.3V)＋
    `pin11`(地上 reset、**両極性 0/1 とも試行**)＋`pin7` 解除 では地上チューナは立たない
    （同条件で衛星チューナ 0x60 は応答する＝pin5 は衛星側を給電）。
  - **地上チューナの電源レールは未特定**。daemon の実 GPIO トレース（ハングまで）は Dione(77/78/81/49/48/74)＋
    reset 解除(7/8) のみで、**pin5/pin11 も地上 tuner power も含まれない**＝地上 tuner init は demod write
    ハングの先（未観測）にある。XC5000 は FW ロードも要る。
- ＝**ゲートウェイ機構と `fapex-i2c xfer` は実機で動作確認済み**（衛星で実証）。段階C の tuner 検証は
  この口で行えるが、**地上パスは demod write ハングの解消（＝daemon の地上 tuner init 観察）か回路図が要る**。

### 段階C 完成への鍵＝demod write ハングの解消
今回の地上チューナ調査で、段階C 完成の鍵が明確になった:
- **mainline `tc90522.c` の init は「非公開ゆえ不完全」**（ドライバ自身が明言）。demod の完全な init
  （`demod_WriteSomeRegs` 相当）と**地上 tuner の電源/init 手順**は、ベンダ daemon だけが持つ。
- その daemon は **demod への最初の I2C write（0x20 reg 0x01=0x00）で D-state ハング**（§5.5 上述。
  fresh ハンドルなら同じ write は通るのに daemon 自身のハンドルだけハング）。∴ **このハングを解消できれば、
  daemon が demod 全 init ＋ 地上 tuner power/init まで進み、`fapex-trace` で全列を観察できる**＝段階C の
  仕様が一気に揃う。次フェーズの最重要ターゲット。

### クロック源
- 復調器のクロックは CLKPWR 非依存（以下）。この結論はチップ種別に依らず成立する（TC90532 も水晶
  参照のデジタル復調器）。
- MB86A22 の **PLL 参照クロックはチップ自身の水晶/参照クロック**（`MB86A2x_REFSEL` =
  4/16/26/32MHz を選ぶ内蔵 PLL。ペアの FC2581 が `FREQ_XTAL=16MHz` なので 16MHz が濃厚）。
  **SoC の CLKPWR ではゲートされない**。
- FE の reset/power（pdkv1 `ControlFeResetPower`）は **GPIO のみ**（`RESET_PULSE` =
  active 50ms → release 50ms）で、**クロック有効化を含まない**。

### CLKPWR は復調器のクロック源ではない
- CLKPWR は **SoC 内部**のクロック/電源ゲート専用。マスク（`abst_libclkpwr.h`）は
  ARM/AUDIO/AXI/DDR2/DMA/ETH/**I2C(bit7)**/GPIO/SFLASH/SSP/**TSD(bit13)**/UART/USB/VIDEO…で、
  **外部チップ（復調器/チューナ）の参照クロックビットは存在しない**。phys `0xCA000000` →
  VA `0xFF60A000` に board ファイルが静的マップ（`iotable_init`）する。
- SoC の I2C コントローラのクロック（CLKPWR bit7）は**既に稼働**（§5.3/§5.4 でバス open 済み・
  USB 列挙済み）。
- **∴ 復調器（MB86A22）を I2C で ACK させるのに `fapex_fdlclkpwr` は不要**。§5.4 壁 2 の
  「復調器クロックに CLKPWR が要る」という前提が**誤り**＝ジレンマは成立しない。

### fdlclkpwr クラッシュの正体（分析）
- CLKPWR の ioctl 番号（NB_CLKPWR=`0x20`）: **START(PowerUp)=`0x20`**・STOP(PowerDown)=`0x21`・
  GET_STATUS=`0x22`・WATCHDOG_START=`0x23`・**SET_PWM=`0x24`**・GET_PWM=`0x25`。
- **`BoardControl_setLed` = SET_PWM(`0x24`)**（LED を PWM で駆動）＝クラッシュ箇所。
  クロック/電源の START(`0x20`) とは**別 ioctl**。§5.4 の起動時トレース `nr=0x21 in=06 00` は
  **STOP**（AUDIO|AUDIO_PROCESSOR の電源断）であって START ではない。
- OUR カーネルは **`CONFIG_MB86H61_WATCHDOG` 未設定**（`.config` で確認）＝HW watchdog 不使用。
  FDL の WATCHDOG_START ハンドラの `stop_watchdog_ext()` はこの config ではコンパイルされず、
  watchdog リセットも起きない＝watchdog はクラッシュ原因ではない。
- FDL（`fapex_fdlclkpwr_main.c`）はグルーのみ。実レジスタ操作 `FAPI_CLKPWR_*`（SetPwm 含む）は
  **proprietary な fapicore（`fapi_core.ko`）側**。カーネルツリーに `drivers/media/fapex` は無い
  ＝ソース非公開。SET_PWM は PWM レジスタ（CLKPWR VA + offset）への書き込みで、静的マップが
  我々の board ファイルで作られていれば本来 fault しない。→ クラッシュが kernel oops か
  userspace segfault かは**ハードで特性化が要る**（候補: PWM レジスタ配置差／daemon の setLed 戻り値
  誤処理／fapicore の CLKPWR ctx open 不整合）。

### 回避策（結論）
- **復調器は CLKPWR に依存しないので、`fapex_fdlclkpwr` を load しない＝BoardControl_setLed を
  通さない＝クラッシュ回避、で復調器には一切影響しない**。Dione bring-up は既に `gpio`+`i2c`
  のみで成立（§5.3）。
- 万一 SoC 内部クロックの明示 enable が要る場面が出たら、**CLKPWR START(`0x20`) を自前の小ツール
  （`fapex-gpio` 相当）で直接叩き、SET_PWM(`0x24`) は絶対に呼ばない**、で分離できる。

### 復調器は自前クロックで独立（検出順から確定）
GPL の `MB86A22_FC2581_Detect`（`mb86a22.c`）の検出順が決定的:
1. demod の I2C アドレス設定（0x20 / 0x24）
2. `MB86A22_Reset(slot)`（HW リセット。`feResetFct`→`FE_ControlFeReset(FE_RESET_PULSE)`→board hook
   →**GPIO reset**。pdkv1 は `GPIO_OP_FE_RES_A/B=12/26`、active-low、active50ms→release50ms）
3. **demod のチップ ID レジスタ `0x00` を I2C read**（`0x23`=MB86A22 / `0x13`=MB86A20S）
4. その後にチューナ（RF）アドレスを ping（0xA8…）

**demod の ID read はチューナに触れる前**に成功する必要がある＝**復調器は自前の参照クロック
（水晶）で独立して動く**。チューナ電源（"Dione: Tuner Power"）や tuner の有無は demod の ACK に
無関係。

### 真の非 ACK 原因（次に切り分ける）
クロック・チューナ・Dione ではなく、**復調器自身の reset / 電源レール / バス / タイミング**に絞れる:
- **reset**: 観測の pin8=復調器 reset が実際に MB86A22 の reset か、極性（active-low）・パルス長
  （~50ms×2）・解除が正しいか（pdkv1 GPL は 12/26 なので pin は要再確認）。
- **電源レール**: 48/49 が本当に MB86A22 の 1.2/3.3V か、他レールが要らないか。
- **バス**: demod が I2C ch0（fapi174）か ch1（fapi175）か。
- **タイミング/速度**: reset 解除後の settle、I2C 速度（100k）。
- **次の実機テスト**: 復調器（48/49 電源＋reset 解除）の後、**`fdlclkpwr` 無し**で I2C `0x20` の
  **レジスタ `0x00` read**（期待値 `0x23`）を自前の小ツールで叩く。ACK すればクロック無関係が
  確定＝あとは init レジスタ列の観察（§5.4 (b)/(c)）へ。非 ACK なら上の reset/レール/バス/タイミング
  を順に当たる。

### 実機検証（2026-09-18、§5.5 のテスト実施）
自前の clean な I2C ツール `tools/fapex-i2c`（FAPex I2C chardev の GPL ABI を叩く。OPEN=`0x4018FF60`・
READ=`0x8014FF62`・WRITE=`0x4014FF62`、`read/write/scan`。非 ACK でハングしないよう OpenParam の
`timeOut` を設定）を作り、実機で検証した。手順: `fapi_sys/fapi_core/fapex_cfdl/fapex_fdlgpio/
fapex_fdli2c` を insmod（**`fdlclkpwr` は載せない**）→ `fapex-gpio` で FE 電源を GPIO 投入
（`5/72/36 + 77/78/81 + 48/49` ON、`74/7/8/11` reset 解除）→ `fapex-i2c` で demod を read/scan。

**結果（重要）**:
- **CLKPWR/クロックは無関係が実証された**: `fdlclkpwr` を一切載せずに I2C は正常動作し、
  **block2（I2C2）で実デバイス 2 個（slave `0x72`/`0x73`, reg0=`0x01`）を検出**＝ツールと ABI は
  正しく、I2C コントローラのクロックも生きている（§5.5 の分析を実機が裏付け）。
- **復調器 `0x20` は I2C block0/block1 のどこにも ACK しない**（フル FE 電源 GPIO 投入下でも）。
  block0/block1 のスキャン（0x03..0x77）は 0 デバイス。
- **★ バス選択は OpenParam の `blockIndex` で行われ、chardev minor ではない**（`fapi174/175/176/177`
  のどれで開いても、block2 を指定すれば `0x72`/`0x73` が出る。block0/1 はどの minor でも空）。
  ＝§5.4 の「`fapi174`=I2C0/`fapi175`=I2C1」は正しくは「daemon が **OpenParam.blockIndex** で
  I2C0/I2C1 を選んでいる」。minor はハンドルにすぎない。
- 実機に **kernel oops なし**（`fdlclkpwr` を載せないので §5.4 壁2 のクラッシュも当然起きない）。

**解釈と次**: GPIO 電源だけでは前段バス（I2C0/block0）が電気的に立たない＝復調器が現れない。
§5.4 で daemon の demod write がハングしたのも整合（demod がそもそも応答していない）。∴ 復調器の
活性化レバーは GPIO 電源ではなく、**Dione の完全起動**で前段モジュール（復調器）が有効化される公算。

### ★ Dione 完全起動 → block0 再スキャン（2026-09-18、確定）
手動 GPIO では Dione が列挙しなかった（§5.4 の flaky。VBUS 41/42 やハブ再 bind も無効）。そこで
**ベンダ daemon 自身に board init を任せた**（`fdlclkpwr` は載せない＝task(a) の回避策）。結果:
- **daemon は BoardControl を越えて起動**（§5.1 のクラッシュは起きない＝**task(a) の fix を実機実証**）。
  `dione server is ready to work` に到達し、`WirelessDtvPower` で **Dione を列挙・認識**
  （`usb 2-1.4: 06b8:102d`、stdout `Dione : VendorID=0x6b8`）。手動 GPIO で出せなかった Dione が、
  daemon のフル board init（CtrlUsbPhy 等）で確実に上がる。
- トレース（`fapex-trace`）で確認した bring-up: **I2C_OPEN fapi174=blockIndex0 / fapi175=blockIndex1**
  （speed=1=100k, timeout=0x1f4=500）、`WirelessDtvPower` の GPIO（81/78/77/49/48→0 then →1、74 解除）、
  demod/衛星 reset 解除（pin `0x08`/`0x07`）。
- **daemon 停止後（Dione は列挙されたまま、GPIO pad は persist）に `fapex-i2c` で block0 を再スキャン**:
  **slave `0x20` が read/write とも ACK**（以前は空）。レジスタは実在の値
  （reg04=`02`, reg28=`60`, reg70=`18`, reg80=`af` と非ゼロで変化）＝スプリアスでない本物の I2C デバイス。
  0x20〜0x23 が ACK。※ reg0 は `0x00`（dev-kit MB86A22 の chip-ID 期待値 `0x23` ではない）＝未初期化
  状態か、この基板の chip-ID レジスタ/手順が dev-kit と異なる。init は tune 時の `demod_WriteSomeRegs`
  で入る見込み（§5.4 (b) で捕捉）。
- **結論（task(a) をエンドツーエンドで確定）**: 復調器の非 ACK は **CLKPWR/クロックではなく Dione
  ゲート**。①`fdlclkpwr` を載せない→BoardControl クラッシュ回避→②daemon が Dione を列挙→
  ③block0 が立ち復調器 0x20 が応答。§5.4 壁1・壁2 とも解消。

### demod init 捕捉の試行（2026-09-18）— FW load 突破と最初の demod write、残る timing 壁
`fapex-trace` を bring-up シムで強化して daemon の bootup を通し、demod init を捕捉しにいった。

**tracer の追加シム**（`tools/fapex-trace`、既定 off・観察専用ではない bring-up 用）:
- **h57fmb0 rebind シム**: FMB ioctl が `ENODEV(-19)` を返したら、その fd のパスを開き直して
  `dup2` で同じ fd 番号に貼り替えてから retry。**Dione は FW 起動時に一度自己再列挙する**
  （USB disconnect addr N → reconnect addr N+1）ため、daemon の既存 h57fmb0 fd が stale になり
  `ENODEV` になる。これを透過的に吸収する。従来の `EPROTO(-71)` retry（FW 起動待ち）と併用。
  （`/tmp/fapex-h57-retry` で有効化。§5.4）
- **h57 quiet モード**: `/tmp/fapex-h57-quiet` があると h57fmb ioctl のログ出力を抑制
  （シム/passthrough は維持）。FW 転送 ~1500 回のログ I/O を省く。

**結果（実機）**:
- 上記シムで **daemon の Dione FW load が完遂**（h57fmb0 ioctl ~1500、`GET_INFO`=VID `06b8`/PID
  `102d`、`GET_FACTOR` は EPROTO→自己再列挙 ENODEV を rebind で突破）。§5.4 の「bootup flaky／
  stale fd」を rebind シムで解消。
- 続いて **demod init の最初の I2C write を捕捉**:
  `fapi174(block0) nr=0x62 W i2c_slave=0x20 tx=[2]:01 00`（＝復調器 reg `0x01` に `0x00`）。
- **ただしこの write が D-state で無限ハング**（`kill` でも即死なず＝カーネル内でブロック）。直後に
  daemon の bootup が中断してプロセス終了。以降の `demod_WriteSomeRegs` は取れなかった。
- **原因（timing 壁）**: 復調器の I2C バス（block0）は Dione にゲートされており、daemon は
  **FW load 直後（Dione が post-FW 再列挙中で block0 が一時的に死んでいる窓）**に demod へ書くため
  ACK されずハングする。`fapex-i2c` が成功したのは **Dione が安定列挙した後**（settle 後は 0x20 が
  read/write とも ACK、reg04=`02` 等）。＝ハードな失敗ではなく **タイミングゲート**。

### ★ demod write ハングの切り分け（2026-09-18、確定）— settle(ii) を試した結論
(ii)「demod write 前に settle 待ちを注入」を tracer に実装（`/tmp/fapex-i2c-settle` ms、
最初の slave 0x20 write の直前に sleep）。さらに I2C0 ハンドルを open+I2C_OPEN+`dup2` で貼り替える
実験もした。**いずれも daemon の demod write ハングを解消せず**。切り分けの結果:
- settle=2000/4000/10000ms でも daemon の write はハング（daemon の死亡時刻が settle 分だけ後ろへ
  ずれる＝write 到達→ハング→死。固定 watchdog ではない）。→ **timing ではない**。
- daemon が demod write でハング中（プロセス生存）に、**別プロセスの fresh `fapex-i2c` で同一 write
  （`0x20` reg `0x01`=`0x00`）も read（reg04=`02`）も成功**（rc=0）。→ **バス contention でも demod
  故障でもない**。block0 は生きていて、fresh ハンドルなら並行で読み書きできる。
- ∴ **ハングは daemon 自身の `fapi174` ハンドル/コンテキスト固有**（Dione 再列挙後にそのハンドルの
  write だけがカーネル内で無限ブロック。fresh ハンドルは正常）。tracer から dup2 で貼り替えても
  daemon の write は解消しなかった＝原因は proprietary な fapi_core の per-handle 状態と推定。

### ★ Milestone 2 にとっての結論（重要）
**復調器（I2C0/block0・slave `0x20`）は clean な `fapex-i2c` で read/write とも完全に制御可能**
（daemon がハング中でも並行アクセス可）。＝**段階Cのクリーン実装は、ベンダ daemon 無しで復調器を
直接駆動できる**。必要なのは「どのレジスタ列を書くか」の知識で、それは GPL に手がかりがある
（`fapex_subsystems/fe/mb86a22/` の `MB86A22_API.c` 等）。∴ daemon から init 列を丸取りする必要性は
低い（daemon 観察は板固有パラメータ＝xtal/IF/tuner 種別の確認用）。

**復調器チップの同定＝確定（実機シルク写真）**: 復調器は **東芝 `TC90532XBG`**（TC90522 系の
ISDB-T/S 復調器）。実機 slave 0x20 の reg `0x00`=`0x00`（MB86A22 の `0x23` と不一致）だったのは
MB86A22 ではないため。0x20〜0x23 の 4 アドレス ACK は TC90522 系のサブモジュール別 I2C アドレスと
一致。register dump（Dione 起動後、参考）: reg04=`02`, reg0a=`04`, reg0b=`44`, reg0d=`a0`,
reg0e=`77`, reg0f=`10`。地上チューナ = Xceive `XC5200`（XC5000 系）。`D2818` は ISDB-S チューナと推定。
段階Cは mainline の `tc90522.c` / `xc5000.c`（GPL）を出発点にできる（上記「復調器の同定」参照）。

### 未解決・次
- **demod init 列の入手**: チップ同定は完了（TC90532＝TC90522 系／XC5000）。(a) **mainline GPL の
  `drivers/media/dvb-frontends/tc90522.c` と `drivers/media/tuners/xc5000.c` を仕様の出発点**にし、
  `fapex-i2c`（I2C0 slave 0x20）で実機検証する（clean-room 順序: 仕様→実装→検証）。ベンダ daemon の
  init 列を丸取りする必要はない。(b) daemon から確認するなら（板固有の xtal/IF/接続）、demod write
  ハングを避ける経路（tune 要求＝control port プロトコル特定、または daemon のハンドル問題の回避）が要る。
- FAPex I2C ドライバは **非 ACK/特定条件で無限ブロック**（timeout=500 が効かない場面）＝観察時は
  D-state ハングに注意。`fapex-i2c` は OpenParam timeout＋`busybox timeout` ラッパで回避。
- `0x72`/`0x73`（I2C2, reg0=`0x01`）の同定は別途。

## 6. 段階 C（クリーン実装）への含意

- OpenIRD が満たすべき chardev 契約は **px4_drv 互換**（CLAUDE.md §4）。FAPex の
  `/dev/fapi/fapiNNN` はそのままでは px4_drv ではない。段階 C は、ここで観察した
  「復調・チューナへの I2C 手順」を仕様に起こし、px4_drv 互換 chardev の下に
  クリーン実装する。
- 当面はまず **復調器の I2C 実バイト列**（§4 のトレーサ。**復調器は SoC FAPex I2C fapi 0x60・
  slave 0x20**＝§5.4）と、Dione 起動後の TS 出力口の ioctl/mmap 列を取る。生 TS が読める口まで
  辿れば段階 C の設計に足りる観察が揃う。**TS の口が FAPex TSD（fapi 0x90）か h57fmb0 の mmap
  （`fmb_hw_usb_mmap`）かは次フェーズで確定**（Dione 経由なら後者の可能性）。

## 7. 参照

- GPL: `FAPI_FAPexK_V01L02R02_*`（カーネル薄層・ヘッダ）、`FAPI_FAPexU_V01L02R02_*`
  （ユーザ lib）。所有者が Pixela の GPL/OSS 提供で入手し `dl/` に置く（配布物には
  含めない）。
- 機種の物証・区画・侵入路は `temp/spec_SB-TV02-WFPL.md`（内部）。
