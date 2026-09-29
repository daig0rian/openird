# FAPex ユーザランド ↔ カーネルの契約（SB-TV02-WFPL / 段階 B の観察）

<!-- openird-spec
Status: observing
As-of:  実機 SB-TV02-WFPL / 段階 B の観察 (継続中)
Impl:   -
Tests:  -
Note:   段階 C（ird-tuner-*）の契約になる予定。観察が続いている
-->

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
> - **FMB(h57fmb0, type=`0xae`)**: GET_INFO=`0x08`、GET_FACTOR=`0x11`。~~bootup で h57fmb0 ioctl
>   約 1500 回＝Dione ファーム転送~~ ← **2026-09-20 に訂正**。ioctl はレジスタアクセスと制御で、
>   ファーム本体は `write()` で 512B ずつ流れる（計 1,092,480 B＝idle+trc）。§5.5 の「Dione の
>   ファーム転送は ioctl ではなく write()」。
> - **I2C**: `fapi174`=SoC I2C0、`fapi175`=SoC I2C1。**復調器の制御 I2C は SoC FAPex I2C(fapi `0x60`・
>   I2C ch0・slave `0x20`)経由**（h57fmb0 トンネルではない）。(§5.4)
> - **init 構造**: FW load → I2C0/1 open → GPIO で reset 解除(pin8=復調器/7=衛星/11=地デジ) → 復調器へ I2C。
> - **front-end 同定（実機写真、§5.5）**: 復調器 = 東芝 **TC90532**（TC90522 系。I2C0 slave `0x20`/`0x21`=
>   地上 ISDB-T、`0x22`/`0x23`=衛星 ISDB-S）。チューナは復調器の `0xfe` ゲートウェイの奥（地上=Xceive
>   **XC5200**、衛星=**D2818**＠`0x60`）。**2026-09-29 訂正（§5.7）**: 地上の XC5200 は SoC I2C1 の `0xc2` に直結で、
>   ゲートの奥にいるのは衛星チューナだけ。段階C の仕様源＝mainline `tc90522.c`/`xc5000.c`。
> - **復調器アクセス＝解決（§5.5、実機確定）**: 復調器は I2C0/block0・slave `0x20`。非 ACK の真因は
>   **CLKPWR/クロックではなく Dione ゲート**だった。`fapex_fdlclkpwr` を**載せない**＝BoardControl
>   (setLed=SET_PWM 0x24) クラッシュ回避（§5.1 の壁が消滅）→ daemon が Dione を列挙（2-1.4 06b8:102d）
>   → **block0 が立ち 0x20 が read/write とも ACK**（実レジスタ値あり）。手段: 自作 `tools/fapex-i2c`。
> - **~~手動 GPIO 単独での Dione 起動は flaky~~ → 2026-09-20 に解決**。自前ツールだけで
>   確実に USB 列挙まで持ち込める。鍵は (a) CLKPWR を同一 fd で `0x00`→`0x21`→`0x23`(値渡し
>   `0x3000`) と出して**開いたまま保持**、(b) GPIO は**1 操作ごとに open/close**する
>   （ベンダもそうしており、close してもピン状態は保持される）。手順は §5.5 の
>   「自前ツールで Dione を USB 列挙まで起動できた」。
> - **残り**: Dione の**ファーム転送**（`/slot/firm/mb86h57_h58_aac_*.bin`＝所有者の機器上の
>   blob）と、その転送路 `h57fmb.ko` が GPL 非収録の proprietary ドライバである点。復調器が
>   I2C で ACK するにはファームがロードされて動いている必要がある（それが「block0 が立つ」）。
> - **残り**: demod init レジスタ列の捕捉（tune 要求→`demod_WriteSomeRegs` を I2C pre-log）。§5.4(b)。

## 5.0 観察の土台：観測 rig（2026-09-20 以降はこちらが本線）

**現在の観察手段は `docs/specs/sb-tv02-wfpl-vendor-boot.md` §6 の観測 rig** である。
ベンダの rootfs と slot をフラッシュに置き、起動スクリプト 2 本だけを差し替えて
**ベンダアプリだけを自動起動しない**状態を作る。純正の環境がそのまま揃うので、下記
サンドボックスに付きまとっていた副作用（`fapex_fdlclkpwr` を外さねばならない、`/dev` に
mtd が無い、NVRAM が無い）が最初から無い。ビルドは `scripts/build-vendor-rig.sh`、
使い方は `tools/vendor-rig/README.md`。観察ツール（`fapex-trace`/`-ts`/`-i2c`/`-gpio`/`-poke`）は
rig の `/slot` に焼き込まれている。

以下 §5 の chroot サンドボックス手順は **rig より前（2026-09-17〜18）の方法**で、経緯と
そこで得た知見のために残す。いま同じ観察をするなら rig を使う。

**（以下、旧サンドボックス手順）前提**: OpenIRD 本番 rootfs（strace 入り）で起動、
`ssh openird`。ベンダの `rootfs.img`(mtd6) と `slot0.img`(mtd7) は所有者のダンプ。

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

## 5.5 復調器/チューナの同定・I2C 構造・demod init（2026-09-18）

> **確定サマリ（この節の現時点の結論。以下の各小節は調査の時系列で、初期は復調器を MB86A22 と
> 推定していたが実機写真で TC90532 と訂正。クロック非依存の結論はチップに依らず維持）**
> - **復調器 = 東芝 TC90532XBG（TC90522 系、ISDB-T/S）**。実機写真で確定（MB86A22 ではない）。
>   I2C0 の **slave `0x20`/`0x21`=ISDB-T、`0x22`/`0x23`=ISDB-S**（bit1 でサブモジュール選択。tc90522.c 準拠）。
> - **チューナは復調器の I2C ゲートウェイ（reg `0xfe`）の奥**。地上 = Xceive **XC5200**（0x20 の奥、
>   現状未給電で未応答）、衛星 = **D2818**（I2C `0x60`、0x22 の奥、応答確認済み）。
> - **fdlclkpwr は復調器に不要**（復調器は CLKPWR 非依存）。載せなければ BoardControl クラッシュも回避。
> - **復調器の非 ACK の真因 = Dione ゲート**（GPIO 電源だけでは前段バスが立たず、daemon で Dione を
>   完全起動すると block0 が立ち 0x20 が read/write 可）。当初の「クロック不足」「reset/レール」説は**否定**。
> - **demod write ハング = daemon プロセス固有**（別プロセスの fresh ハンドルなら同一 RW が通る。
>   in-process proxy でも解けず）。**段階C の必須ブロッカーではない**（mainline `tc90522.c`/`xc5000.c`
>   を移植し復調器は standalone プロセスから直接検証できる）。

§5.4 の壁 1/2（「復調器が非 ACK＝クロック不足の疑い」「CLKPWR を載せるとクラッシュ」の
ジレンマ）を、GPL ソース（FAPex U/K・カーネル board ファイル）と ioctl ABI だけから分析した
（初期分析。**ベンダバイナリの逆アセは見ていない**＝clean-room の「仕様化」ステップ）。

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

### demod write ハング＝「daemon プロセス固有」（2026-09-18、proxy 実験で確定）
ハング解消のため `fapex-trace` に **proxy シム**（`/tmp/fapex-i2c-proxy`）を実装した: daemon の
FAPex I2C RW を、daemon 自身の fd ではなく **tracer が open し直した fresh ハンドル**で代理実行し
（daemon の I2C_OPEN param を捕捉して再適用）、結果を返す。診断ログ付き。結果:
- proxy は正しく発火（`[proxydiag] en=1 fd=18 opset=1`、`[proxy] nfd=21 i2copen=0`＝fresh open も
  I2C_OPEN も成功）。**しかしその fresh fd への RW も同じくハング**した。
- ＝**ハングは fd/ハンドル固有ではなく「daemon プロセス固有」**。同一 RW が**別プロセス**（standalone
  `fapex-i2c`）なら通るのに、**daemon プロセス内では fresh ハンドルでもハング**する。in-process の
  proxy では回避不能＝**cross-process 委譲（別プロセスで I2C を実行）でなければ解けない**。
- 状況証拠: ハングは FW load 直後（Dione が post-FW 再列挙中）に起き、settle(最大 10s)でも解けず、
  daemon は h57fmb0(USB)を開いたまま。＝**USB(Dione)↔I2C0 のプロセス依存干渉**の疑い（daemon は USB
  コンテキストと絡んでいて I2C0 RW が待ち続ける／別プロセスは無関係で通る）。深い機序で、解くには
  cross-process I2C サーバ（FIFO 越しに別プロセスで実行）か、カーネル/USB 側の解析が要る。

### 段階C は daemon 観察に依存せず進められる（方針）
demod write ハングは**段階C の必須ブロッカーではない**と整理する:
- 復調器 TC90532(≒TC90522)もチューナ XC5000 も **mainline GPL ドライバ**があり（PT3/PX 実績）、
  **段階C はこれらを移植して clean 実装できる**。復調器は standalone プロセス（`fapex-i2c` 相当）から
  直接叩ける＝実装・検証は daemon 無しで可能。
- ベンダ daemon の観察が要るのは、mainline `tc90522.c` の**非公開 init の穴**（demod の細かい init 値）と
  **板固有パラメータ**（xtal/IF/地上 tuner の電源 GPIO）を埋める補助のみ。これらは実機での empirical
  探索でも詰められる。∴ demod write ハングの解消は「あれば近道」だが必須ではない。

### （参考）demod write ハング解消が近道になる理由
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

### 復調器は自前クロックで独立（検出順から推定 — 初期分析）
> **訂正（後の実機確定）**: この節は復調器を MB86A22 と仮定した初期推定で、根拠にした
> `MB86A22_FC2581_Detect` は**別チップ（実機は TC90532）**のドライバだった。「復調器は水晶で
> 自前クロック＝CLKPWR 非依存」の結論は TC90532 でも成立する（維持）。ただし「チューナ/Dione の
> 電源は demod の ACK に無関係」は**誤り**で、実機では**復調器バス自体が Dione にゲートされている**
> （§下「Dione 完全起動 → block0 再スキャン」で確定）。

GPL の `MB86A22_FC2581_Detect`（`mb86a22.c`）の検出順が決定的:
1. demod の I2C アドレス設定（0x20 / 0x24）
2. `MB86A22_Reset(slot)`（HW リセット。`feResetFct`→`FE_ControlFeReset(FE_RESET_PULSE)`→board hook
   →**GPIO reset**。pdkv1 は `GPIO_OP_FE_RES_A/B=12/26`、active-low、active50ms→release50ms）
3. **demod のチップ ID レジスタ `0x00` を I2C read**（`0x23`=MB86A22 / `0x13`=MB86A20S）
4. その後にチューナ（RF）アドレスを ping（0xA8…）

**demod の ID read はチューナに触れる前**に成功する必要がある＝**復調器は自前の参照クロック
（水晶）で独立して動く**。チューナ電源（"Dione: Tuner Power"）や tuner の有無は demod の ACK に
無関係。

### 真の非 ACK 原因（初期の切り分け仮説 — 後に否定）
> **訂正（後の実機確定）**: 下の「reset/電源レール/バス/タイミング」仮説は**否定された**。実機では
> **GPIO 電源だけでは前段バス（I2C0/block0）が立たず**、daemon で **Dione を完全起動すると block0 が
> 立ち slave 0x20 が read/write 可**になった（＝**Dione ゲート**が真因）。詳細は §下「Dione 完全起動 →
> block0 再スキャン」。以下は当時の仮説として残す。

（当時の仮説）クロック・チューナ・Dione ではなく、**復調器自身の reset / 電源レール / バス / タイミング**に絞れる:
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

### ★`forceinit` の電源投入列（2026-09-20 実機・トレースで確定）

修正済み `fapex_trace.so` 越しにデーモンを動かし、工場シェルで `forceinit` を打って
捕捉した GPIO 列。タイムスタンプ付きで、§5.3 の推定手順が**そのまま裏づけられた**。

```
t=57.200  pin 0x51 (81) DIONE 3.3V   GPIO_OPEN func=0x00000100 → WRITEBIT 0
t=57.200  pin 0x4e (78) DIONE 1.8V   GPIO_OPEN func=0x00000100 → WRITEBIT 0
t=57.210  pin 0x4d (77) DIONE 1.2V   GPIO_OPEN func=0x00000100 → WRITEBIT 0
t=57.210  pin 0x31 (49) 復調器 3.3V  GPIO_OPEN func=0x00000100 → WRITEBIT 0
t=57.210  pin 0x30 (48) 復調器 1.2V  GPIO_OPEN func=0x00000100 → WRITEBIT 0
t=57.210  pin 0x4a (74) DIONE reset  GPIO_OPEN func=0x00000100 → WRITEBIT 0
          ← ここまでが「全部 0 に落とす」フェーズ
t=57.210  pin 0x4d (77) → WRITEBIT 1
t=57.220  pin 0x51 (81) → WRITEBIT 1
t=57.220  pin 0x4e (78) → WRITEBIT 1
t=57.220  pin 0x31 (49) → WRITEBIT 1
t=57.220  pin 0x30 (48) → WRITEBIT 1
t=57.270  pin 0x4a (74) → WRITEBIT 1     ← 約 50ms 後にリセット解除
```

確定した点:

- **順序は 77 → 81 → 78 → 49 → 48 →(約 50ms)→ 74 解除**。§5.3 の記述どおり。
- **ioctl の pin フィールドは GPL ボードコードの 10 進 GPIO 番号そのまま**
  （`0x51`=81, `0x4e`=78, `0x4d`=77, `0x4a`=74, `0x31`=49, `0x30`=48）。
  トレースが 16 進表示するだけで、別の採番ではない。
- **func は `0x00000100`＝OUT・初期値 0 で開いてから WRITEBIT で上げる**。
  `OUT_1=0x0101`（初期値 1 で開く）とは手順が違う。
- 立ち上げ前に**全ピンを明示的に 0 に落とす**。
- 立ち上げに使うのはこの 9 本（上記 6 本＋リセット `0x0b`=11 地デジ / `0x08`=8 復調器 /
  `0x07`=7 衛星）だけで、トレース中に 543 回現れる `0x49`(73) `0x44`(68) `0x11`(17)
  `0x10`(16) `0x0d`(13) `0x0e`(14) は **LED とリセット SW の 110ms ポーリング**である
  （`fapi199 nr=0x05` を伴う）。混同しないこと。

### ★復調器は Dione の完全起動なしでは触れない（2026-09-20 実機・確定）

「Dione を迂回して復調器だけを立ち上げられるか」を、ベンダバイナリを一切走らせずに
（`fapex-gpio` と `fapex-i2c` のみで）測った。答えは **触れない**。

素の rig（デーモン未起動）で GPIO の初期値は GPL のボードコードどおり全 L
（`48 49 8 11 74 77 78 81` すべて 0＝電源 OFF・リセット保持）。そこから:

| 試した状態 | 復調器 I2C（`fapi174` block0, slave 0x20/0x21, reg 0x0b） |
|---|---|
| 復調器の電源のみ（`49`→`48`）＋リセット解除（`8`,`11`） | **NO ACK / timeout** |
| ＋Dione の電源も（`77`→`81`→`78`→`49`→`48` → `74` 解除） | **NO ACK / timeout**（Dione は USB に列挙せず） |
| ベンダの `forceinit` で Dione を立ち上げた後 | **ACK**（§前節。`tctregr` と一致する値が読める） |

2 番目の状態で `/sys/bus/usb/devices/*/idProduct` は `8171`(WiFi) `0608`(内蔵ハブ)
`0002`×2(ルートハブ) のみで、**Dione の `102d` が出ない**。∴ GPIO で電源とリセットを
入れるだけでは Dione は起動せず、復調器も見えない。§5.4/§5.5 の「手動 GPIO 単独での
Dione 起動は flaky」を、より強い形（単独では起動しない）で確認した。

**CLKPWR 仮説は否定された**（2026-09-20、`tools/fapex-poke` で検証）。`fapi199` を開いた
まま `nr=0x00` → `nr=0x21`（`06 00 00 00 00 00 00 00`）を**同一 fd で**出すと `ret=0` に
なる（別 fd では `-6008`＝FAPex の設定はハンドルに紐づく）。その状態を保持したまま
GPIO で電源を入れても、**Dione は USB に列挙せず復調器も ACK しない**。

### ★★Dione のファーム転送は ioctl ではなく `write()`（2026-09-20 実機・確定）

**訂正**: §5.4 の「bootup で h57fmb0 ioctl 約 1500 回＝Dione ファーム転送」は**誤り**。
ioctl はレジスタアクセスと制御であり、ファーム本体は `/dev/h57fmb0` への **`write()`** で
流れる。`fapex-trace` に `h57fmb*` 限定の read/write フックを足して確定した。

```
write(fd, buf,        232864) = 512     ← n は「残りバイト数」、戻り値が受理バイト数
write(fd, buf + 512,  232352) = 512
write(fd, buf + 1024, 231840) = 512
…
```

| 項目 | 値 |
|---|---|
| 転送単位 | **512 バイト**（USB バルクの最大パケット長）。512 が 2,132 回 ＋ 480 が 1 回 ＋ 416 が 1 回 |
| 総バイト数 | **1,092,480** |
| 内訳 | `mb86h57_h58_aac_idle.bin` 232,864 → 続いて `mb86h57_h58_aac_trc.bin` 859,616（**合計が総バイト数と完全一致**） |
| 転送順 | idle が先、trc が後 |
| ファームのマジック | 先頭 16B が `4d 42 38 41 43 30 31 38 00 00 00 00 00 00 00 a0` ＝ **`"MB8AC018"`** ＋ `00…a0`。両ファイル共通で、フラッシュ上のファイル先頭と一致 |
| `read()` | **0 回**（この観察範囲では転送は片方向） |

`h57fmb0` の ioctl の内訳（同じ観察・計 1,562 回）:

| nr | dir | size | 回数 | 読み |
|---|---|---|---|---|
| `0x02` | R | 12 | 782 | **レジスタ読み**。引数 `{u32 flag, u32 addr, u32 *val}`。addr は `0x08xxxx`/`0x09xxxx` で 2 刻み＝16bit レジスタ、`*val` はスタック上（`0xbe…`） |
| `0x03` | W | 12 | 757 | **レジスタ書き**（同じ引数形） |
| `0x01` | RW | 12 | 9 | 同じ 12B を往復（用途未特定） |
| `0x11` / `0x12` | R / W | 20 | 5 / 5 | 転送記述子らしい `{flags\|len, len, offset, ptr, size}`。`len` は 4 と `0x200` を観測。0x11 で取って 0x12 で返す対で現れる |
| `0x04` | R | 46 | 2 | **版情報**。`"0.0.4.1"`（NUL 埋め 20B）＋ `11 20 21 10 52 76 10 00`（×2）＋ `"V10060700"` ＋ `1a` |
| `0x08` | R | 12 | 1 | GET_INFO。`b8 06 / 2d 10 / 03`＝VID `0x06b8`, PID `0x102d`, index 3 |

**段階 C への含意**: ファーム転送は「512B ずつ `write()` するだけ」で、暗号も challenge も
挟まっていない（少なくともこの経路では）。∴ 自前の USB ドライバを書く場合、必要なのは
**バルク OUT エンドポイントへの 512B 単位の転送**と、上表のレジスタ read/write 相当の
制御転送である。ファーム自体は所有者の機器上の `/slot/firm/` にあるものを実行時に使う
（§1-5 の方針と整合。配布物には含めない）。

### ★★自前ツールで Dione を USB 列挙まで起動できた（2026-09-20 実機・成功）

**決め手は GPIO 操作ごとに `fapi186` を open/close すること**だった。トレースをよく見ると
ベンダは 1 操作ごとに開いて閉じている（毎回 `fd 19` が再利用される）。∴ **close しても
ピンの状態は保持される**。こちらは 9 本の fd を同時に握り続けており、構造が違っていた。

成功した手順（ベンダバイナリを一切使わない）:

```sh
# 1. CLKPWR を同一 fd で 3 発、開いたまま保持する
fapex-poke --hold /dev/fapi/fapi199 \
    0x00 - 0 + 0x21 w 8 0x06 0 0 0 0 0 0 0 + 0x23 wv 4 0x00 0x30 0 0 &

# 2. GPIO は「1 操作 = 1 プロセス」で。fapex-gpio write が open→WRITEBIT→close をする
G="fapex-gpio /dev/fapi/fapi186 write"
$G 7 0; $G 8 0;  usleep 30000; $G 7 1; $G 8 1
$G 11 0;         usleep 20000; $G 11 1
$G 81 0; $G 78 0; $G 77 0; $G 49 0; $G 48 0; $G 74 0
$G 77 1; $G 81 1; $G 78 1; $G 49 1; $G 48 1
usleep 50000;    $G 74 1
```

結果:

```
usb 2-1.4: new high speed USB device using musb_hdrc1 and address 3
usb 2-1.4: configuration #1 chosen from 1 choice
idProduct: 8171 102d 0608 0002 0002      ← 102d = Dione
/dev/h57fmb0 (180,248) も存在
```

CLKPWR の 3 発目 **`nr=0x23` を値渡しで `0x3000`** はトレースから拾った（`fapex_trace.so`
のポインタ保護が `<byvalue:0x00003000>` として記録した）。これが欠けていると起動しない。

**ただし復調器はまだ ACK しない**。Dione が USB 列挙されただけでは足りず、**ファームが
ロードされて動いている**ことが必要＝§5.5 の「block0 が立つ」の実体はこれである。

段階 C への含意:

- **Dione の電源投入と USB 列挙はクリーンな自前コードで再現できる**（達成済み）。
- 残るのは (a) Dione のファーム転送（`/slot/firm/mb86h57_h58_aac_*.bin`＝所有者の機器上の
  blob。§1-5 の方針では配布物に含めず実行時に機器のものを使う）と、(b) その転送路である
  `h57fmb.ko` が GPL 非収録の proprietary ドライバである点。(b) は USB デバイスなので、
  USB トラフィックの観察からクリーンなドライバを書く余地がある＝段階 C の作業。

**注意（実機を壊しかけた点）**: 手動で電源レールを駆動している最中に保持プロセスを
`killall` すると、**実機がリセットする**（2026-09-20 に遭遇。uptime が 1 分に戻り tmpfs が
消えた）。レールを急に落とさないこと。

### 参考: 保持方式での再現は失敗する（2026-09-20）

以下は **fd を握り続ける方式**で試した記録。上の open/close 方式に至る前の経緯として残す。
当初挙げた手順差 3 点のうち 2 点を潰し、CLKPWR も保持した上で再現したが、失敗した。

実施した忠実な再現（`fapex-gpio` の既存機能で足りた。`PIN:0` は `func=0x0100` で開き、
`w:PIN:LVL` は開いたままの fd に WRITEBIT するので、ベンダと同じ形が書ける）:

```sh
# CLKPWR を同一 fd で 0x00 → 0x21 して保持
fapex-poke --hold /dev/fapi/fapi199 0x00 - 0 + 0x21 w 8 0x06 0 0 0 0 0 0 0 &

# リセット 3 本（デーモン起動時 t≈24.4 の手順）→ 電源 6 本（forceinit t≈57.2 の手順）
fapex-gpio /dev/fapi/fapi186 seq \
  7:0 w:7:0 8:0 w:8:0 s:30 w:7:1 w:8:1 11:0 w:11:0 s:20 w:11:1 \
  81:0 w:81:0 78:0 w:78:0 77:0 w:77:0 49:0 w:49:0 48:0 w:48:0 74:0 w:74:0 \
  w:77:1 w:81:1 w:78:1 w:49:1 w:48:1 s:50 w:74:1 &
```

結果: 9 本を保持し CLKPWR も生きている状態で、**Dione は USB に列挙せず**（`idProduct`
は `8171 0608 0002 0002` のまま）、**復調器も NO ACK**。`dmesg` には内蔵ハブ
（GenesysLogic・4 ポート）は出るが、Dione が付く 2-1.4 には何も現れない。

∴ この方式では起動しない。原因は後に判明した（fd を握り続けること自体）。当時の記録: 潰れた候補は func の違い・明示的な全 0 フェーズ・CLKPWR で、残る候補はデーモン起動時
（`forceinit` より前）の初期化**で、トレースの先頭 8,200 行が未精査。GPIO 72（USB
ハイサイドスイッチ）や 75（DIONE FW 書き込みイネーブル）は GPL の初期値が H なので
起動直後から立っているはずだが、実測していない。

**次の一手**: トレーサ付きでデーモンを起動し、**`forceinit` より前の全 fapi 呼び出し**を
読む（`fapi199` の他の `nr`、`fapi186` の初期設定、`h57fmb0` への最初のアクセス）。
そこに Dione を起こす鍵がある。

**段階 C への含意（重要）**: 復調器アクセスが Dione の完全起動に依存するなら、Dione の
ファーム（`mb86h57_h58_aac_*.bin`＝ベンダ blob・再配布不可）が必要になり、クリーンな
配布物だけでは選局できない。逃げ道は 2 つあり、どちらも未検証:

1. **CLKPWR を含めた正しい起動列**を自前で再現して Dione を立ち上げる。ファームは
   所有者の機器から供給される（フラッシュ上の `/slot/firm/` にある）＝配布物には含めず、
   実行時に機器のものを使う形なら §1-5 の方針と整合する。
2. **復調器の I2C ゲートが Dione 以外の要因**（I2C mux・レベルシフタ・未同定のピン）で
   あることを見つける。そうなら Dione 非依存が成立する。

### ★自前ツールで復調器に読み書きできる（2026-09-20 実機・段階 C の前提が立った）

`fapex-i2c`（自前・clean な chardev 利用者）で、**ベンダを介さずに復調器へ読み書き
できる**ことを実機で確認した。ベンダの `tctregr` と同じ値が返り、書き込みも ACK する。

前提: rig でデーモンを起動し `forceinit` で Dione を立ち上げ、地上 ch27 にロックした
状態（Dione が上がっていないと復調器が ACK しない＝§5.5 の block0 の話）。

```
# ベンダの工場シェルでの基準値
tctregr 0x0b → 0x44    0x1e → 0x2a    0x39 → 0xaa    0x73 → 0x45    0xb0 → 0xa9

# 自前ツールで同じレジスタ（SoC I2C0 = fapi174, slave 0x20）
/slot/fapex-i2c /dev/fapi/fapi174 0 read 0x20 0x0b 1  → 44   ← 一致
                                        0x1e          → 2a   ← 一致
                                        0x39          → aa   ← 一致
                                        0x73          → 45   ← 一致
                                        0xb0          → a9   ← 一致

# 書き込み（同じ値を書き戻す冪等な操作）
/slot/fapex-i2c /dev/fapi/fapi174 0 write 0x20 0x0b 0x44  → OK
→ 読み戻し 0x44、その後も Sync=LOCK / E_CN=29.0 / BER=0 で受信は無傷
```

**slave 0x20 と 0x21 はどちらも同じ値を返す**（`0x0b` で両方 `0x44`）。地上側サブモジュール
が両アドレスで見えているか、一方が別名と思われる。要切り分け。

**デーモンが動いている最中でも自前ハンドルで読み書きできる**（§5.5 の「fresh な
fapex-i2c ハンドルなら ACK する」と整合）。∴ 段階 C の選局実装は、まず**デーモンで
Dione を立ち上げてから自前で復調器を叩く**形で段階的に検証できる。Dione 抜きでの
復調器アクセスが可能かは別途確認が必要（§5.5 は手動 GPIO だけでの Dione 起動は
flaky と記録している）。

### ★復調器レジスタのロック時ダンプ（段階 C の仕様源）

工場テストシェルの `tctregr`/`tcsregr`（引数 1 個・16 進）で読んだ 256 バイト全域。
**逆アセンブルを使わずに「ベンダの init が作った最終状態」が取れている**のが要点で、
mainline `tc90522.c` の公開レジスタ意味論と突き合わせる材料になる。

**地上側（`tctregr`）・ch27 = 557142857 Hz ロック時**（Sync=LOCK / E_CN≈29dB / BER=0）:

```
     +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +a +b +c +d +e +f
00: 00 00 00 00 03 00 80 00 00 00 04 44 00 a0 11 10 
10: 00 27 08 33 20 40 00 70 b0 00 30 00 30 02 2a 8a 
20: 00 ff 80 4c 4c 00 00 0c 60 6b 40 40 ff 00 ff 00 
30: 22 0d 21 14 0f 00 00 00 00 aa 10 21 3f 24 08 0c 
40: 0c 00 00 4f ff 00 20 00 90 e4 00 80 00 53 10 05 
50: 00 68 20 7b d8 e0 20 70 10 52 71 00 50 00 b7 00 
60: 1e 20 00 00 00 00 00 00 00 00 80 20 26 28 34 00 
70: 38 00 00 45 16 02 00 01 8c a5 22 30 00 d2 ff 00 
80: 00 10 5a 00 11 8e fa 5c 28 00 00 00 22 92 89 00 
90: 00 59 f9 ff 80 4e c4 7f dd aa c0 00 00 00 00 00 
a0: 00 00 00 00 00 00 02 00 02 00 02 00 00 74 00 71 
b0: a9 20 3d 25 8b 4b 3f ff ff fc 80 00 00 00 00 00 
c0: 00 00 00 00 00 32 01 00 00 00 00 00 00 00 00 00 
d0: 30 26 1c 10 16 00 59 38 58 e8 00 76 00 00 00 00 
e0: 58 52 00 06 f4 00 00 02 00 00 13 05 00 40 00 01 
f0: 00 00 00 00 80 00 00 00 02 00 00 c0 00 00 00 00 
```

**衛星側（`tcsregr`）・BS-1 = 1049480000 Hz ロック時**（Sync=LOCK / E_CN≈21〜22dB / BER=0）:

```
     +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +a +b +c +d +e +f
00: 00 10 00 00 02 00 40 11 11 00 ff 17 59 f2 f0 50 
10: b0 00 30 80 00 00 00 00 04 00 00 00 08 aa 20 08 
20: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
30: 08 00 00 00 00 00 00 71 40 10 00 90 08 96 d9 b6 
40: 5d 99 22 11 58 54 30 41 40 16 41 41 14 04 96 5e 
50: 04 c0 8a 13 90 92 00 00 6d 6f 2e 23 02 33 17 2f 
60: 00 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
70: 00 66 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
80: 00 00 00 00 65 7a 00 00 00 40 00 00 07 00 26 00 
90: 00 00 00 00 00 00 00 4d 0a b0 00 ff ff ff ff 00 
a0: 00 00 00 91 00 40 04 00 00 17 00 00 00 00 00 ff 
b0: ff ff ff ff ff 00 00 00 e4 d9 42 fe 1e d4 05 fd 
c0: 11 ca 60 00 08 ee 41 04 7f ff 30 30 30 30 40 10 
d0: 40 11 40 12 ff ff ff ff ff ff ff ff ff ff ff ff 
e0: ff ff ff ff ff f9 40 10 70 10 80 00 00 00 03 00 
f0: 00 00 00 03 00 00 00 20 9f 00 00 06 00 00 00 00 
```

**読むときの注意**: 一部は測定値・カウンタで、同じ状態でも読むたびに変わる。
実測で変動を確認したのは 地上側 `0x7e`・`0x85`〜`0x9a`・`0xdb`・`0xf6`、
衛星側 `0xb8`・`0xf7`。**設定と測定の切り分けには同一状態での複数回読みが必須**
（地上側で 2 状態の差分 14 個を取ったが、全て変動値だった実例がある）。
衛星側の上位領域に `0xff` が並ぶのは未実装アドレスの読み値と見られる。

### 未解決・次
- **demod init 列の入手**: チップ同定は完了（TC90532＝TC90522 系／XC5000）。(a) **mainline GPL の
  `drivers/media/dvb-frontends/tc90522.c` と `drivers/media/tuners/xc5000.c` を仕様の出発点**にし、
  `fapex-i2c`（I2C0 slave 0x20）で実機検証する（clean-room 順序: 仕様→実装→検証）。ベンダ daemon の
  init 列を丸取りする必要はない。(b) daemon から確認するなら（板固有の xtal/IF/接続）、demod write
  ハングを避ける経路（tune 要求＝control port プロトコル特定、または daemon のハンドル問題の回避）が要る。
- FAPex I2C ドライバは **非 ACK/特定条件で無限ブロック**（timeout=500 が効かない場面）＝観察時は
  D-state ハングに注意。`fapex-i2c` は OpenParam timeout＋`busybox timeout` ラッパで回避。
- `0x72`/`0x73`（I2C2, reg0=`0x01`）の同定は別途。

## 5.6 TS データパスの同定・TSD ABI・生 TS 取り出し（2026-09-18）

目標「地上・衛星それぞれで Multi2 復号**前**の生 TS を取り出す」に向けて、**TS の出口**を
GPL ソースと vendor バイナリ `dione_server`（`grep -a` の printable strings＝挙動の観察。
逆アセは見ていない＝clean-room の「仕様化」）から特定した。

### 確定サマリ
- **データフロー**: アンテナ → tuner(XC5200/D2818) → **demod TC90532**（生 MPEG2-TS＝スクランブル
  済み）→ **Dione MB86H58**（トランスコーダ）→ USB → **SoC TSD** → `TsStreamer` → sink。
- **TS source は SoC の TSD**（`dione_server`: `TsSourceTsd_create`, `FAPEX_TSD_Open/Mmap/Ioctl`,
  `FAPEX_IOCCOM_TSD_GET_BUFFER_LEVEL`, "Overflowed in TSD"）。**sink は file か TCP**
  （`TsSinkFile_create` / `TsSinkSocket`＝HTTP で client へ）。∴ 生 TS はファイルに落とせる。
- **TS 出口の制御 = Dione の Transcode Mode**（`SetTranscodeMode`/`GetTranscodeMode`）。モード enum に
  **`THROUGH`/`MPEG2_THROUGH`（素通し＝トランスコードせず生 TS を通す）**, `IDLE`, `TRANSCODE`,
  `OFFLINE_MPEG2_TO_H264` 等。Dione FW は `aac_idle.bin`/`aac_trc.bin` の 2 種。**通常(app/LASSIE)
  パスは H.264/AAC にトランスコードして配信**するので生 TS は出さない。**THROUGH に切れば生 TS が
  SoC TSD に届く**。`SetScrambledPacketNotifier`（GPL `fapex_config_tsd.c`）から TSD が扱う TS は
  **スクランブル済み**＝目標の生 TS と確定。
- **工場テストシェル**（観察の駆動役に最適）: **パスコマンドは要らない**。`dione_server` を
  起動すると stdin（シリアルコンソール）にプロンプト `-> ` が出て 84 コマンドが使える
  （2026-09-19 実機。`Hello Factory Port (please input passcommand)` は USB シリアル側の
  チャネルに向けた案内で、stdin の可用性とは無関係。`testmode` はコマンド表 075 番の
  引数 1 個のコマンドで、入場の鍵ではない）。詳細は
  `docs/specs/sb-tv02-wfpl-vendor-boot.md` §5。配下に薄い直叩き原語 —
  `Factory_SetTunerFreq`（直接選局）, `Factory_CheckTunerLock`/`_CheckTunerBER`/**`_CheckTunerTS`**,
  `SetLNBPower`/`GetLNBPower`/`GetLNBShortState`（**BS/CS アンテナ電源**）, `TunerPowerOn/Off`,
  `DTV_POWER_CTRL_*`, `ForceStartSignalMonitorUnlock`（ロック無しでも信号監視）。制御は Option バイト方式
  （`[CMD-IN] Option = 0x%02x`）。
- **BS/CS アンテナ電源 = `SetLNBPower`**（`GetLNBShortState`＝過電流検知つき）。衛星選局時に別途 ON にする。
- **TSD の実測パラメータ（2026-09-20）**: ベンダは `tsSelect=0 (CH_A)` / **`iflags=1`（直列+PS）** /
  `bufferSize=0x3ac000` / `pid=0x0000` / `flags=0x04` で開く。CH_A と CH_B の 2 系統。
  `SET_BUFFER_READSIZE`(0x9A) は **値渡し**。詳細は後述の「ベンダ daemon の TSD 設定」。
- **TS のゲートは復調器ではなく Dione の TsStreamer**（2026-09-19 実機で確定）。復調器は常時 TS を
  出しており、`sfreq` だけでは `chktunerts` が `No TS_IN`、`settuner` 後は `sfreq` で再選局しても
  TS が維持される。∴ TSD を自前で読む観察は **`settuner` を使わず `sfreq` でロックさせる**と
  競合しない（`docs/specs/sb-tv02-wfpl-vendor-boot.md` §5）。

### 生 TS の 2 経路（H を優先・段階C のクリーン性を左右）
SoC の TSD は入力を **(a) HW TS 入力 CH（`INPUT_CH_A/B/C`, serial/parallel）** と
**(b) packet-RAM（`LINEAR_AUTO_MODE`=0x29, USB=Dione から流し込み）** の 2 系統から取れる。

- **Path H（demod → SoC TSD 直結, Dione 迂回）**: 生 TS が SoC TSD の HW CH から取れるなら **Dione の
  プロプラ FW/トランスコード非依存**で最もクリーン。段階C の理想。
- **Path D（demod → Dione(THROUGH) → USB → TSD packet-RAM）**: 生 TS 取得に **Dione FW が必須**。
  ~~vendor daemon が実際に使う経路~~ ← **2026-09-20 実機で否定**。ベンダは `LINEAR_AUTO(0x29)` を
  一度も使わず、`tsSelect=0/1`＝ハードウェア TS 入力から読む（後述「自前で TSD を読む試みの結果」）。
  ただし TS が現れるのは Dione のトランスコーダ設定が済んだ後で、デーモンを止めると供給が消える
  ＝Dione が経路のどこかに関与している。「CH_A/CH_B の先にある送り手が復調器か Dione か」は未確定。

**実機シルク写真（2026-09-18）による配置**: **復調器 TC90532 は SoC MB86H610 の真下に隣接**、
**Dione MB86H58A は左端で遠い**。復調器の上辺（デジタル面）が SoC を向く。本機は「MB86H61 Reference
Board」派生（リファレンスは demod→SoC TSD 直結が標準）。→ **Path H が有力**。ただし TS ネットは
多層内層を通り得るので写真は確証でなく、**経験的プローブ（`fapex-ts scan`）が最終判定**。

### FAPex TSD ABI（GPL `abst_lib.h`/`abst_libtsd.h`・PVR サンプル由来）
- **minor 割当**（`abst_lib.h`）: TSD=0..131。**TSD0 common=minor0**, TSD1=1, TSD2=2, TSD3=3。
  channel sub は 4..131（各ブロック 32 ch）＝TSD0 sub=4..35。I2C=174.. と整合（fapi174=I2C0）。
- **ioctl**（type=0xFF, NB_TSD=0x90, NB_SUBID_TSD=0xFD）:
  `OPEN`=_IOW(0x90,OpenParam[20]), `SET_INPUT`=_IOW(0x94,同), `SET_PID_FILTER`=_IOW(0x95,PidParam[20]),
  `OPEN_SUBID`=_IOW(0xFD,{i32 sub_id; i32 channelNo}), `GET_BUFFER_READINFO`=_IOR(0x9C,ReadInfo[24]),
  `SET_BUFFER_READSIZE`=_IOW(0x9A,u32), `GET_BUFFER_LEVEL`=_IOR(0x9E,u32), 共通 START/STOP/CLOSE_SUBID。
- **構造体**: `OpenParam{u32 useStc; tsSelect; syncByteDistance; iflags; oflags}`（`InputParam` も同型）。
  `tsSelect`: CH_A=0/CH_B=1/CH_C=2/LINEAR_AUTO=0x29。`iflags` SP mode: 直列+PS=1/並列-PS=2/並列+PS=3、
  `|AM_SYNC=0x20 |INV_CLK=0x04`。`PidParam{u32 pid; flags; bufferSize; thH; thL}`（pid=0x1fff は無視、
  `FLTFLAGS_RM_TS_HEADER=0x1000` を**付けない**と 188B 生パケットのまま）。
  `ReadInfo{fillSize, readOffset, sectionOffset, sectionLength, sectionNo, bufferStatus}`。
- **lib の id↔fd（`abst_common_priv.c`）**: 共通=open `/dev/fapi/fapiNNN`→`ioctl(TSD_OPEN)`;
  channel=open sub ノード→`ioctl(OPEN_SUBID,{sub_id=minor,channelNo})`→`ioctl(SET_PID_FILTER)`→
  `mmap(fd)`→`START`。
- **読み出しモデル**: START 後 `GET_BUFFER_READINFO`→ mmap ring の `readOffset` から `fillSize` バイト
  読む（ラップは linear 末尾でクランプ）→ `SET_BUFFER_READSIZE(consumed)` で read ポインタを進める。
  read() 直読みも lib にある（`FAPEX_TSD_Read`→`read(fd)`）が、当面 mmap+READINFO を使う。

### ★ベンダ daemon の TSD 設定（2026-09-20 実機・実測パラメータ）

観測 rig（`docs/specs/sb-tv02-wfpl-vendor-boot.md` §6。自前カーネル＋ベンダ userland を
純正のまま起動し、ベンダアプリだけ手で起動する）で `dione_server` を
`LD_PRELOAD=fapex_trace.so` 付きで動かし、工場テストシェルに
`forceinit` → `dpower 1 1` → `settuner 0 557142857 0 0` を流して**地上 ch27 にロックし
`there is TS_IN` が成立した状態**で捕捉した列。上の ABI 記述（GPL ヘッダ由来）に対する
**実際の引数値**にあたる。生ログの抜粋は `temp/observe/tsd-setup-settuner.txt`。

```
[setup]
open  fapi000 (TSD0 common)   → nr=0x90 OPEN W[20]:
        useStc=0  tsSelect=0 (CH_A)  syncByteDistance=0  iflags=1  oflags=0
open  fapi004 (channel sub)   → nr=0xfd OPEN_SUBID {sub_id=0x04, channelNo=0}
                              → nr=0x95 SET_PID_FILTER {pid=0x0000, flags=0x04,
                                   bufferSize=0x3ac000, thH=0x0000b798, thL=0}
mmap  fapi004 len=0x3ac000    → 0x400eb000

open  fapi001 (TSD1 common)   → nr=0x90 OPEN W[20]:
        useStc=0  tsSelect=1 (CH_B)  syncByteDistance=0  iflags=1  oflags=0
open  fapi036 (channel sub)   → nr=0xfd OPEN_SUBID {sub_id=0x24, channelNo=0}
                              → nr=0x95 SET_PID_FILTER {pid=0x0000, flags=0x04,
                                   bufferSize=0x3ac000, thH=0x0002de60, thL=0}
mmap  fapi036 len=0x3ac000    → 0x40497000

[start]  nr=0x01 START を fapi004 と fapi036 の両方に
[poll]   nr=0x9c GET_BUFFER_READINFO R[24]:
           fillSize=0x0000bb44 readOffset=0 sectionOffset=0 sectionLength=0
           sectionNo=0xffffffff bufferStatus=0x00000020
         以降 fillSize≈0xba88/0xb9cc、readOffset が 0xbb44 → 0x0175cc → 0x022f98 と進む
         ＋ nr=0x9a SET_BUFFER_READSIZE、nr=0x9e GET_BUFFER_LEVEL R[4]（0x468/0x3ac/0x524…）
```

**確定した点**:

- **`iflags = 1` ＝ 直列 + PS モード。** `AM_SYNC(0x20)` も `INV_CLK(0x04)` も立てない。
  → `tools/fapex-ts` の既定 `--mode parps`（`iflags=3`＝並列+PS）と**食い違う**。復調器の
  TS 出力設定と対で決まる値なので、総当りより先にこの実測値を使うべき。
- **入力は `CH_A`**（`tsSelect=0`）。`fapex-ts` の既定 `--input A` は合っていた。
- **ベンダは TSD を 2 系統開く**: CH_A(`fapi000`+`fapi004`) と CH_B(`fapi001`+`fapi036`)。
  `bufferSize` は両系統 `0x3ac000`、違うのは `thH` だけ（`0xb798` / `0x2de60`）。
- `sub_id` は channel ノードの minor と一致（`fapi004`→`0x04`、`fapi036`→`0x24`=36）。
  ABI 記述の「`sub_id=minor`」を実機で確認。
- `flags=0x04` で `FLTFLAGS_RM_TS_HEADER(0x1000)` は立っていない＝188B 生パケットのまま
  という ABI 記述どおり。`pid=0x0000`（PAT）。
- `bufferStatus=0x20`、`sectionNo=0xffffffff`。

**ABI 記述の訂正**: `SET_BUFFER_READSIZE`(0x9A) は `_IOW(...,u32)` とエンコードされて
いるが、**実際は値渡し**である。トレースが出す `in=` のバイト列は、その整数をポインタと
みなして読んだ先（たまたまマップされているコード領域）のゴミで、ARM 命令列に見える。
信じてはいけない。同種の値渡し引数は `fapex-trace` の SIGSEGV の原因でもあった
（`msync` による読み取り可能性プローブで対処。コミット `d7b626e`）。

### ★自前で TSD を読む試みの結果（2026-09-20 実機）— 経路の再解釈

上の実測値（CH_A/CH_B・`iflags=1`・`bufferSize=0x3ac000`・`thH`）を `fapex-ts` の既定に
入れ、`bufferStatus`/`GET_BUFFER_LEVEL` を毎秒出すようにして測った結果。

**まず Path D の記述が誤りだった。** §「生 TS の 2 経路」は Path D を「vendor daemon が
実際に使う経路」と書いていたが、**ベンダは `LINEAR_AUTO(0x29)`＝packet-RAM を一度も
使わない**。使うのは `tsSelect=0/1`＝**ハードウェア TS 入力**である。∴ ベンダ自身が
USB packet-RAM 経路ではなく SoC の TS 入力ピンから読んでいる。

**TSD ブロックは互いに独立している**（＝競合は回避できる）:

- デーモンが TSD0/TSD1 を使っている状態で自前ツールが**同じブロック**を掴むと双方が
  壊れる（デーモン側が `ts_streamer.c:467 errno_=22` を吐き続けコンソールを埋める）。
- **別ブロック**（TSD2＝`fapi002`＋channel `fapi068`）なら、デーモンは無傷で動き続け
  （9 スレッド継続・`there is TS_IN` 継続）、こちらも普通に開ける。

**しかし別ブロックでは受信できない**。TSD2 を `tsSelect=1 (CH_B)` で開き、BS にロックして
TS が流れている状態で 5 秒読んでも:

```
[diag] fill=0 roff=0x0 status=0x0 level=0 secOff=0 secLen=0 secNo=-1
```

`bufferStatus=0x0` / `level=0`。ベンダの channel は TS が流れている間 `bufferStatus=0x20`
だったので、**PID フィルタの手前で、そのブロックのバッファに何も届いていない**と読める
（PID 不一致なら status か level が動くはず）。∴ **ハードウェア TS 入力は特定の TSD
ブロックに結び付いている**公算が高い（ベンダの使い方 TSD0←CH_A / TSD1←CH_B が配線を
そのまま反映している）。`tsSelect` はブロックが届く範囲の中から選ぶだけ、という理解。

**デーモンを止めると TS 源自体が止まる**。`killall dione_server` してから TSD0/CH_A
（地上ロック時）や TSD1/CH_B（BS ロック時）を読んでも 0 バイト。復調器は I2C 状態なので
ロックを保つが、Dione の電源・クロックはデーモンが管理しているため、TS の供給が消える。

**∴「ベンダに選局させて自分で読む」は原理的に成立しない。** 必要なブロックはデーモンが
占有し、デーモンを外すと TS が来ない。自前で TSD を読むには**選局も自前で行う**しかない
＝これは段階 C の入り口そのものである。材料は揃っている（ロック状態の復調器レジスタ
ダンプ、`settuner` の引数意味、TSD の実測パラメータ）。

### ツール `tools/fapex-ts`（実装・ビルド済み）
上記 ABI だけで TSD demux 出力を読む clean な chardev 利用者。`probe`（単発）/`scan`（HW 入力
CH_A/B/C × {serial,par,parps}(+amsync) 総当りで sync を探す＝**Path H 判定**）/`capture`（生 TS を
ファイルへ）。`--input/--mode/--pid/--linauto/--secs/-o …`。musl static ARM（`scripts/build-fapex-ts.sh`）。
probe は既定 **pid=0x0000（PAT＝常時存在・非スクランブル）** で TS 到来を確認。

### 未解決・次
- **Path H の実機判定**: 選局は rig のベンダ工場シェルで済む（`forceinit`/`dpower 1 1`/
  `sfreq 1 <Hz>` で復調器が LOCK。`settuner` を使わなければ Dione の TsStreamer は開かれず
  TSD が空く）。その状態で `fapex-ts` を **CH_A・`iflags=1`（直列+PS）** で叩く
  ＝上の実測パラメータを使う。総当り `scan` は当面不要。
  - **2026-09-20 の試行は失敗**: `fapex-ts scan` が最初の組み合わせでブロックした
    （出力は ioctl 定数と "sweeping" の 2 行だけ）。デーモンが TSD を設定済みの状態でだけ
    セットアップを通過し `empty data` を繰り返した（＝掃いた組み合わせに TS が無い）。
    既定が `iflags=3` で実測と食い違っていたこと、および段階ごとのログとタイムアウトが
    無くどの ioctl で止まったか分からないことが原因の候補。**ツール側の修正が先**。
  - 空振りが確定したら Path D（Dione を THROUGH で上げ `--linauto` で TSD packet-RAM）。
- **whole-TS（全 PID）取得機構**: 現状 1 PID=1 channel。ベンダも `pid=0x0000` で 1 PID しか
  開かないので、**ベンダ観察からは答えが出ない**（＝ベンダは全 PID を必要としていない）。
  channel 31 の `bm_wrap`（`PvrChannelSet`）か、PID を複数 channel に並べる方式を自前で
  決める。段階 C の chardev 契約（whole-TS を出す）に直結する設計判断。
- **Transcode THROUGH の設定列**: `SetTranscodeMode(THROUGH)` が h57fmb0 のどの ioctl 列になるかを
  観察（Path D で生 TS を得る場合に必要。Path H なら不要の可能性）。
- **LNB 電源**: `SetLNBPower` の実体（GPIO か I2C か Dione コマンドか）を観察して衛星系で ON にする。

### Stage 2: daemon 完走観察（cross-process I2C 委譲、2026-09-18 実機）

demod write ハングを **cross-process 委譲**（`fapex-trace` の `/tmp/fapex-i2c-deleg` シム →
外部ヘルパ `fapex-i2cd`）で回避し、vendor daemon を board init 完了まで走らせて観察した。

**委譲の実装で踏んだ 3 点（実機で判明）**:
- **この ARM 2.6.31 は socketcall 多重化でなく直接 socket syscall を持つ**（`__NR_socket=281`, bind=282,
  connect=283, sendto=290, recvfrom=292）。socketcall(281,args) は `sys_socket` にルートされ `EINVAL(-22)`。
- **connect + peer 送信では届かない**。connect をやめ**明示アドレス sendto**。
- **返信が返らない（autobind 不発）**。tracer 側ソケットを**一意 abstract アドレス（`\0i2cr`+fd）へ明示 bind**。
- 結果: daemon の demod write（I2C0 slave `0x20`, reg`0x01`=`0x00`）を helper が代行し **rc=0**
  （`[deleg] rc=0`, `[delegdiag] sk=27 sd=36 rv=10`）。**demod write ハング解消を実証**。

**daemon は「dione server is ready to work」まで完走**（board_control→network→WirelessDtvPower→
start_device）:
- **Dione 列挙（2-1.4 `06b8:102d`）**＝daemon の board init（CtrlUsbPhy 等）で確実（手動 GPIO では不可、§5.3/今回）。
  daemon 終了後も Dione は列挙維持＝block0 生存（standalone fapex-i2c で demod 駆動可）。
- **GPIO（fapi186）**: 電源 `81/78/77/49/48` を 0→1、`74`(reset)解除、その後 `07`(衛星 reset)/`08`(復調器 reset)解除。
- **demod reset 解除の最初の I2C**: slave `0x20` reg`0x01`=`0x00`（tc90522 の ISDB-T ter reset 解除に一致）。
- dmesg: Dione は **post-FW 再列挙**（addr N→N+1、EPROTO/ENODEV）を一度起こす＝rebind シムが吸収。

**★ Path H 確定（daemon の実 `TSD_OPEN` OpenParam から）**:
- **TSOUT1 = TSD0（fapi000）: tsSelect=0(CH_A), iflags=1(SP_MODE1=シリアル), sync=0**。
- **TSOUT2 = TSD1（fapi001）: tsSelect=1(CH_B), iflags=1(シリアル)**。
- SET_PID_FILTER（fapi004/fapi036）: pid=`0x0000`, flags=`0x04`, bufferSize=`0x3ac000`(3850240), mmap リング。
- ∴ **入力は packet-RAM でなく HW TS 入力 CH＝復調器 TS は SoC TSD に直結（Path H）**。生 TS は `fapex-ts --input A --mode serial` で読める口。段階C は Dione 非依存（起動ゲートとしては要るが TS 経路には不要）。

**残り（次）**: daemon はサーバで、init 後は**制御ポートでクライアントの選局要求待ち**だが、TCP リスナが立たず
（net stub の影響）プロセスが終了する（`netstat` に制御ポート無し）。`/dev/ttyUSB0` の **Factory Port
（"Hello Factory Port (please input passcommand)"）** も痕跡あり。実際の選局列（demod フル init・地上
tuner 電源・XC5000 FW・周波数）を観察するには **(a) daemon を存続させ制御ポート/Factory で選局要求を送る**、
または **(b) Dione 起動状態のまま standalone（fapex-i2c で tc90522/xc5000 移植＋fapex-ts で TS 取得）**。

### Stage 2 続き: demod/tuner init 列の捕捉・daemon 存続（2026-09-18 実機）

委譲で demod write が完走した後、tracer の潜在バグで daemon が落ちていた:
- **`after_call` の I2C rx ログが WRITE でも `rxDataPtr` を deref**（write では残骸=`0x1`）→ `put_bytes`
  が `0x1` を読んで **daemon を SIGSEGV**（`si_addr=0x1`）。`dir & IOC_READ` を条件に加えて修正
  （READ の時だけ rx ログ）。→ これで daemon が demod 最初の write の後クラッシュせず init を完走。

**★ demod/tuner init 列を捕捉（委譲 54 op、slave 別）**:
- **復調器 TC90532（I2C0 slave `0x20`＝ISDB-T サブモジュール）: 24 レジスタ書き込み（2 パス）** —
  `01=00`（ter reset 解除）, `03=90`, `04=03`, `06=80`, `1c=20`, `1d=56`, `1e=6a`, `1f=9a`, `23=4d`,
  `25=00`, `26=00`, `32=21`, `3b=21`, `3c=3f`, `70=38`, `71=00`, `73=45`, `74=16`, `75=02`, `76=00`,
  `77=01`, `7d=d2`, `e4=f4`, `f4=80`。＝tc90522 系 ISDB-T の基本 init（周波数非依存＝選局前の初期化）。
- **地上チューナ XC5200（I2C0 slave `0x c2`＝7bit `0x61`、demod ゲートウェイ越しでなく直接）: `00 3d 00` ×6**。
  ＝XC5000 の初期化コマンド（周波数はまだ含まない）。
- GPIO: 電源＋reset 解除（pin7 衛星/pin8 復調器）は §Stage 2 と同じ。

**★ daemon をサーバ状態で存続させる方法（判明）**: daemon の `UdpPort_main_loop`（`udp_port.c`）は
**stdin（コンソール）も読む**。sandbox で stdin=`/dev/null` だと `read()=0`（EOF）で main が return し
プロセス終了していた（segfault 修正後の残りの終了要因）。**stdin を保持 FIFO にすると `UdpPort_main_loop`
が read でブロック→daemon が存続**（demod/tuner init 済みのサーバ状態、eth0/ssh 維持、watchdog 無効）。

**コマンド 3 系統（観察）**: (1) **stdin/コンソール** = `UdpPort_main_loop` のイベント/デバッグメニュー
（1 バイト=1 イベント。未知は "Internal error!!! (ev=%d)"、別に "->" プロンプトの "…is not entry" メニュー）。
(2) **`/dev/ttyUSB0`**（USB-serial, 内蔵ハブ 2-1.3）= **Factory Port（passcommand "testmode"→factoryMode→
`setfreq`/`setfreqd`/`gfreq` 等のテキストコマンド、`SetFrequencyCore(%s,%d Hz)`）**。(3) **UDP ソケット** =
LASSIE 制御（`CMD_BLOCK_HEADER`）。

**残り（次）**: 実際の**選局（周波数設定→lock→TS）**を起こすには、上記いずれかのコマンド系統で選局要求を送る:
- 有力 = **Factory Port**: `/dev/ttyUSB0` を pty で与え（`c 136 <ptn>`）、`testmode`→`setfreq <地上ch/Hz>`。
  daemon が周波数設定＋lock まで実行し、TS が SoC TSD（CH_A シリアル）に流れる → **`fapex-ts --input A --mode
  serial capture` で生 TS 取得**。demod/tuner の周波数依存レジスタも fapex-trace で観察できる。
- 代替 = stdin コンソールのデバッグメニュー（`setfreq` がコンソール側なら stdin 直、要確認）。

## 5.7 ベンダ像でのトレース（2026-09-29・vendor-base）

vendor-base（`docs/specs/sb-tv02-wfpl-vendor-boot.md` §7。カーネル・区画表・slot はダンプの
まま）で `dione_server` を `fapex_trace.so` の下で起動し、工場テストシェルから
`settuner 0 557142857 0 0` → `gsignal`／`chktunerstat`／`chktunerts` → `sfreq 1 503142857` →
`settuner 0 503142857 0 0` → `playsts`／`playstop` を流した。以前の観察（chroot・旧 rig）で
止まっていた復調器への書き込みも含め、**起動から選局までの列が全部記録できた**。
生ログは `temp/observe/wfpl-20260929/`（commit しない）、区間ごとの要約は
`tools/fapex-trace/trsum.py`。

### フロントエンドの接続（§5 の訂正）

| 部品 | 経路 | アドレス（FAPex の 8bit 表記） |
|---|---|---|
| 復調器・地上 | SoC I2C0（`fapi174`） | `0x20`（読みは `0x21`） |
| 復調器・衛星 | SoC I2C0 | `0x22`（読みは `0x23`） |
| 地上チューナ XC5200 | **SoC I2C1（`fapi175`）に直結** | **`0xc2`**（読みは `0xc3`。7bit `0x61`） |
| 衛星チューナ | 衛星側復調器の中継。`fe c0 <reg> <val>` で書き、`fe c1` の後に `0x23` から読む | `0xc0`（7bit `0x60`） |

§5 冒頭の「チューナは復調器の `0xfe` ゲートウェイの奥（地上=XC5200）」は地上について誤り。

### 起動と選局の順序（アプリが行うこと）

1. CLKPWR（`fapi199`）：`nr=0x00` → `0x21`（`06 00 00 00 00 00 00 00`）→ `0x23`（値渡し `0x3000`）
2. `mtd1`／`mtd3`／`mtd4` を開く（libooter の設定と nvram）
3. I2C0・I2C1 を OPEN（blockIndex 0/1・speed 1＝100kHz・timeOut 500）
4. GPIO：81/78/77/49/48/74 を 0 → 77→81→78→49→48 を 1 → 74 を 1
5. TSD：TSD0 CH_A（`fapi000`/`fapi004`）と TSD1 CH_B（`fapi001`/`fapi036`）を開いて mmap（§5.6 と同じ値）
6. Dione：GET_INFO → 転送記述子 → 版情報 → レジスタ R/W とファームの `write()`（§5.5 と同じ規模）
7. GPIO 7・8（衛星・地上の復調器のリセット）を 0→1 → 地上復調器（`0x20`）へ 48 回の書き込み
8. GPIO 11（地上チューナのリセット）を 0→1 → XC5200 にファーム（書き込み 257 回）→
   `00 09`（BUSY）を読んで待つ → レジスタ `0x01`=`0x8002`、`0x02`=`0x00c0`、`0x0d`=`0x0000`、
   `0x05`=`0x1040` を書く（書き込みごとに BUSY を待つ）
9. 衛星側復調器（`0x22`）と衛星チューナ（中継経由）の初期化 → 地上復調器 → XC5200 の周波数 → 地上復調器
10. **起動の最後に GPIO をすべて 0 に落とす（Dione と復調器の電源を切る）。**

`settuner` は 4〜9 を丸ごと繰り返してから周波数を書く（Dione のファームも毎回転送し直す）。
`sfreq` は Dione に触れず、衛星側の中継の読み書き → 地上復調器 → XC5200 の周波数 → BUSY 待ち →
地上復調器、だけを書き直す（「復調器だけ」ではない）。

### XC5200 の周波数語

レジスタ `0x10`（XC5000 系の FINERFREQ）に書く値は **（周波数の整数 MHz）× 64 − 104**
＝15.625kHz 単位で「整数 MHz − 1.625MHz」。1/7 MHz は捨てている（mainline `xc5000.c` の
DTV 6MHz は −1.75MHz）。

| 選局 | 書いた値 |
|---|---|
| ch13 473.142857 MHz（起動時の既定） | `0x75d8` |
| ch18 503.142857 MHz | `0x7d58` |
| ch27 557.142857 MHz | `0x8ad8` |

### CH_A に流れているのは約 1.6 Mbit/s（U1 の手がかり）

`settuner` の後、アプリは TSD0（`fapi004`）を GET_BUFFER_READINFO で読み続ける（1 回あたり
47,564 バイト＝253 パケット前後、bufferStatus `0x20`）。読んだ量と時刻から求めたレートは、
ch27・ch18 とも **1.4〜1.7 Mbit/s** で、地上デジタルの TS 全体（約 16.8 Mbit/s）の約 1/10。
∴ 工場手順の `settuner` の後に SoC の CH_A へ届いているのは、Dione がトランスコードした出力と
読める（復調器の生 TS がそのまま来ているのではない）。`playstop` の後も変わらない。
TSD1（CH_B）は地上では読まれない。**確定は B（`docs/sb-tv02-wfpl-chardev-plan.md`）で行う。**

### Dione のレジスタ値と、選局後のパラメータの塊

`h57fmb0` のレジスタ操作（`nr=0x01`〜`0x03`、引数 `{u32 flag; u32 addr; u32 *val}`）は値が
ポインタの先にあり、これまで記録されていなかった。2026-09-29 に `fapex_trace.so` を拡張して
`val=` として記録するようにした（実機で確認済み。レジスタは 16bit で、値は下位 2 バイト）。

`settuner` で XC5200 に周波数を書き、地上復調器を設定した後、アプリは Dione に次を行う
（ch27、2026-09-29）：

1. `0x082008` を読む（`0x0001`）
2. **パラメータの塊を 3 つ書く**（2 刻みの連続アドレス。書くたびに同じ番地を読み返す）

   | 範囲 | 語数 | 中身の見え方 |
   |---|---|---|
   | `0x001000`〜`0x001366` | 436 | 先頭付近に `0x8000`・`0x8004`・`0x8005`・`0x8010`〜`0x8014`・`0x8023`〜`0x8025`・`0x8028`・`0x8029`＝ARIB の SI の PID（PAT・NIT・SDT・EIT・TDT・SDTT・BIT・CDT など）に有効ビット `0x8000` を立てた形。Dione 内のデマルチプレクサの PID の表と読める |
   | `0x001500`〜`0x00168a` | 198 | 大半が 0 |
   | `0x001a00`〜`0x001a6e` | 56 | 大半が 0 |

3. 転送記述子の操作（`nr=0x11` → `nr=0x12`）
4. TSD の CH_A（`fapi004`）と CH_B（`fapi036`）に START（`nr=0x01`）

工場手順のトランスコード設定（`StartTranscodeChange`〜`CommitTranscode`）はこの 2〜3 に当たると
読める。THROUGH（素通し）にしたときにこの塊がどう変わるかは未観察。生ログは
`temp/observe/wfpl-20260929/a2v-full.log`。

### ★SoC の TS 入力に TS を送っているのは Dione（2026-09-29 実機・U1）

`settuner` で ch27 にロックさせた後、アプリを一時停止 → 終了させ、空いた TSD で SoC の
TS 入力を自前の `fapex-ts` で読んだ。

| 状態 | CH_A（TSD0） | CH_B（TSD1） | CH_C（TSD2） |
|---|---|---|---|
| アプリ動作中（アプリ自身の読み出し） | 約 1.6 Mbit/s | 読まれていない | — |
| アプリ終了後・復調器はロックのまま（`0x80`=`0x00`・`0xb0`=`0xa9`） | 0 B | 0 B | 0 B |
| 同上＋アプリと同じ CLKPWR 設定（`0x00`→`0x21 06…`→`0x23 0x3000`）を保持し給餌 | 0 B | 0 B | 0 B |

後の 2 行はいずれも入力形式を総当り（直列／並列／並列＋PS × 自動同期の有無）して 0 B。
∴ **復調器の TS は SoC の TS 入力には来ていない**。復調器の TS は Dione の TS 入力に入り
（工場手順の `chktunerts` が見ているもの）、SoC の CH_A に届くのは Dione の出力である。
生 TS を SoC で得るには、Dione を THROUGH（素通し）で動かす必要がある（§5.6 の Path D 系。
ただし USB の packet-RAM ではなく、SoC の TS 入力ピン経由）。

### トランスコードの設定（`gettraco`）と塊の語の対応（2026-09-29 実機）

工場テストシェルの `gettraco`（引数なし）は、アプリが持つトランスコードの設定を名前と数値で
表示する。待機中（Dione の電源が切れている）でも表示され、そのとき Dione には触れない
＝アプリ内の値で、選局のたびに Dione へ書かれる。既定値（2026-09-29・抜粋）：

```
FirmwareMode         = MPEG2_TO_H264(0x0)
VideoTSOutMode       = TRANSCODE(0x1)        VideoTSOut2Mode = HUMBNAIL_TRANSCODE_REMUX(0xb)
AudioTSOutMode       = THROUGH(0x1)          AudioTSOut2Mode = TRANSCODE(0x2)
AudioMode            = MPEG2_AAC(0x4)        Audio2Mode      = MPEG4_AAC(0x5)
PMTMode / PMT2Mode   = REWRITE_STREAM_TYPE(0x7)
In{Video,Audio}StreamType  = MPEG2_VIDEO(0x2) / MPEG2_AAC(0xf)
Out{Video,Audio}StreamType = H264(0x1b) / MPEG2_AAC(0xf)   Out2… = H264(0x1b) / MPEG4_AAC(0x11)
Video{Out,2Out}StreamID = 0xe0   Audio{Out,2Out}StreamID = 0xc0
HOutSizeSD×VOutSizeSD = 720×480  HOutSizeBL×VOutSizeBL = 320×240
VideoBitrate/Max/Ave/Min = 4000/4000/2000/1200   …BL = 768/768/512/256
GOPSize = 15  GOPStruct = IBBP(0x0)  VLCMode = CABAC(0x0)  InterlaceProgressive = INTERLACE(0x1)
PID[0..26] = 0x0010, 0x0011, 0x0012, …   CheckoutFormat = MEMORY_STICK(0x0)
```

モード名の語彙（`dione_server` の文字列）：`TRANSCODE`・`THROUGH`・`MPEG2_THROUGH`・
`OFFLINE_MPEG2_TO_H264`・`OFFLINE_MPEG2_TO_MPEG2`・`OFFLINE_H264_TO_MPEG2`・`THUMBNAIL_TRANSCODE`。
操作の語彙：`StartTranscodeChange` → `Set{TranscodeMode,TranscodeSetting,TranscodeSetting2,
TranscodeSetting3,TranscodePIDFilter,TranscodeCheckoutFormat,Transcode3D_2D_Mode}` →
`CommitTranscode`。外からは制御ポートのサービス `LSC_DIONE_TRANSCODE` として見える。

選局時に書く塊（前節）との対応（値の一致から読んだもの）：

| 番地 | 値 | 対応する設定 |
|---|---|---|
| `0x00101c` | `020f` | `InVideoStreamType`(2)・`InAudioStreamType`(0xf) を 1 バイトずつ |
| `0x001028`〜 | `8010 8011 8012 8014 …` | `PID[0..]` に有効ビット `0x8000` |
| `0x001130`・`0x001132` | `00e0`・`00c0` | 出力 1 の `VideoOutStreamID`・`AudioOutStreamID` |
| `0x00113a` | `1b0f` | 出力 1 の `OutVideoStreamType`・`OutAudioStreamType` |
| `0x00127a` | `1b11` | 出力 2 の `Out2VideoStreamType`・`Out2AudioStreamType` |
| `0x001532`〜`0x001538` | `0fa0 0fa0 07d0 04b0` | `VideoBitrate`・`Max`・`Ave`・`Min` |
| `0x001540`〜`0x001548` | `02d0 01e0 0fa0 07d0 04b0` | `HOutSizeSD`・`VOutSizeSD`・SD のビットレート |
| `0x00162e`〜`0x001638` | `0140 00f0 0300 0300 0200 0100` | `HOutSizeBL`・`VOutSizeBL`・BL のビットレート |
| `0x001586`・`0x001588` | `ea60`・`03e9` | 60000/1001（フレームレート） |

`VideoTSOutMode`（`TRANSCODE`＝1）がどの語かは未確定（`0x00100c`＝`4512` のように 4bit ずつ
詰めた語がある）。

**`h57fmb0` の `nr=0x01`（12 バイト・RW）はレジスタ操作ではなくコマンドの口**。先頭の語の下位
16bit がコマンド番号、上位 16bit がサブコマンド、3 語目が引数（body）。アプリのログ
`send cmd [OK] cmd = 0x%02x sub_cmd = 0x%02x body = 0x%04x` と一致する（`ssloglev 255` で出る。
既定では出ない。255 は他のログも大量に出すので観察後は `ssloglev 7` に戻す）。`settuner` の後半
（Dione が既に起きているとき）で観測した順：`cmd 0x08` ×数回 → `cmd 0x05 body 0x0004` →
`cmd 0x05 body 0x0001` → （塊）→ `cmd 0x04 sub 0x20 body 0x0000`（`CommitTranscode` の中）→
`cmd 0x05 body 0x0002`。コマンド名の語彙（文字列）：`send_cmd(INITIALIZE,MPEG2_H264)`・
`send_cmd(INITIALIZE,DOWNLOAD)`・`send_cmd(IDLE)`、状態 `SYSTEM_STATE_START`／`STOP`／`NULL_OUT`。
ログ 255 では、`CommitParam` が書いた塊の語を 1 つずつ読み返して `get_lsi_register 0x100a = 0x0010`
のように表示する＝アプリはこの塊を「LSI のレジスタ」として扱っている。

**塊の書き込みは 1 回の `settuner` の中で 2 回**（Dione を冷起動したとき）。idle ファームの後と、
トランスコード用ファーム（trc・1,679 塊）を転送して選局した後。**2 回で値が違うのは `0x00100a`
だけ**（前者 `0x001b`、後者 `0x0010`）。4bit ずつ見ると、下位の `b`・`0` が `VideoTSOut2Mode`
（既定 `0xb`）、次の `1` が `VideoTSOutMode`（TRANSCODE＝1）に当たると読める（仮説）。

**差し替えの試行**（`fapex-trace` の `/tmp/fapex-h57-rewrite`。揮発のみ）：

| 試行 | 差し替え | 結果 |
|---|---|---|
| 1 | `0x00100a` を 1 回目（`0x001b`）で `0x0000` に | idle ファームの後の `cmd 0x04 sub 0x20` がタイムアウト（`errno = 110`）→ `XERROR factor 0x80000000` → STOP。TS 無し |
| 1b | `0x00100a` を 2 回目（`0x0010`）だけ `0x0000` に | `CommitTranscode` の中の同じコマンドがタイムアウト → STOP。TS 無し＝両方 0 は無効な設定 |
| 2〜7 | `0x00100a` を 2 回目だけ `0x0020`・`0x0040`・`0x0050`・`0x0060`・`0x0070` に | どれも受け付けられ（エラー無し）、CH_A のレートも中身（PID の構成・ヌルの割合）も基準と同じ |

∴ `0x00100a` の 4bit を単独で変えても出力は切り替わらない（または THROUGH ではない値）。

### CH_A の中身と、アプリ動作中にリングを読む方法（2026-09-29 実機）

TSD のリングはアプリの mmap の中身がそのまま物理メモリにある（`/proc/<pid>/maps` の
`/dev/fapi/fapi004` の行のオフセット＝物理アドレス）。**CH_A は物理 `0x46000000`、CH_B は
`0x45400000`、各 3,850,240 B**。アプリが TSD を握っていても `/dev/mem` から読める
（`dd if=/dev/mem bs=4096 skip=$((0x46000000/4096)) count=940`）。デバイス領域なので
`/proc/<pid>/mem` からは読めない。

トランスコード設定（既定）で ch27 を選局したときの CH_A（`tools/host/tsref.py` で要約）：

- 20,480 パケットのうち **約 89% がヌル**。レートは一定（約 1.6 Mbit/s）で、中身が変わっても
  レートは変わらないと見られる＝**レートだけでは THROUGH を判定できない**。
- 残りは PAT・CAT・SI（`0x0010`/`0x0011`/`0x0012`/`0x0014`/`0x0024`）・`0x01f0`・`0x01ff`（PCR）・
  `0x0901`・`0x0130`/`0x0138` を少量ずつ。**映像にあたる大量のパケットは無い**。
- `transport_scrambling_control` が `10`/`11`（本物のスクランブル）のパケットは 0。Dione の出力は
  `01`（予約）の印を付けている。

CH_B（TSD1）には地上の選局中、TS が来ていない（リングはほぼ 0 埋め）。

### ウォッチドッグ（2026-09-29 実機）

- アプリの LED・リセット SW の 110ms ループが出す CLKPWR `nr=0x05` は
  **`FAPEX_IOCCOM_CLKPWR_WATCHDOG_RESET`**（アプリ自身のエラーログに出た名前）。
- 別のプロセスが CLKPWR を開いて `nr=0x00`（OPEN）を出すと、アプリの給餌は `-6008` で
  失敗するようになり、その後アプリを止めても 100 秒以上リセットされなかった
  （OPEN でウォッチドッグの設定が解けたと読める）。
- ∴ アプリに選局させた後、アプリを止めてハードの状態を引き継ぐことができる
  （Dione は列挙されたまま、復調器はロックのまま）。手順：`fapex-poke` で CLKPWR を OPEN
  → アプリの全スレッドに `kill -STOP` → `kill -9`。

## 5.8 ★受信の位相と生 TS の所在（2026-09-29 実機・確定）

「Dione のどこかから生 TS を出せないか」を突き詰めた結果、**WFPL ではソフトから生
（スクランブルのまま）TS は取れない**ことが分かった。根拠と構図：

### ICカードの口は Dione 配下（＝MULTI2 復号は Dione の中）

- 主 CPU のユーザランドに ICカード／スマートカードの口もノードも無く、`fdlicc.ko` も
  載っていない。カードへの接続はすべて `LSC_DIONE_ICCARD`／`WirelessDioneICCard.c`
  経由（`GetICCardStatusInfo`・`ICCardCmdSet/Get`）＝h57fmb の USB トンネルで Dione へ。
- SoC 自体は ICカードコントローラ（GPL `MB86HXX_ICC_BASE 0xC5000000`・IRQ 12/13）を
  2 面持つが、この製品ではドライバも載らず未使用。B-CAS スロットは Dione 側に配線。
- ∴ ECM→カード→Ks→MULTI2 復号は Dione の中で完結する。

### AV サブシステム＝この製品では Dione（主 SoC のは休止）

- FAPex SDK には AV 一式（`fdlvidec`/`fdlvienc`/`fdlaudec`/`fdlauout`/`fdlviout`/
  `fdlhdmi`…）があり、SoC は AV 用に 46MB（`CONFIG_FIRM_AREA_SIZE`）を予約する＝主 SoC は
  AV サブシステムを持つ。だが**この製品のスロットに載る FAPex は TSD/I2C/GPIO/CLKPWR の
  4 つだけ**で、AV デコード・エンコード系は 1 つも載らない。トランスコード＋復号は
  すべて USB 接続の別チップ Dione（MB86H58）が担う。YB1 で復号を担った裏 CPU にあたる
  ものが、WFPL では独立した封止チップに外出しされている。

### 受信の位相（配線）

```
アンテナ → tuner(XC5200/D2818) → 復調器 TC90532 ──(生・スクランブル TS)──> Dione
                                                     │  [System Demux → ECM→カード→Ks
                                                     │   → MULTI2 復号 → トランスコード]
                                                     └──(復号済み TS)──> SoC TSD CH_A/CH_B
```

- **復調器の生 TS は SoC の TS 入力には来ない。「ロックだけ」と「配信」の両状態で確認した**
  （2026-09-29。ロックと配信は別状態なので分けて見た）：
  - **ロックだけ**（`forceinit`→`dpower 1 1`→`sfreq 1 <Hz>`＝復調器 LOCK・Dione の
    TsStreamer は開かない・`chktunerts` は No TS_IN）：SoC 入力 A/B/C を全モード
    （直列／並列／並列+PS）で読んで 0 バイト。
  - **配信**（`settuner`＝Dione の TsStreamer を開く）：CH_A は Dione の出力、空いている
    CH_C は全モード 0 バイト、CH_B も別ブロックで空（§5.6）。
  - SoC が受けるのは Dione の**出力**だけ（CH_A の約 1.6 Mbps・ヌル 89%・映像
    無し・スクランブル 10/11 無し＝復号後の細い remux）。
- ∴ 生 TS は**復調器→Dione の基板配線上**にしか無く、ソフトからは取れない。Dione の
  中で TS は必ず復号段を通る（THROUGH は「復号済み・トランスコードなし」）。

### YB1 との違い（なぜ YB1 は取れて WFPL は取れないか）

- YB1（BMMPX1301）は B-CAS スロットが無く**オンデバイスで復号しない**。復調後の生
  スクランブル TS が主 SoC の AV SRAM の入力 FIFO に入り、そこを読めた（参照受信機と
  ビット一致）。復号器の**手前**に SoC のタップがあった。
- WFPL は B-CAS スロットを持ち Dione が**インラインで復号**する。復調器→Dione→SoC の
  一方向で、SoC は復号後しか見えない。復号器の手前に SoC のタップが無い。

### 含意（生 TS が要るなら）

- ソフトだけでは不可。取るなら (a) 復調器→Dione の TS 配線を基板でタップする（ハード
  改造・CFW のソフト範囲外）、または (b) Dione に取り込んだ TS を復号前に吐かせる
  （Dione 内部モード＝B-CAS/復号器を手探りする方向で、限定受信の境界に近く採らない）。
- **OpenIRD の chardev 契約は「生（スクランブル）TS」**（CLAUDE.md §4。復号は上位に
  委ねる）。WFPL はこの契約をソフトだけでは満たせない。一方、選局（復調器 TC90532・
  チューナ XC5200/D2818）の仕様は Dione に依存せず §5.7 で採れているので、段階 C の
  「選局部分の仕様」は WFPL でも成果として残る。

## 5.9 復調器 TC90532 の制御（主 CPU・公開ドライバで書ける・2026-09-29）

「復調器を誰が・何を送って制御するか」と「なぜ Dione が要るか」を詰めた。

### 誰が・何を（観察）

- **操作するのは主 CPU**。FAPex I2C0（`/dev/fapi/fapi174`・SoC I2C0 `0xD1000000`）で
  復調器へ。スレーブ **0x20＝地上（ISDB-T OFDM）／0x22＝衛星（ISDB-S）**。Dione は関与しない。
- **選局時に主 CPU が 0x20 へ書く初期化（24 レジスタ・観察したバイト列）**：
  `01=00 03=90 04=03 06=80 1c=20 1d=56 1e=6a 1f=9a 23=4d 25=00 26=00 32=21 3b=21 3c=3f
  70=38 71=00 73=45 74=16 75=02 76=00 77=01 7d=d2 e4=f4 f4=80`（周波数で変わるのは
  1c〜1f・23・7d 付近＝AGC/搬送波系。値は電波・場所依存）。
- **状態の読み**：`0x80`（ロック）・`0x8b`（C/N・3 バイト）・`0xb0`。自前ツールで読める
  （§5.5）。
- **チューナへの中継**：`0xfe`（I2C スルー）→ 衛星チューナ CXD2818。地上チューナ XC5200 は
  I2C1（`0xc2`）直結。

### 公開情報（この復調器はブラックボックスではない）

- チップは **Toshiba TC90522**（= TC90532XBG）。**mainline に `drivers/media/dvb-frontends/
  tc90522.c`**（カーネル 3.18〜）、および **px4_drv（nns779・GPL）** が公開ドライバ。PT3 /
  PX-Q3PE 等で使われる 2×ISDB-T OFDM ＋ 2×ISDB-S 8PSK の 4 サブモジュール復調器で、各サブが
  別 I2C アドレス＝観察の 0x20/0x22 と一致。
- mainline は「初期化情報が非公開」としてあえて薄い（`TC90522_I2C_THRU_REG 0xfe` は一致）。
  **px4_drv がより完全**で、`0x03`=sleep（0xf0/0x00）、`0x1d`=TS 出力ピン、`0x25/0x20/0x23/0x01`
  =AGC、読み `0x80`/`0x8b`/`0xb0`＝ロック/C-N と定義。**こちらの実機の読み（0x80/0x8b/0xb0）は
  px4_drv とバイト一致**。
- ∴ 段階 C の復調器・チューナ制御は、**公開 GPL（tc90522.c / px4_drv / xc5000.c）の意味論＋
  観察した初期化値**で書ける（`Origin: gpl-derived`＋`observed`）。Dione のプロプラに依存しない。

### 復調器は Dione 抜きで制御できる（§5.5 の訂正・2026-09-29 実機）

`noapp`（FAPex だけ載せ、ベンダアプリも電源シーケンスも動かさない）で切り分けた。**CLKPWR も
Dione も一切無し**で、復調器の電源（GPIO 48/49）とリセット解除（7/8）だけを入れると、**復調器は
I2C0 で ACK する**（`0x20` reg `0x80`=`0x2e`・`0xb0`=`0xa0`）。地上チューナ XC5200（`0xc2`）も
ACK する。∴ §5.5 の「復調器は Dione の完全起動なしでは触れない」は誤りで、当時の切り分けが
不完全だった（reset の解除順か CLKPWR の干渉）。前段の**制御は Dione から独立**。

- **CLKPWR は I2C に不要**（ブート既定のクロックで FAPex I2C0/TSD が開ける）。**以前の再起動は、
  こちらの CLKPWR 初期化がウォッチドッグを起動し給餌しなかったため**と判明（CLKPWR を触らなければ
  noapp は安定）。`fapex-i2c --timeout` で非 ACK も guard できる。

### ★Dione 抜きで復調器をロックさせても生 TS は SoC に来ない（2026-09-29・確定）

採取済みの前段選局列を、**書き込みだけでなく読み（チューナの BUSY ポーリング）も順序どおり
再生**すると（464 操作＝W 433＋R 31。R がチューナの `0xc3` 待ちの自然な遅延になる）、**Dione を
一切起こさず**（CLKPWR も無し・復調器 rail 48/49＋reset 7/8＋チューナ reset 11 だけ）に、
復調器が**安定してロック／受信状態**に達した：`0x80`=`0x80` 一定、`0x8b`（C/N）=`0x000015`〜
`0x000018` で安定（未受信時は 0）、`0xb0`=`0xa9`（Dione あり時のロック状態で見た値と一致）。
書き込みだけの再生（前版）が `0x8b`=0 のままだったのは、この読み待ちを飛ばしていたため。

**その状態で SoC TSD 入力を A/B/C × serial/par/parps の全組で読んでも 0 バイト。** ∴ **復調器の
TS 出力は SoC に配線されておらず、Dione へ行く**——復号器を一切介さずに確定した。復調器が実際に
受信・TS 出力している最中でも SoC は何も受けない。

含意（確度）:
- **「生 TS はソフトから取れない」の確度が上がった。** Dione あり時の実測（SoC は Dione の復号後の
  細い出力・§5.8）に加え、**復号器を全く介さない経路でも SoC に生 TS が現れない**ことを直接示した。
- **前段（復調器 TC90532＋チューナ XC5200）は Dione 非依存でフルに駆動でき、ロックまで到達できる**
  （CLKPWR も不要）。§5.5 の「Dione なしでは触れない／ロックできない」は完全に否定。段階 C の選局は
  公開 GPL（tc90522.c / px4_drv / xc5000.c）＋観察値で実装・実機検証できる。ブロックされるのは
  TS の取り出しだけ（Dione へ行くため）。
- **なぜ「Dione が要る」と見えたか**＝立ち上げ順（復調器 reset を Dione の後に解除）と、こちらの
  再生が読み待ちを飛ばしていたことの二重の誤り。Dione は前段の制御にもロックにも要らない。

## 6. 段階 C（クリーン実装）への含意

- OpenIRD が満たすべき chardev 契約は **px4_drv 互換**（CLAUDE.md §4）。FAPex の
  `/dev/fapi/fapiNNN` はそのままでは px4_drv ではない。段階 C は、ここで観察した
  「復調・チューナへの I2C 手順」を仕様に起こし、px4_drv 互換 chardev の下に
  クリーン実装する。
- 当面はまず **復調器の I2C 実バイト列**（§4 のトレーサ。**復調器は SoC FAPex I2C fapi 0x60・
  slave 0x20**＝§5.4）と、生 TS の口（**§5.6 で SoC TSD＝fapi 0x90 と確定。読み出しは `fapex-ts`**）を
  取る。生 TS が読める口まで辿れば段階 C の設計に足りる観察が揃う。**Path H（demod→TSD 直結）なら
  Dione 非依存でクリーン、Path D なら Dione THROUGH が要る**＝§5.6 の scan で判定する。

## 7. 参照

- GPL: `FAPI_FAPexK_V01L02R02_*`（カーネル薄層・ヘッダ）、`FAPI_FAPexU_V01L02R02_*`
  （ユーザ lib）。所有者が Pixela の GPL/OSS 提供で入手し `dl/` に置く（配布物には
  含めない）。
- 機種の物証・区画・侵入路は `temp/spec_SB-TV02-WFPL.md`（内部）。
