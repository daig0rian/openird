# POE-BR100-YB1 受信スタックの構成と境界（段階 B の観察）

<!-- openird-spec
Status: observing
As-of:  実機 POE-BR100-YB1 / ベンダ FW 2.1.17 r3 (2026-09-21)
Impl:   -
Tests:  -
Note:   段階 C（ird-tuner-*）の契約になる予定。観察が続いている
-->

<!-- Open 層: (1) factory コンテナから取り出したベンダ像の静的観察（ファイル一覧・
     modinfo・平文スクリプト）、(2) 実機の /proc・/sys・fd の読み取り、(3) ベンダが
     公開する UPnP サービス記述と、その action を実際に叩いた応答。
     逆アセ由来の内部ロジックは置かない。個体識別情報は伏字。 -->

Milestone 1。ベンダ FW を載せたまま実機を観察し、**受信の主役がどのカーネル ABI を
使うか**を固定する。段階 C（`ird-tuner-*` のクリーン実装）が満たすべき契約の原型。

観察は 2026-09-21、`poe-br100-yb1` 実機（Renesas EMMA3SEP / MIPS BE）で行った。
手順は §8。

## 0. スコープ

限定受信（B-CAS / MULTI2 / ARIB スクランブル解除）は**永久に対象外**。本文書は
復調器のロックと**スクランブルされたままの TS**の取り出しだけを扱う。`/dev/val_dscram`
`/dev/val_scram` および CAS 関連のベンダ資材は、存在を列挙する以上のことをしない。

## 1. 実機の素性

| 項目 | 値 |
|---|---|
| カーネル | `2.6.27.39 #924 PREEMPT Mon May 23 12:16:17 JST 2016 mips` |
| cmdline | `rootfstype=squashfs root=/dev/mtdblock3 ro ip=none PRODUCT_FWIDS=PIX_AIRTUNER_LITE U_BOOT_REV=612` |
| ベンダ FW | 2.1.17 r3（`/app/bin/version.dat`） |
| libc | **uClibc 0.9.31**（`/lib/ld-uClibc-0.9.31.so`）＝Milestone 0 チェック 2 |
| System RAM | `00000000-08ffffff`（144MB）。`free` が見せるのは 106MB＝**残りは AV 側の予約** |

マウント:

```
/dev/root       /             squashfs ro     (mtd3)
/dev/mtdblock4  /app/bin      squashfs ro,noatime
/dev/mtdblock8  /app/setting  yaffs2   rw,sync,noatime
/dev/mtdblock9  /app/flashDev yaffs2   rw,sync,noatime
```

## 2. 受信スタックの構成（`/app/bin/drivers/` の .ko）

受信の知能は**カーネルモジュール側**にあり、`blur`（`/app/bin/blur`, 10MB）が
chardev 越しに使う。SB-TV02 の FAPex と違い、**モジュールが役割ごとに分かれていて
名前が意味を持つ**。

| モジュール | MODULE_LICENSE | depends | 実機で load | 生やす chardev |
|---|---|---|---|---|
| `ei2c_ofdm_drv` | なし（unspecified） | — | ○ | `/dev/ei2c` (222,0) |
| `fe_common_drv` | なし | ei2c_ofdm_drv | ○ | — |
| `fe_main_drv` | なし | fe_common_drv | ○ | **`/dev/fe_main` (220,0)** |
| `demod_tc90522_90532_drv` | なし | fe_common, ei2c_ofdm, fe_main | **×** | — |
| `tuner_MxLinear_MxL135RF_drv` | なし | fe_common_drv | **×** | — |
| `tunerW_MxLinear_MxL135RF_drv` | なし | fe_common_drv | **×** | — |
| `tuner_PHILIPS_TDA8263HN_drv` | なし | fe_common_drv | **×** | — |
| `tunerW_PHILIPS_TDA8263HN_drv` | なし | fe_common_drv | **×** | — |
| `io_controller_drv` | なし | ei2c_ofdm_drv | ○ | `/dev/io_controller` (221,0) |
| `csd` | なし | — | ○ | major 252（`/proc/devices` の `csd`） |
| `val_merge` | **Proprietary** | csd | ○ | `/dev/val_*` (230) / `/dev/val_demux*` (231) |
| `mem2` | Dual BSD/GPL | — | ○（permanent） | `/dev/mem2` (245) |
| `i2c_emma` / `i2c_algo_emma` | **GPL v2**（Renesas Electronics Corporation） | — | ○ | `/dev/i2c-N` (89) |
| `h57fmb` | GPL | — | ○ | `/dev/h57fmb0` (180,248) |
| `8192du` | GPL（Realtek） | — | ×（起動後 halt） | — |

全て vermagic `2.6.27.39 preempt mod_unload MIPS32_R2 32BIT`。カーネルは
`ei2c_ofdm_drv` の license 未宣言で taint（`/proc/sys/kernel/tainted` = 1）。

**含意 3 点**:

1. `demod_*` / `tuner_*` の .ko は**実機で load されない**のに地上波がロックする
   （§5）。∴ 本機の復調・チューナ制御は `fe_main_drv` / `fe_common_drv` /
   `ei2c_ofdm_drv` の中にあり、別 .ko は他グレード（W/R・衛星）向けと考えられる。
2. **ファイル名は品種の証拠にならない。** `demod_tc90522_90532_drv` /
   `tuner_MxLinear_MxL135RF_drv` という名前はあるが、これらは load されない。
   load される側のシンボルはまったく別の品種（`tc90512` / `hst111ah`）を語る。
   §2.1 のとおり 1 つのバイナリが設定で多機種を賄う作りなので、**本機の実物は
   シンボルからは確定できない**（分解写真は `MXL683`）。
3. 復調器が東芝 TC905xx 系であれば、mainline の GPL `tc90522.c` が意味論の出所に
   なり得る（SB-TV02-WFPL と同系列＝
   [sb-tv02-wfpl-stage-b-fapex.md](sb-tv02-wfpl-stage-b-fapex.md) §5.5 も効く）。
   **ただし品種未確定なので、これは当てにできる前提ではない**（§7-1）。

`i2c_emma` / `i2c_algo_emma` の **GPL ソースは配布されている**（`drivers/i2c/busses/
i2c-emma.{c,h}`、`drivers/i2c/algos/i2c-algo-emma.{c,h}`）。u-boot 側にも
`drivers/i2c/i2c-emma3sx.c` がある。∴ i2c の足回りは `Origin: gpl-derived` で書ける。

### 2.1 モジュールのシンボルから読める構造（2026-09-21）

`nm` と `strings` による観察（clean-room で許された範囲。逆アセはしていない）。
**各シンボルの意味は名前から推定したもので、実装は読んでいない。**

`ei2c_ofdm_drv` は独自の I2C コントローラを持つのではなく、**Linux 標準の i2c
クライアントドライバ**である: `i2c_register_driver` / `i2c_attach_client` /
`i2c_master_send` / `i2c_master_recv` / `i2c_smbus_read_*` を参照し、文字列
`emma_i2c_0` を持つ。∴ 復調器は標準アダプタ上に居る。

**このモジュール群では、チューナは復調器の I2C ゲートの向こう**にある。根拠:

| モジュール | シンボル |
|---|---|
| `ei2c_ofdm_drv` | `ei2c_get_through_demod_tc90512` / `ei2c_set_demod_tc90512` |
| `fe_common_drv` | `TunerRead_viaDemod` / `TunerWrite_viaDemod` / `Tuner_set_slave_addr` / `getSlvAdr_Demod1` / `getSlvAdr_Demod2` |

**⚠ ただしこれは本機の構成ではない。** これらのモジュールが語るのは復調器とチューナが
別チップの設計（TC905xx ＋ 別チューナ）で、本機の `blur` はその経路を使わない
（§3.1 で `/dev/fe_main` への ioctl が 0 件と実測）。本機のフロントエンドは
**MaxLinear MXL683＝RF チューナと ISDB-T 復調の 1 チップ**と見られ、それなら
バス上に 1 アドレスしか現れないこと（§4.1）とも整合する。詳細は §2.2。

`fe_main_drv` は**段階 C の chardev が出すべき値をそのまま持っている**:
`FE_get_agc` / `FE_get_ber` / `FE_get_cn` / `FE_get_sync` / `FE_select_channel` /
`FE_select_source` / `FE_set_lna` / `FE_set_ISIC` / `FE_debug_show_demod_reg_all` /
`FE_set_debuglevel`。UPnP の `X_GetOfdmTunerInfo`（§5）の Agc/Ber/Cn と 1:1 に対応する。

**ただし blur はこの経路を使っていない**（§3.1）。`io_controller_drv` が
`ei2c_read_reg_separate` / `ei2c_write_reg` / `ei2c_get_tuner_hst111ah_S` 等を直接
呼んでおり、blur が ioctl するのは `/dev/io_controller` の方。∴ `/dev/fe_main` は
**このアプリが使っていない並行インターフェース**であって、機能が無いのではない。

**品種の語彙が .ko のファイル名と食い違う。** 実機で load されるモジュール群の
シンボルは `tc90512`（復調）と `hst111ah`（チューナ・S/T 両方）を語るが、
ファイル名側は `demod_tc90522_90532_drv` / `tuner_MxLinear_MxL135RF_drv` で、
そちらは **load されない**（§2）。`fe_common_drv` の init ログ文字列には
`selectDemod1` / `selectDemod2` / `selectTuner1`〜`4` / `use_ofdm` / `use_psk` /
`i2c_port` があり、**1 つのバイナリが実行時の設定で多機種を賄う**作りと分かる。
∴ **本機に載っている実物は、シンボルからは確定できない**（分解写真は `MXL683`）。

### 2.2 本機のフロントエンドは `blur` 内の MxLWare68x が制御する（2026-09-21）

`blur` の可読文字列に **`MxLWare68x_API_*` が 154 箇所・41 種**ある。MaxLinear の
MXL68x 系ホストライブラリで、**チューナ系と復調系の API が同じライブラリに同居**
している（`CfgTunerChanTune` / `CfgTunerAGC` と `CfgDemodMPEGOut` / `ReqDemodCNR`）
＝**1 チップで RF と復調を兼ねる部品**であることの裏づけ。基板写真の `MXL683` と整合する。

初期化から受信までに現れる API（名前のみ。呼び出し順は未観測）:
`CfgDrvInit` → `CfgDevPowerSupply` → `CfgDevXtal` → **`CfgDevFirmwareDownload`** →
`CfgDevInit` / `CfgDevOperationalMode` → **`CfgTunerChanTune`** → `CfgDemodMPEGOut` →
`CfgDemodBerPer`。状態取得は `ReqTunerLockStatus` / `ReqDemodLockStatus` /
`ReqDemodCNR` / `ReqDemodBER` / `ReqDemodTMCCParams` / `ReqTunerAGCStatus` ほか。
`blur` 内にはさらに薄い制御層 `MxL68x_Ctrl_{Initialize,SetSource,SetFreq,GetCN,
GetBER,GetAGC,GetTMCC,SetFastScan}` がある。

#### MXL683 について公開されている技術情報（2026-09-21 調査）

メーカー（MaxLinear）の公開ページから取れる事実。**出所が明確なので段階 C の
根拠に使える。**

| 項目 | 値 |
|---|---|
| 構成 | ISDB-T/SBTVD-T の**単一チップ受信機**。デジタルチューナ＋13 セグメント復調器＋**組み込み CPU**＋**シリアル MPEG TS 出力** |
| 制御 | I2C 互換のデジタル制御インターフェース |
| 同調範囲 | **44 MHz 〜 885 MHz** |
| インタリーブ | IL-2 / 4 / 8 / 16 |
| 電源・封止 | 3.3V 単一または 3.3V/1.8V の 2 電源、QFN48 7×7mm、65nm CMOS |

**この 3 点が観察の裏づけになる**:

1. **組み込み CPU がある** → `CfgDevFirmwareDownload`（§2.2）が要る理由。起動時に
   `0x60` へ流れる 48 バイトのブロック列（§4.3）は、この CPU 向けのコード。
   ∴ 段階 C でもファームの転送は避けられない。
2. **TS 出力はシリアル** → SoC 側の「TS SideA 入力」が `GPIO[0〜1]` と `GPIO[9〜11]`
   の 5 本のシェアピン（§9.2）であることと整合する（クロック・データ・同期・有効
   …のシリアル TS インターフェース）。
3. **同調範囲 44〜885 MHz** → 1/64 MHz 単位なら 2816〜56640 で**16bit に収まる**。
   ∴ `0x3028`（上位ワード）が常に 0 なのは偶然ではなく、**全同調範囲で 0 のまま**
   と考えてよい（§4.3 の推定の裏づけ）。

**データシート・レジスタ表は公開されていない。** メーカーのページは
「myMxL アカウントで登録またはログイン」を要求し、技術文書はすべてその先にある。
**登録すれば入手できるのか、NDA が要るのかは未確認**。もし入手できれば、
§9.1 の A3/A4（レジスタの意味）は観察で詰める作業ではなく**読むだけ**になるので、
§9.4 のとおり入手経路は一通り当たって空振りだった。

#### MXL683 のオープン実装は存在しない（2026-09-21 調査・否定的結論）

段階 C の重さを左右するので調べた。**使えるものは無い。**

| 見つけたもの | 中身 | 使えるか |
|---|---|---|
| HiSilicon STB SDK の第三者ミラー（`HiSTBLinuxV100R005C00SPC0xx` 等 8 リポジトリ）の `source/msp/drv/frontend/demod/MXL68x/` | `MxL68x.c` / `_Main.c` / `_PhyCtrlApi.{c,h}` / `_PhyCfg.c` / `_OEM_Drv.{c,h}` / **`_FwDownload.c`**。`MxLWare68x_API_*` が 45 種、ISDB-T と `mxl683`/`mxl681` に言及 | **不可**。全ファイルが `Copyright (c) 2011, MaxLinear, Inc.` のみで**許諾文が 1 行も無い**（`MODULE_LICENSE` 無し、リポジトリに LICENSE/COPYING 無し）＝GPL ドロップに紛れ込んだベンダ SDK |
| Amlogic カーネル各種（`linux-amlogic` / `linux-meson` / `vero2-linux` / `vero3-linux`） | `CONFIG_AM_MXL683_FE` と `AM_DTV_DEMOD_MXL683` の**宣言だけ**。Makefile の `obj-$(CONFIG_AM_MXL683_FE) += mxl683/` は**コメントアウト**され、`mxl683/` ディレクトリは公開物から除かれている | **不可**（実体が無い） |
| mainline `drivers/media/dvb-frontends/mxl692.c` | `SPDX-License-Identifier: GPL-2.0`。「Copyright (c) 2016 MaxLinear, Inc. … **which was released under GPL V2**」 | **参考のみ**。ATSC/QAM 専用で **ISDB-T を扱わない**。別世代・別ファミリ |

GitHub のコード索引に `mxl683` という名のファイルは **0 件**。

**含意**: (a) MaxLinear は MxL69x では GPLv2 で出した前例があるが、MxL683（2012 年の
部品）については公開実装が無い。(b) ∴ 段階 C の復調側は
**観察（バス上のバイト列）＋データシートから起こすしかない**。上の MaxLinear
コードは許諾が無いので、**読んで実装の出所にしてはならない**（clean-room 以前に
ライセンスの問題）。

**段階 C への含意（重い）**:

1. 本機の復調は **TC90522 系のレジスタ列ではなく MXL68x 系のシーケンス**。
   ∴ mainline の GPL `tc90522.c` は**この機種の意味論の出所にならない**。
2. **`CfgDevFirmwareDownload` があり、チップ内 RAM にファームを流し込む必要がある。**
   `/lib/firmware` も `.fw` も rootfs・app のどちらにも無いので、実体は `blur`
   バイナリの中にある公算が大きい。∴ 段階 C でも**所有者の機器上の blob を実行時に
   使う**形になる（§1-5 と整合。配布物には入れられない）。
3. スレーブアドレスは静的文字列に無く、`slave addr(OFDM) = 0x%x` として実行時に
   出るだけ。**実測で `0x60` に確定**（§4.1）。

なお `blur` は **`DEMOD_*` の ioctl 名を 24 種すべて文字列として持つ**
（`DEMOD_SET_CHANNEL` / `DEMOD_GET_CN` / `DEMOD_ACCESS_DEMOD_REG` …）。
§2.1 の `fe_main_drv` の `FE_*` 関数群と対になる名前で、**コードは持っているが
本機では使っていない**（§3.1）。

## 3. ユーザランド ↔ カーネルの境界（これが契約）

実機で `/dev/*` を開いているプロセスを全 PID 走査した結果、**受信系の chardev を
開くのは `blur` だけ**で、その内訳は:

| chardev | 用途（推定） |
|---|---|
| **`/dev/fe_main`** (220,0) | **選局・ロック状態**。fd は blur の全スレッドに複製 |
| `/dev/val_demux`, `/dev/val_demux1` (231,0/1) | TS デマルチプレクサ |
| `/dev/mem2` (245,0) | バッファの mmap（blur の `maps` に `/dev/mem2` が出る） |
| `/dev/mem` (1,1) | 物理メモリ直（blur の `maps` に 2 本） |
| `/dev/io_controller` (221,0) | ボード I/O |
| `/dev/val_*` (230,*) | AV サブシステム全般（表示・録画・符号化…） |

**開かれていない**もの: `/dev/ei2c`、`/dev/i2c-0..3`、`/dev/h57fmb0`、
`/dev/tsout_demux`。

### 3.1 `/dev/fe_main` は開かれるだけで使われない（2026-09-21・トレースで確定）

`blur` に ioctl トレーサを被せて起動から観察した結果（手順は §8）:

- `blur` は起動時に `/dev/fe_main` を **open するだけ**で、その後 **ioctl を 1 度も
  発行しない**。地上波の選局を 5 回成功させても 0 件のまま。
- ∴ 「選局は `fe_main` の ioctl 契約」という当初の見立ては**誤り**。

選局中にユーザランドから出る操作は次の 2 つだけだった（定常の 200ms ポーリングを除く）:

| chardev | type | 内容 |
|---|---|---|
| `/dev/val_demux` | `0xc3` | 40 B 引数。**PID フィルタの設定**（後述） |
| `/dev/io_controller` | `0xb3` | 4 B 引数＝**ユーザ空間アドレス**。メッセージ/イベントキュー |

`io_controller` の nr は `0x01`〜`0x06`。うち `0x05`(W) と `0x06`(R) は 4 B の
**ポインタ**を渡す／返す。指す先を 64 B 展開すると、先頭 8 B が可変のコード、
9-12 B目が**自分自身のアドレス**（自己参照＝キュー要素）、以降にヒープ/mmap 領域と
思われるアドレスが並ぶ。**周波数の値はこの 64 B には現れない**。

`nr=0x01`〜`0x04` は 200 ms 周期の定常ポーリング（`0x01`/`0x02` は常に 1、
`0x03` が `0x13`〜`0x16` を書き `0x04` が `0x11` を読む）＝前面パネル相当の
背景動作で、選局とは無関係。

`val_demux` 側で選局に伴って現れるのは `nr=0x0c/0x0d/0x0e/0x15/0x16/0x17/0x2e/0x2f/
0x33/0x34/0x6a`。引数は 40 B で、先頭 4 B に `00 10 00 21` `00 10 00 1f`
`02 10 00 6a` のような値が入る＝下位 16 bit が **PID**（0x0021=PAT 近傍, 0x006a 等）、
上位がフラグと読める。**選局の結果として TS の PID を拾い始める段**であって、
選局そのものではない。

∴ **周波数指定がユーザランド ↔ カーネルの syscall 境界を渡っていない。**
残る経路は `blur` が mmap している `/dev/mem2` / `/dev/mem` の共有メモリで、
`io_controller` の ioctl はその呼び鈴（doorbell）にすぎない、という形である。

### 3.2 段階 C への含意（重要）

ベンダのユーザランド ↔ カーネル間プロトコルを解くことは、**段階 C の前提ではない**。
OpenIRD が作るのは `fe_main`/`io_controller` の代替ではなく、**ハードウェアを直接
叩く自前ドライバ**だからである。必要な事実は次の 2 つに絞られる:

1. 復調器とチューナの **品種・I2C バス・スレーブアドレス・アクセス手順**。
2. 復調器が出した TS が SoC のどこに入り、どう読み出されるか（`csd`/`val_merge` の
   AV サブシステム）。ここだけが proprietary で、観察でしか辿れない。

∴ 次は `io_controller` のポインタ追跡を深掘りするのではなく、**I2C バスの直接
探索**に進んだ。結果は §4.1＝**バス 1 で応答するのは `0x60` ただ 1 つ**で、
§2.1（チューナは復調器のゲート越し）と合わせると `0x60` が復調器と読める。
品種とアクセス手順が未達で、§7-1 が最優先の課題になった。

静的ノードだが未使用のもの（rootfs に居るだけで `/proc/devices` に登録が無い）:
`/dev/tsout_demux` (210,0)、`/dev/i2c-2`、`/dev/i2c-3`。

`blur` のリンク: uClibc 0.9.31 / libdl / libgcc_s / libm / libpthread / libresolv /
librt ＋ `libcrypto.so.1.0.0` `libssl.so.1.0.0` `libjpeg.so.8.0.2` `libz.so.1.2.5`
＋ `/app/bin/libtrmp_values.so`。**受信用の別 .so は無い**＝受信のユーザランド側は
`blur` 本体に入っている。

## 4. I2C トポロジ

`i2c_emma`（GPL v2 / Renesas）が Linux 標準の i2c アダプタを **2 本**生やす。

| アダプタ | `name` | IRQ | 起動からの割り込み数 | 用途 |
|---|---|---|---|---|
| `i2c-0` | `emma-iic`（`emma_i2c_0`） | 64 | 0 | 未使用 |
| `i2c-1` | `emma-iic`（`emma_i2c_1`） | 65 | 多数（背景 約 3/s） | **選局系。実装は `0x60` 1 個だけ**（§4.1） |

**根拠**: 選局 1 回（§5）で IRQ 65 が **+2664** 跳ねた。背景レートの数百倍。
`i2c-0` は選局前後とも 0 のまま。

`/sys/bus/i2c/devices/` には `0-0000` / `1-0000` が居るだけでスレーブの登録は無い
（ドライバがアダプタを直接使っている）。実装されているアドレスは §4.1 で走査した。

`ei2c_ofdm_drv` は `i2c_emma` に依存しない独立のモジュールで、`/dev/ei2c` を生やすが
誰も開いていない。標準 i2c との役割分担は §4.1 で一部判明した。

### 4.1 バス 1 を自前で走査した結果（2026-09-21・`tools/yb1-i2c`）

標準の i2c-dev ABI（`I2C_RDWR`）で `/dev/i2c-1` を直接叩けることを実機で確認した。
**ベンダのインターフェースを介さずに済む**＝段階 C にとって重要な足場。

- **応答するのは 7bit `0x60` ただ 1 つ。** `0x03`〜`0x77` を 2 通りで走査した:
  ベアリード 1 バイト、およびレジスタ 0x00 を書いてからの読み出し。どちらでも
  `0x60` 以外は NAK。
- `0x60` からの読み出しは素直なレジスタファイルに見えない。連続 8 バイト読みが
  `00 08` / `00 04` / `00 00` の 2 バイト周期を返し、**選局しても周波数に追随しない**
  （503/473/500/557 MHz で比較）。適切なアクセス手順が別にあると考えられる。
- **バス 0 は触ってはいけない。** 何も載っておらず、転送がエラーで返らず
  アダプタ内でブロックする（IRQ 64 は 0 のまま）。`tools/yb1-i2c` は行ごとに
  出力を吐くので、殺しても途中までは残る。
- 走査は `dmesg` に `-----> I2C force Stop bit` を残すが、これは NAK からの正常復帰。
  走査後も `blur` は動き続け、受信も変わらなかった（503→`Cn` 33）。

**含意**: 選局時に IRQ 65 が跳ねる（§4）のに、バス 1 には `0x60` しか居ない。
∴ 選局のバストラフィックは `0x60` 宛てである。そして §2.1 で **チューナは復調器の
I2C ゲートの向こうに居る**と分かったので、**`0x60` が復調器**で、チューナは
バス上に直接は現れない、と読むのが整合する。

`0x60` の品種は未確定。素直なレジスタ読みでは中身が出ないので、正しいアクセス
手順（サブアドレス方式など）が要る。

### 4.2 バスを直接ログする（2026-09-21・成立）

`blur` は `/dev/i2c-N` を開かない（§3.1）ので、ユーザランドからはバイト列が採れない。
そこで **GPL の i2c アルゴリズムドライバ `i2c-algo-emma` を自前でビルドし直し、
転送ログを足して差し替えた**。ベンダのモジュールには一切触れていない。

成立の条件は 3 つとも満たされている:

- ベンダの `.ko` は **`MODVERSIONS=n`**（`__versions` セクションが無い）＝vermagic が
  合えば載る。
- 実機の `.config` が **`/proc/config.gz`** から取れる（`CONFIG_IKCONFIG_PROC=y`）。
  GPL ドロップに emma3sep の defconfig は無いので、これが無ければ詰んでいた。
- 自前ビルドの vermagic（`2.6.27.39 preempt mod_unload MIPS32_R2 32BIT`）と
  **未定義シンボルがベンダ `.ko` と完全一致**することを機械で確認した。

差し替えは**先に load した方が勝つ**性質を使う。`rcS` の OpenIRD ブロック
（`setup.sh` の直前）で自前の `i2c-algo-emma.ko` を `insmod` しておくと、後から
`blur` が入れるベンダ版は `EEXIST` で失敗し、ベンダの `i2c-emma.ko` は**自前の
algo に bind する**。∴ app 区画（mtd4）には手を触れずに済む。

- 手順とビルドは `scripts/build-yb1-kmod.sh`、patch は
  `br-ext-chip-renesas/board/emma3sep/kernel/patches/`。
- ログは既定で**無効**（module param `trace=0`）。有効化は
  `/app/setting/openird-i2ctrace`（永続区画）を置いて再起動、または
  `/sys/module/i2c_algo_emma/parameters/trace`。
- カーネルのログバッファは **16KB しかない**（`CONFIG_LOG_BUF_SHIFT=14`）ので、
  採取中は実機側で `dmesg -c` を 20ms 間隔で回して tmpfs に吸い出す
  （busybox の `sleep` は小数を取らない。`usleep 20000` を使う）。

### 4.3 MXL683 のレジスタアクセス方式（2026-09-21・実測）

選局 1 回（地上 503MHz・ロックまで）で **826 メッセージ**、すべて `0x60` 宛て。
内訳は **4 バイト書き 432 回 / 2 バイト読み 394 回**。並びから形が読める:

```
W addr=0x60 len=4  ff fb a8 0a     ← 読みたいレジスタ番号を 0xfffb に書く
R addr=0x60 len=2  00 00           ← 値が 2 バイトで返る

W addr=0x60 len=4  30 1e 00 00     ← 書き込みは [reg_hi reg_lo val_hi val_lo]
W addr=0x60 len=4  30 03 0f 89
```

∴ **レジスタは 16bit アドレス・16bit 値**。書き込みは 4 バイト 1 トランザクション、
読み出しは「`0xfffb` に対象レジスタ番号を書く → 2 バイト読む」の 2 トランザクション。
`0xfffb` は読み出し先を選ぶ特別なレジスタとして働いている。

起動時（選局前）には同じ `0x60` 宛てに **48 バイトの書き込みが連続**する。先頭が
`e4 02 … 2c` `e5 02 … 2c` `e6 02 … 2c` と 1 ずつ進むブロック列で、
`MxLWare68x_API_CfgDevFirmwareDownload`（§2.2）＝**チップ内 RAM へのファーム転送**と
みて整合する。

これで段階 C に必要な「バス上の事実」を採る経路が開いた。

#### 選局 1 回の中身（2026-09-21・実測）

`0xfffb` への書き込み（読み出し先の選択）を除くと、**実際に触る制御レジスタは
21 種・書き込みは 30〜42 回だけ**で、残り（356〜478 回）はすべてステータス読み＝
ロック待ちのポーリングだった。503 MHz の全書き込みを順に並べると:

```
 [ 0] 0xe028 <- 0x0000
 [ 1] 0x301e <- 0x0000      [ 2] 0x301f <- 0x0000
 [ 3] 0x3002 <- 0x0000      [ 4] 0x3003 <- 0x0f89
 [ 5] 0x0012 <- 0x0000
 [ 6] 0x3028 <- 0x0000      [ 7] 0x3029 <- 0x0000     ← 周波数を 0 に落とす
 [ 8] 0x007c <- 0x001f      [ 9] 0x0131 <- 0x0081
 [10] 0x0049 <- 0x001c      [11] 0x00db <- 0x0017
 [12] 0x00ea <- 0x0000      [13] 0x00eb <- 0x00d8
 [14] 0x00b6 <- 0x0040      [15] 0x015f <- 0x0037     [16] 0x0160 <- 0x0037
 [17] 0x3029 <- 0x7dc9      ← ★周波数（下位）
 [18] 0x3028 <- 0x0000      ← 　周波数（上位・推定）
 [19] 0xe033 <- 0x0000
 [20] 0x101c <- 0x0004
 [21] 0x101b <- 0x0000      [22] 0x101b <- 0x0001     ← 起動トリガ（推定）
 [23..] 0xe028/0xe029 <- 0x0000 を交互に 10 回ほど      ← メールボックス/状態クリア（推定）
```

周波数を変えて 4 回採り、**値が変わるのは `0x3029` だけ**だった
（`0xe028`/`0xe029` は回数が違うだけで値は常に 0）。

#### ★ 周波数レジスタ＝`0x3029`、単位は 1/64 MHz（検証済み）

| 設定 | `0x3029` に書かれた値 |
|---|---|
| 473 MHz | `0x7649` |
| 503 MHz | `0x7dc9` |
| 509 MHz | `0x7f49` |
| 557 MHz | `0x8b49` |

差分は 6 MHz あたり 0x180 = 384 ＝ **1 MHz あたり 64**。定数項は +9 で、これは
ISDB-T の中心周波数オフセット **1/7 MHz = 142.857 kHz** が 1/64 MHz 単位で 9.14 に
なることに対応する。∴

```
reg 0x3029 = round(f_center_MHz * 64)        単位 = 1/64 MHz = 15.625 kHz
  地上波は f_center = X + 1/7 （X は整数 MHz の呼び値）
```

**検証**: 未観測の 545 / 479 / 521 MHz について先に値を予測し、その後で実機に選局
させて突き合わせた。**3/3 一致**（`0x8849` / `0x77c9` / `0x8249`）。479 MHz は
ロックしない（`Ber` 99999）が値は同じく書かれた＝**指令値であって測定値ではない**。

`0x3028` は常に 0 で、`0x3029` と対になる 32bit 値の上位ワードと読むのが自然。
メーカー公開の同調範囲 **44〜885 MHz**（§2.2）は 1/64 MHz 単位で 2816〜56640＝
**16bit に収まる**ので、上位が 0 なのは観測した帯域に限った話ではなく
**全同調範囲でそうなる**と考えてよい。32bit として扱う設計であること自体は未確認。

#### ステータス読み側（2026-09-21・実測）

`X_GetOfdmTunerInfo` を 1 回だけ呼び、その前後のログを切り出すと、**その呼び出しが
引き起こした読みだけ**が残る。地上 10 チャンネルで採った結果:

読まれるレジスタは **20 種**。うち 16 種は全チャンネルで値が動かない
（`0x002b`=7, `0x1147`=223, `0x112e`=1, `0x101c`=4, `0x1148`=14, `0x1129`=31,
`0x002c`=8, `0xa8b4`/`0xa8c6`/`0xa8c8`/`0xa8ca`=0 ほか）。動くのは 3 種だけ:

| レジスタ | 値の範囲 | Cn との相関 |
|---|---|---|
| **`0x112d`** | 567〜1094 | **−0.984** |
| `0x0034` | 16 / 17 | −0.580（Agc と +0.786） |
| `0x0033` | 7〜238 | +0.428（毎回大きく振れる） |

**★ `0x112d` が C/N の出どころ。** 値が小さいほど C/N が良い（雑音側の量）。

```
  0x112d:  567  639  675  734  737  839  840 1000 1051 1094
  Cn(dB):   33   32   31   31   31   30   30   29   28   28
```

**換算式は未確定。理由も分かった。** アッテネータを入れて範囲を広げようとしたが
（§5.5）、C/N は 1 dB ほどしか動かず 27〜33 dB のまま（raw 536〜1332）。
`Cn = K − 10log10(raw)` の K は 58.10〜60.39 と 2.3 dB ばらつく。

ばらつきの正体は **`0x112d` が瞬時値で、`Cn` は平滑された値**であること。同じ
チャンネルを 2 回測ると raw は 869 → 646、967 → 1089 と大きく動くのに `Cn` は ±1 しか
動かない。∴ 両者は同時刻の対ではなく、この採り方では式を当てられない。

**段階 C への実務的な結論**: chardev の品質値としては `0x112d` の単調性で足りる
（値が小さいほど良い）。dB への正確な較正が要るなら、固定条件で raw を多数回
平均する測定系か、信号発生器が要る。

**★ `SignalStrength` は独立した測定値ではない。** 10 点すべてで

```
  SignalStrength = 3 * Cn - 10
```

が厳密に成立した（31→83, 33→89, 32→86, 30→80, 29→77, 28→74）。∴ 段階 C の
chardev は **C/N だけ持てばよく**、`SignalStrength` 相当はそこから計算できる。

`Agc`（2〜4）は `0x0034`（16 または 17）と向きは合うが、値が 2 種類しか観測
できておらず対応は確定していない。`0x0033` は呼ぶたびに大きく振れるので、
自由走行のカウンタか高速 AGC の瞬時値と見られる。**いずれも未確定。**

残るのは制御側 19 レジスタの意味、チップ内 RAM へのファーム転送（§2.2）、
そして TS の取り出し口（§7）。

### 4.4 検討して否定した筋（記録）

- **PIIC2 説は否定**。SoC の I2C は 3 本ある（u-boot `emmaregs.h`:
  `PTOP_PIIC0/1/2_BASE_OFS` = `0x7000`/`0x8000`/`0x9000`、`PBRD_BASE=0x10100000`
  ∴ `0x10107000`/`0x10108000`/**`0x10109000`**）。u-boot は
  `CONFIG_SYS_MAX_I2C_BUS 3` で 3 本使うが、**Linux 側のヘッダは PIIC0/1 しか
  定義せず** `platform.c` も 2 本しか登録しない。そこで「`ei2c_ofdm_drv` が
  PIIC2 を直接叩いているのでは」と考えたが、**同モジュールのシンボルが標準の
  i2c クライアント API そのもの**だったので否定した（§2.1）。IRQ も
  `EMMA_IRQ_INT(56)/(57)`＝実機表示 64/65 の 2 本だけで、58 に当たる 66 は無い。
- **`/dev/mem` から I2C のレジスタは覗けない**。この カーネルの `/dev/mem` は
  RAM しか許可せず、`0x10107000` を読むと `Bad address`（EFAULT）。RAM 側
  （`0x00030000` の EMMA ファーム）は読める。
- **UPnP `X_SetLoggingLevel` は 1 と 2 しか受け付けず**（他は 402）、受理しても
  ドライバ側のログは出ない。`fe_common_drv` が持つ
  `demod_saddr=%#.2x tuner_slv=%#.2x …` の printk を出させる経路ではない。

## 5. 外部からの駆動系＝ベンダ UPnP（観察の足場）

`http://<device>:49152/upnp/control/X_AirTunerControl`（**無認証**）。
サービス記述は `/app/bin/dms_resource/X_AirTunerControl.xml`。

観察に効く action:

```
X_SetTuning(BroadcastWave: "tb"|"bs"|"cs", Frequency: int)
X_GetOfdmTunerInfo() -> Frequency, SignalStrength, Cn, Ber, Agc   (すべて int)
X_SetTunerSetting(Attenuator, PowerSupply)
X_StartChannelScan(BroadcastWave, Range, FrequencyList, TransportStreamIDList,
                   ScanType: "initial"|"rescan", AreaCode) -> ReportContainerID, ScanID
X_GetStateVersatile() -> LastObjectID, TbObjectID, BsObjectID, CsObjectID,
                         RecordState, BroadcastWave, ServiceId, StorageState, FwVersion
```

### 5.1 `Frequency` の単位＝MHz、フィールドは 16bit

`X_SetTuning` に渡した値がそのまま `X_GetOfdmTunerInfo` の `Frequency` に返るが、
**2^16 で切り詰められる**。

| 設定値 | 返り `Frequency` | 設定値 mod 65536 |
|---|---|---|
| 557142857 | 21321 | 21321 |
| 557143 | 32855 | 32855 |
| 473142857 | 38473 | 38473 |
| 473143 | 14391 | 14391 |
| 557 | 557 | 557 |

∴ **`Frequency` は MHz の `uint16`**。ISDB-T の実中心周波数は `X + 1/7` MHz だが、
整数 MHz（473 等）でもロックする＝復調器の AFC 引き込み範囲内。

### 5.2 実測（地上波・整数 MHz で掃引）

```
  set  freq   ss   cn     ber  agc
  500   500    0    0   99999    3     <- チャンネル間の谷
  557   557   80   30       0    4
  476   476    0    0   99999    3     <- 谷
  473   473   86   32       0    3
  503   503   89   33       0    2
  509   509   86   32       0    2
  515   515    0    0   99999    3
  521   521   83   31       0    2
  527   527   86   32       0    2
  533   533   77   29       0    3
  539   539   74   28       0    3
  545   545   74   28       0    4
  551   551   80   30       0    3
  563   563    0    0   99999    6
  569   569    0    0   99999    6
```

- **ロック時**: `SignalStrength` 74–89 / `Cn` 28–33 / `Ber` 0 / `Agc` 2–4
- **非ロック時**: `SignalStrength` 0 / `Cn` 0 / **`Ber` 99999**（センチネル）/ `Agc` 3–6
- `Cn` は dB とみて妥当な範囲。`Ber` は 0 か 99999 の 2 値しか観測していない。

**段階 C の chardev が出すべき値はこの 5 つ**（px4_drv 系の上位が求めるのは
C/N とロック状態）。

### 5.3 選局セッションは失効する

`X_GetOfdmTunerInfo` は **`X_SetTuning` の直後しか応答しない**。選局せずに 30 秒
置くと `UPnPError 501 Action Failed` を返す（起動直後・未選局でも同じ）。
∴ 観察では「選局 → すぐ読む」を対にする。

### 5.5 現在の設定は ContentDirectory から読める（2026-09-21）

`X_SetTunerSetting` には対になる Get が無いが、**ContentDirectory の
ObjectID `321`**（`32` = "System Info" の下、`object.item.textItem.tunerInfo`）に
現在値が出る。∴ 設定を変える実験を**読み戻して確かめ、確実に戻せる**。

```
Browse(ObjectID=32, BrowseDirectChildren) →
  320  object.item.textItem.cardInfo     （BCAS カード情報。§0 によりスコープ外）
  321  object.item.textItem.tunerInfo
        deviceName / serialNumber(伏字) / fwVersion
        attenuator  = 0    ← 現在値
        powerSupply = 1    ← 現在値
        storageState / storage*(容量) / areaCode ほか
```

**`X_SetTunerSetting` の綴りは `ON` / `OFF`（大文字）**。実測した挙動:

| 渡した値 | 応答 | 実際 |
|---|---|---|
| `false`/`true`, `off`/`on`, `0`/`1`, `disable`/`enable`, `no`/`yes` | **402** Invalid Args | 変化なし |
| **`OFF`/`ON`** | **501** Action Failed | **適用される** |

- 402（引数が不正）と 501（引数は解釈できたが失敗）の違いが綴りを教えてくれた。
- **501 を返すのに設定は効く**（`attenuator` 0→1、戻すと 1→0 を ObjectID 321 で確認）。
  呼び出し側から見ると失敗に見えるので注意。
- **選局セッションが生きている間でないと効かない**。`X_SetTuning` の直後に投げる
  必要がある（セッション無しだと 501 を返して何も起きない）。
- 出力は `0`/`1`、入力は `OFF`/`ON` と**表記が違う**。
- `PowerSupply` はアンテナ給電。触らないときは読んだ現在値に対応する綴りを渡す。

この機種ではアッテネータを入れても C/N は 1 dB ほどしか動かなかった（§4.3）。

### 5.7 純正 PC クライアントのキャプチャから確定した事実（2026-09-21）

初期設定のチャンネルスキャンから NHK 総合の視聴開始までを、PC クライアントと
実機の間で採ったパケットキャプチャ（`tshark` で解析）。**UPnP 制御は 49152 番の
平文 HTTP** なので、要求も応答もそのまま読める。

#### `X_StartChannelScan` の実引数

```xml
<BroadcastWave>tb</BroadcastWave>
<Range>0</Range>
<FrequencyList>0</FrequencyList>
<TransportStreamIDs>0</TransportStreamIDs>   ← SCPD の宣言は TransportStreamIDList
<ScanType>initial</ScanType>
<AreaCode>23</AreaCode>
→ <ReportContainerID>33201</ReportContainerID><ScanID>1</ScanID>
```

- **引数名が SCPD の宣言と食い違う**（`TransportStreamIDs` vs `TransportStreamIDList`）。
  それでも通るので、実装は名前ではなく順序で読んでいると見られる。
- `Range` / `FrequencyList` / `TransportStreamIDs` はいずれも `0`＝全帯域おまかせ。
- 進捗は `ReportContainerID`（33201）を Browse して読む。DIDL の
  `object.item.textItem.progressInfo` が流れ、`tunerState` も併せて更新される。

#### `AreaCode` = **23**（東京）

クライアントの画面で「東京」を選んだときの値。スキャン後、実機の
`tunerInfo`（§5.5）の `areaCode` も **23** になっていることを確認した。

**この符号の全体表は未確定。** `blur` には都道府県名の文字列表があり、並びは
ARIB の地域順（北海道 7 分割 → 東北 → 関東 …）だが、**その添字では東京は 13/14
番目**で 23 にならない。∴ 名前表と符号は別物。`blur` には
`AREA SETTING : areaCode [%d], wideAreaCode [%d]` という出力があり、
県域符号と広域符号の 2 本立てらしい。他エリアの値が要るときは、同じ要領で
クライアントの選択を捕まえるのが確実。

#### スキャン結果＝チャンネル一覧（東京・42 件）

`res` の URL に **周波数(整数 MHz)・TSID・サービス ID** がそのまま入る:

```
http://<ip>:50125/live.TB.<freq_MHz>.<tsid>.<serviceId>.dtcp&quality=AVC-MID-RES
```

| 局 | freq(MHz) | 物理ch | TSID | 主なサービス ID |
|---|---|---|---|---|
| NHK 総合 1・2 | 557 | 27 | `0x7fe0` | `0x0400` / `0x0401` |
| NHK E テレ 1〜3 | 551 | 26 | `0x7fe1` | `0x0408`〜`0x040a` |
| 日テレ | 545 | 25 | `0x7fe2` | `0x0410`/`0x0411`/`0x0414` |
| テレビ朝日 | 539 | 24 | `0x7fe5` | `0x0428`〜`0x042a` |
| テレ東 | 533 | 23 | `0x7fe6` | `0x0430`〜`0x0432` |
| TBS | 527 | 22 | `0x7fe3` | `0x0418`/`0x0419` |
| フジテレビ | 521 | 21 | `0x7fe4` | `0x0420`〜`0x0422` |

§4.3 の周波数レジスタの検証で使った周波数と一致する。

#### 視聴開始＝`.at1` はペアリング済みのヘッダが要る

```
GET /live.TB.557.7fe0.0400.at1&resolution=1280x720&videoBitrate=6000,6000,5500,5000&dualMono=LL
  AT-ClientUID: <32 hex>
  AT-ClientID:  <32 hex>
→ HTTP/1.1 200 OK
  Content-Type: video/X-atd1
  Transfer-Encoding: chunked
```

- `.at1` は**この 2 つのヘッダが無いと通らない**（ヘッダ無しの素の GET が 405 に
  なるのはこのため）。値は事前の `X_RequestPairing` → `X_AuthenticatePairing`
  （128〜150 バイトの hex を往復するチャレンジ/レスポンス）で得る。
- **中身は生 TS ではない。** `resolution` / `videoBitrate` を取る＝トランスコード
  済みで、本文は高エントロピー（MPEG-TS の同期バイト列が立たない）＝暗号化されて
  いる。∴ 「UPnP から生 TS は出ない」（§0 の前提）は変わらない。
- ペアリングの中身は DRM 隣接なので**解析しない**。OpenIRD は自前の chardev を
  作る側なので、この経路を再現する必要がそもそも無い。

### 5.6 この個体はチャンネルスキャン未実施（キャプチャ前の状態）

**§5.7 のキャプチャを採る前の状態。** `/app/setting/DTVS_ChannelList.data` /
`DTVS_ServiceList.data` はいずれも 12 バイトの全ゼロ、BS/CS 側も 16 バイトの空
だった。∴ 番組表・サービス ID 経由の経路が使えず、**周波数直指定（§5.1）が
観察の唯一の入口**だった。§4.3 の測定はすべてこの状態で採っている。

その後 §5.7 でスキャンを実施したので、現在はチャンネル一覧が入っている。
観察を同じ条件で再現したいときは、この違いに注意する。

## 6. EMMA の裏 CPU ファーム

`/app/bin/setup.sh`（平文）と mtd5（`EMMA_FW`, squashfs）より:

```
mount -t squashfs /dev/mtdblock5 /tmp/emma_fw
./emma3sep_driver_std_op0_load.sh      # = fw_load.sh emma3sep_driver_std_op0.mem 0x00030000
    -> dd if=emma3sep_driver_std_op0.mem of=/dev/mem seek=$((0x30000/1024)) bs=1024
```

**物理 0x00030000 に 2,734,892 バイトを置くだけ**（ヘッダ・署名・復号なし）。
AV サブシステム（`csd` / `val_merge` / `CSD_*` 割り込み）はこのマイクロコードで動く。

段階 C でもこの blob は**所有者の機器上のものを実行時に使う**形になる（§1-5 と整合）。

続けて `setup.sh` が行うのは `insmod h57fmb.ko` →
`dione_transcode -w -P <DIONE_SEL_PWR_CTRL>`（`PIX_AIRTUNER_LITE` では 1）→
`insmod 8192du.ko` → `cmd_executor_server &` → `blur --rootfs=FLASH`。
**受信系の .ko を insmod する行は `setup.sh` に無い**＝`blur` 自身が入れている。

`h57fmb`（Dione MB86H58A＝トランスコーダ）は USB VID `0x06b8` PID `0x102d` で
列挙された直後に disconnect する（`dione_transcode -w` が SPI-ROM を書いて落とす）。
**生 TS の経路には要らない**見込み。

## 7. 未解明・次

1. ~~MXL683 のアクセス手順~~ — **解決**（§4.3）。レジスタは 16bit アドレス・16bit 値、
   読み出しは `0xfffb` 経由。バス上のバイト列は GPL の `i2c-algo-emma` を自前で
   建て直して採る（§4.2）。**周波数レジスタ `0x3029`（単位 1/64 MHz）も検証済み。**
2. **C/N の換算式**。`0x112d` が出どころだと分かった（§4.3）が、観測できた範囲が
   28〜33 dB と狭く、対数則と直線則を区別できない。範囲を広げる手段が要る。
   `X_SetTunerSetting(Attenuator, PowerSupply)` でアッテネータを入れられそうだが、
   **許容値の宣言が無く、読み出す action も無く、同じ action でアンテナ電源も
   指定する**ので、値を探る前に扱いを決める必要がある。
3. **`Agc` の出どころ**。`0x0034` と向きは合う（相関 +0.786）が 16/17 の 2 値しか
   観測できていない。`0x0033` は毎回大きく振れる。どちらも未確定。
4. **残り 19 の制御レジスタの意味**。選局で触る制御レジスタは 21 種しかないので、
   1 つずつ（帯域・モード・TS 出力…）を切り分ける余地がある。`fe_main_drv` の
   `FE_debug_show_demod_reg_all` / `FE_set_debuglevel` と `fe_common_drv` の
   `demod_saddr=…` printk も使える見込み（`/dev/fe_main` は blur が使っていないので
   試しても受信を乱しにくい。ioctl 番号の割り出しが課題）。UPnP でログレベルを
   上げる筋は否定済み（§4.4）。
5. **MXL683 のファーム**。`CfgDevFirmwareDownload` が要求する blob が `blur` の
   どこにあり、どう渡されるか（§2.2）。段階 C の実行時依存になる。
6. ~~`/dev/fe_main` の ioctl 契約~~ — **解決（否定的に）**。`fe_main` は使われない（§3.1）。
   周波数は syscall 境界を渡らない。段階 C はこの経路を再現しない（§3.2）。
7. **生 TS の取り出し口**。`/dev/val_demux` + `/dev/mem2` の mmap が本命だが、
   `val_merge` は Proprietary＝観察でしか辿れない。スクランブル解除より**上流**で
   タップできることを確認する必要がある（§0）。
8. ~~`i2c_emma` の GPL ソース未同梱~~ — **誤りだった**。GPL tarball は完全なツリーで、
   i2c の GPL ソースは入っている（§2）。`h57fmb` は GPL 宣言だがソース未確認。

## 8. 観察手順（再現用）

前提: ベンダ rootfs に観察用の入口を足した像が mtd3 に入っていること
（[poe-br100-yb1-nand-update.md](poe-br100-yb1-nand-update.md)）。

1. **シリアル**（COM3 / 機器側 ttyS1, 115200 8N1）で root。
   `/etc/securetty` に `pts/*` が無いため **telnet の root ログインは通らない**
   （busybox `login` が拒否し `Login incorrect`）。
2. バルク観察用に、シリアルから `telnetd -l /bin/sh -p 2323` を起動すると
   認証なしの root シェルが LAN に出る（**書き込み無し・再起動で消える**）。
3. 選局は PC から UPnP（§5）。`X_SetTuning` → 4 秒 → `X_GetOfdmTunerInfo` を対にする。
4. 変化の観測は `/proc/interrupts`（`emma-iic` / `CSD_*`）、`/proc/modules`、
   全 PID の `/proc/*/fd` を選局の前後で取って差分を見る。

### 8.1 ioctl トレース

`tools/yb1-trace`（`scripts/build-yb1-trace.sh` でビルド）を rootfs 像の
`/opt/openird/yb1_trace.so` に入れて焼く。`rcS` が `setup.sh` を呼ぶ直前で
**`/app/setting/openird-trace` があるときだけ** `LD_PRELOAD` を立てる。

- ログは `/tmp/yb1-trace.log`（1 行 = 1 回の `write()` なのでスレッド間で混ざらない）
- フラグ類は `/app/setting`（mtd8・yaffs2 rw）に置く。**更新で上書きされない区画**
  なので焼き直しをまたいで残る:
  - `openird-trace` … トレース自体の有効化
  - `openird-trace-rw` … `/dev/*` への `read()`/`write()` も記録（既定は無効）
  - `openird-trace-noderef` … 4 B 引数のポインタ展開を止める（既定は展開する）
- 設定は**プロセス起動時に 1 度だけ**読まれる。∴ 変更を効かせるには再起動が要る。

### 8.2 ベンダアプリは kill できない（2026-09-21 実機で確認）

`killall blur` を撃つと、数秒後に **ボードごと再起動する**（シリアルに u-boot の
バナーが出る。`/tmp` が消えることでも分かる）。EMMA 内蔵 WDT を養っているのが
`blur` 側のためと考えられる（機種仕様 §2.4）。

∴ **`blur` を別の `LD_PRELOAD` で起動し直す形の観察はできない。** トレーサの設定を
変えるには「フラグを置いて再起動」、トレーサ自体を変えるには「焼き直し」になる。
だからフラグは永続区画に置き、トレーサは設定で振る舞いを変えられるようにしてある。

## 9. 段階 C に足りないもの（2026-09-21 時点）

段階 C の成果物は **px4_drv 互換の chardev**（CLAUDE.md §4.1）。生 TS が読める
キャラクタデバイスを自前で生やすところまでが OpenIRD の範囲。ここまでの観察で
**何が揃い、何が無いか**を並べる。

### 9.0 揃っているもの

| | 状態 |
|---|---|
| フロントエンドの同定 | MXL683（RF＋ISDB-T 復調の 1 チップ）・I2C bus1 の 7bit `0x60`（§2.2, §4.1） |
| I2C の足回り | `i2c-emma` / `i2c-algo-emma` が **GPL v2**。`Origin: gpl-derived` で書ける（§2） |
| レジスタアクセス方式 | 16bit/16bit。読みは `0xfffb` 経由（§4.3） |
| 周波数の指定 | reg `0x3029` = `round(f_center_MHz × 64)`。盲検 3/3（§4.3） |
| C/N の出どころ | reg `0x112d`（単調・逆相関）。`SignalStrength` は `3×Cn−10` の派生値（§4.3） |
| 観察の道具 | バストレース（GPL i2c ドライバ改変）・`yb1-trace`・`yb1-i2c`・モジュールのビルド経路（§4.2, §8.1） |
| 駆動系 | UPnP でベンダアプリに任意周波数で選局させられる（§5） |

### 9.1 フロントエンド（MXL683）の制御に足りないもの

| # | 足りないもの | なぜ要るか | 採り方 |
|---|---|---|---|
| A1 | **チップ内 RAM へのファーム転送**の構造 | これが無いとチップが動かない。**必須** | 起動時トレースの解析（48B ブロックが連番で流れるところまでは観測済み・§4.3）。blob 自体は所有者の機器上のものを実行時に使う（§1-5） |
| A2 | **コールドスタートの初期化列** | 電源投入 → Xtal → FW → ISDB-T モード設定。いま採れているのは「初期化済みの状態からの選局」だけ | 起動からのバストレースは**すでに採れる状態**。未解析なだけ |
| A3 | 残り 19 の制御レジスタの意味 | 帯域・モード・TS 出力の設定 | 条件を変えた差分（§7-4） |
| A4 | **ロック/同期・BER・TMCC・AGC** のレジスタ | chardev が出すべき値。C/N しか割れていない | `X_GetOfdmTunerInfo` の 5 値のうち Cn だけ対応が付いた。Ber は 0 と 99999 しか出ず、Sync/TMCC は UPnP に出ない。`fe_main_drv` の `DEMOD_GET_SYNC` / `GET_TMCC` を突く手がある（§7-4） |
| A5 | C/N → dB の換算式 | 精度が要るとき | 固定条件での多数回平均か信号発生器（§4.3） |
| A6 | 電源・リセットの順序 | 自前で起こすとき | GPL の u-boot ボードファイルに `GPIO[12] = TU_RST#` がある＝**GPL 由来で書ける** |

### 9.2 TS データ経路 ← **最大の壁**

**ここが割れないと chardev は出せない。** 現状わかっていること:

- 復調器が出した TS は、GPL の u-boot ボードファイルによれば
  **`GPIO[0〜1]` と `GPIO[9〜11]` のシェアピン経由で「TS SideA 入力」** として
  SoC に入る（`setup_gpio_PIX_AIRTUNER_LITE.h`）。ここまでは GPL 由来。
- SoC 側で受けるのは **AV サブシステム**＝`csd`（proprietary）＋
  `val_merge`（**Proprietary** 宣言）＋ 裏 CPU マイクロコード（mtd5 から
  物理 `0x00030000` へ転送・§6）。
- `blur` は選局に伴って `/dev/val_demux` に 40B 引数の ioctl を出し、下位 16bit が
  **PID** に見える＝PID フィルタの設定（§3.1）。
- **GPL のレジスタ表に TS / demux / AV のブロックは 1 つも無い**（u-boot
  `emmaregs.h` にあるのは PIIC0/1/2 だけ、kernel `emma3sep.h` にあるのは
  BHIF / BROI / XDMA / Ether / PBRD / TIMER / FLIP(USB) / SATA / NAND / SROM）。
  ∴ **TS 入力とデマルチプレクサは GPL からは一切追えない。**

足りないもの:

| # | 足りないもの | 性質 |
|---|---|---|
| B1 | `val_demux` の ioctl 契約（リング/バッファの取り方） | 観察でしか辿れない（`val_merge` は Proprietary） |
| B2 | 全 PID 素通し（フィルタ無し）の指定方法 | 生 TS には必須。そもそも可能かも未確認 |
| B3 | **スクランブル解除より上流でタップできるか** | §0 の要件。`val_dscram` を通さずに TS を取り出せる必要がある |
| B4 | `val_merge` を経由せず、自前で TS 入力〜DMA を組めるか | **段階 C の分水嶺**（下記） |

**B4 が段階 C の形を決める。** 2 つの道がある:

1. **自前で SoC の TS 入力を叩く** — 最も綺麗だが、GPL にも公開資料にも
   レジスタ情報が無く、裏 CPU マイクロコードの領分。観察（バス・メモリ）だけで
   組み立てることになり、難易度は高い。
2. **裏 CPU マイクロコードを実行時に使い、その口だけを自前で叩く** — ファームは
   所有者の機器上のものを実行時に使う形（§1-5 と整合）。ただし口を開けているのが
   proprietary な `val_merge.ko` なので、**そのカーネル ABI を観察から再実装する**
   必要がある。SB-TV02 の FAPex と同じ構図。

どちらを採るにせよ、まず **B1〜B3 の観察**が要る。`yb1-trace` は `/dev/val_demux`
の ioctl を既に捕まえられているので、**着手はできる**。

### 9.3 土台に足りないもの

| # | 足りないもの | 状態 |
|---|---|---|
| C1 | **GPL からのカーネル再ビルドと起動** | 設定は `/proc/config.gz` から取れ、**モジュールのビルドと差し替えは成立済み**（§4.2）。カーネル本体（uImage）を焼いて起動させる配線は未 |
| C2 | OpenIRD の rootfs（Buildroot・MIPS BE） | `emma3sep_softbank_poe-br100-yb1_defconfig` は骨格のみ |
| C3 | **chardev の ABI 仕様そのもの** | OpenIRD が決める対外契約。px4_drv 互換の形を**別文書に起こす必要がある**（機種非依存）。未着手 |

### 9.4 文書の入手は打ち切り（2026-09-21・否定的結論）

A3/A4（レジスタの意味）を「読むだけ」にできないか、入手経路を一通り当たった。
**すべて空振り。以後この線は追わない。**

| 経路 | 結果 |
|---|---|
| メーカー公開ページ | 仕様の概要のみ（§2.2）。データシート・レジスタ表は無い |
| **myMxL への登録** | 登録しても **MxL683 のフォルダが出ない**。My Documents に並ぶのは MxL1600 / MxL37xx / MxL862xx 系（ネットワーク・PHY 系）だけで、放送・STB 系は付与されない。∴ 登録だけでは届かない |
| GPL の MaxLinear 系ドライバ | `mxl692`(Eagle) と `mxl5xx`(Hydra) はどちらも GPL だが、opcode / PLID ヘッダ付きのメッセージ protocol で**別系統**。本機の固定 4 バイトのレジスタ方式とは合わない |
| ベンダ SDK の第三者ミラー | 見つかるが**許諾文が無い**（後述）。使えない |
| ST の STiH207 リファレンス設計 | MxL683 を載せた設計は実在するが、ドライバの公開ソースは見つからない |
| USB ドングル・linux-media | MxL683 を扱う実装も提案も無い |

∴ **A3/A4 は観察で詰めるしかない。** 幸い道具（§4.2 のバストレース）と手法
（§4.3 の差分）は確立しているので、時間の問題ではある。

なお **9.2（TS 経路）は、仮にデータシートが手に入っても解決しない。** あちらは
MXL683 ではなく Renesas EMMA3SEP 側の話で、GPL にもメーカー公開情報にも
レジスタが無い。

### 9.5 見通し

- **9.1（フロントエンド）は見通しが立っている。** 方式・周波数・C/N が割れ、
  残りは同じ手法（バストレースの差分）で詰められる。A1/A2 は既存の道具で採れる。
- **9.2（TS 経路）が唯一の本質的な未知。** GPL に情報が無く、proprietary な
  AV サブシステム越しになる。ここの見通しが立つまで、段階 C の完成時期は読めない。
- ∴ 次に投資すべきは **B1〜B3 の観察**（`val_demux` の ioctl 契約）であって、
  フロントエンド側の細部ではない。フロントエンドは「詰めれば終わる」が、
  TS 経路は「できるかどうかがまだ分からない」。

