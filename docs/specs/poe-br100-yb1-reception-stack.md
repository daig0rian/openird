# POE-BR100-YB1 受信スタックの構成と境界（段階 B の観察）

<!-- openird-spec
Status: observing
As-of:  実機 POE-BR100-YB1 / ベンダ FW 2.1.17 r3 (2026-09-22)
Impl:   -
Tests:  -
Note:   段階 C（ird-tuner-*）の契約になる予定。観察が続いている
-->

<!-- Open 層: (1) factory コンテナから取り出したベンダイメージの静的観察（ファイル一覧・
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

**`h57fmb` は GPL を宣言しているのにソースが配布されていない。** `/proc/modules` で
`(P)` が付かない唯一のベンダモジュールで（＝GPL 互換ライセンスの宣言）、実機では
**トランスコーダ MB86H58A を USB 越しに扱う**（major 180 = usb、§10.7.4）。
GPL tarball を `h57`・`h58`・`fmb`・`dione` で全件検索しても**該当ソースは 0 件**。
**GPL 順守の欠落として記録する**（入手できれば段階 C のトランスコーダ側が
`Origin: gpl-derived` で書けるようになる）。

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
**実際に登録しても MxL683 のフォルダは出ない**（放送・STB 系は付与されない）。
他の入手経路も一通り当たって空振りだったので、**この線は打ち切った**。経緯と
当たった経路の一覧は §9.4。

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
   **→ 2026-09-22 に確定（§26）**: 全区画を当たって**ファイルとしては存在しない**
   ことを確かめ、`blur` に **MaxLinear の `MBIN` 形式**で埋め込まれていると分かった
   （`Ctrl_DownloadFirmwareInMbin`）。
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
| `/dev/mem2` (245,0) | blur の `maps` に出るが、**map しているのはただのカーネル RAM**＝AV とは無関係だった（§10.6） |
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
`02 10 00 6a` のような値が入る。**選局の結果として TS の PID を拾い始める段**で
あって、選局そのものではない。

> **訂正（§15.2／§19.1／§22.2）**: 当初この先頭 4 B を「下位 16bit が **PID**、
> 上位がフラグ」と読んだが**誤り**。これは**ハンドル**で、
> **`(デバイス << 24) | (種別 << 16) | 番号`**。`0x00100021` は
> 「デバイス 0・種別 0x10（PID フィルタ）・番号 0x21」であって PID ではない。
> **PID は `nr=0x0c` の第 2 引数**に入り、実測すると `0x0000`(PAT) `0x0010`(NIT)
> `0x0011`(SDT) `0x0012`(EIT) `0x0014`(TDT) … と**本物の ARIB の PID**になる
> （§19.1）。

∴ **周波数指定がユーザランド ↔ カーネルの syscall 境界を渡っていない。**

> **補足（§2.2／§14.1）**: その先は**共有メモリではなく I2C** だった。周波数は
> `blur` 内の MxLWare68x が MXL683 のレジスタ `0x3029` に直接書く（§4.3）。
> 実測でも**状態の読み出しは裏 CPU の API を 1 回も使わない**（§14.1）＝
> 復調器は AV サブシステムを通らない別系統。当初ここに書いた「残る経路は
> `/dev/mem2` / `/dev/mem` の共有メモリ」という見立ては外れ
> （`/dev/mem2` はただのカーネル RAM・§10.6）。

### 3.2 段階 C への含意（重要）

> **訂正（§11.6）**: 下の「**ハードウェアを直接叩く自前ドライバ**」という前提は
> **TS 経路については成立しない**。AV のレジスタは**裏 CPU（MIPS 4KE）の領分**で、
> メイン CPU からは見えない（§11.1）。∴ 段階 C は**裏 CPU のファームを実行時に
> 使い、コマンドスタック越しに叩く**形になる（§11.6）。
> **フロントエンド（MXL683）側は下の記述どおり**で、I2C を直接叩く。

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

> **2026-09-22 追記・一部訂正**: **そのとおりだった**（§28 で確定）。ただし
> **`e4` は開始点ではなく連番 4**で、ここでは 16KB のログ環が溢れて途中から
> 採れていた。転送の形は
> [mbin-firmware-container.md](mbin-firmware-container.md) §7 を見ること。

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
- **`/dev/mem` の `read()` では I2C のレジスタは覗けない**。このカーネルは
  `valid_phys_addr_range()` で read/write を RAM に限るので、`0x10107000` を読むと
  `Bad address`（EFAULT）。RAM 側（`0x00030000` の EMMA ファーム）は読める。
  **`mmap()` には同じ制限が無く、レジスタも AV 領域も map できる**（§10.1）。
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
5. ~~**MXL683 のファーム**~~ — **全部解決（§27・§28、別文書 [mbin-firmware-container.md](mbin-firmware-container.md)）**。
   `blur` の `.rodata` に生で 1 個（13,224 バイト）、I2C 転送は 4 バイトヘッダ＋最大 44
   バイトの 305 コマンド、完了の合図は `0xa8cc == 0xabcd`。**A1-a〜A1-d すべて確定。**
6. ~~`/dev/fe_main` の ioctl 契約~~ — **解決（否定的に）**。`fe_main` は使われない（§3.1）。
   周波数は syscall 境界を渡らない。段階 C はこの経路を再現しない（§3.2）。
7. ~~**生 TS の取り出し口**。`/dev/val_demux` + `/dev/mem2` の mmap が本命~~ —
   **`/dev/mem2` は外れ**（ただのカーネル RAM・§10.6）。本命は
   **`/dev/val_demux` のコマンドスタック経由**で、**呼び出し一覧は §20.6、
   通る順番は §24.3**。スクランブル解除より上流でタップできることは
   **構造上は確認できた**（`DSCRAM` は PID 単位の独立した段・§11.5）。実証は未。
8. ~~`i2c_emma` の GPL ソース未同梱~~ — **誤りだった**。GPL tarball は完全なツリーで、
   i2c の GPL ソースは入っている（§2）。**`h57fmb` は GPL 宣言なのにソースが
   tarball に無い**＝順守の欠落として §2 に記録した。

## 8. 観察手順（再現用）

前提: ベンダ rootfs に観察用の入口を足したイメージが mtd3 に入っていること
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

`tools/yb1-trace`（`scripts/build-yb1-trace.sh` でビルド）を rootfs イメージの
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

**追記（2026-09-22）**:

- **WDT 説は裏が取れていない。** `/sys/class/misc/watchdog` は登録されているが
  `/dev/watchdog` のノードが無く、**どのプロセスも開いていない**。∴ 再起動の
  引き金は `/dev/watchdog` 越しの WDT ではない（別の監視か、AV の異常停止）。
- **kill しても資源は返らない**（§25.2）。`blur` が消えても `val_merge` の
  デバイス登録は残り、`nr=0x30` の資源も空かない。∴「選局だけさせて落として
  乗っ取る」は成立しない。
- **代わりに `blur` を最初から起動しない道ができた**（§24.1）。`setup.sh` は
  通常ファイルなので bind mount で差し替えられる。**ただしこの状態では
  telnet に入れない**（IP を取る `udhcpc` は `blur` 側が起動する）＝
  シリアルが唯一の口。

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
| A1 | **チップ内 RAM へのファーム転送**の構造 | これが無いとチップが動かない。**必須** | **解決（2026-09-22・§27 と §28）**。切り出し・容器の構造・I2C 転送手順まで全部。仕様は [mbin-firmware-container.md](mbin-firmware-container.md)（§1-§6 が容器、**§7 が転送**、§7.5 に立ち上げ 5 手順）。blob は所有者の機器上のものを実行時に使う（§1-5） |
| A2 | **コールドスタートの初期化列** | 電源投入 → Xtal → FW → ISDB-T モード設定。いま採れているのは「初期化済みの状態からの選局」だけ | 起動からのバストレースは**すでに採れる状態**。未解析なだけ |
| A3 | 残り 19 の制御レジスタの意味 | 帯域・モード・TS 出力の設定 | 条件を変えた差分（§7-4） |
| A4 | **ロック/同期・BER・TMCC・AGC** のレジスタ | chardev が出すべき値。C/N しか割れていない | `X_GetOfdmTunerInfo` の 5 値のうち Cn だけ対応が付いた。Ber は 0 と 99999 しか出ず、Sync/TMCC は UPnP に出ない。`fe_main_drv` の `DEMOD_GET_SYNC` / `GET_TMCC` を突く手がある（§7-4） |
| A5 | C/N → dB の換算式 | 精度が要るとき | 固定条件での多数回平均か信号発生器（§4.3） |
| A6 | 電源・リセットの順序 | 自前で起こすとき | GPL の u-boot ボードファイルに `GPIO[12] = TU_RST#` がある＝**GPL 由来で書ける** |

### 9.2 TS データ経路 ← ~~**最大の壁**~~（→ §24 でほぼ割れた）

> **この節は 2026-09-21 時点の見立て。** 以後の §10〜§26 で大きく動いたので、
> **結論は §24.5 と §25.3 を見ること。** 要点だけ先に:
> - **TS 経路の ABI は割れた。** ベンダアプリ抜きなら、デマルチプレクサの設定が
>   最後まで通る（§24.3）。B1・B4 は解決、B2・B3 も筋が立った（下表）。
> - **`EMMA_GIO_BASE` の線は消えた。** AV レジスタは**裏 CPU の領分**で、
>   メイン CPU からは元々見えない（§11.1）。下の「場所の候補」の項は誤り。
> - **最大の壁は入れ替わった。** いまの関門は**フロントエンド側の A1**
>   （MXL683 のファーム転送・§9.1／§26）。

現状わかっていること（2026-09-21 時点）:

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
- ~~**ただし場所の候補は 1 つに絞れた**: `EMMA_GIO_BASE 0x11000000` の 16MB~~
  **← 誤り。** AV のレジスタは**裏 CPU の領分**で、メイン CPU からは見えない
  （§11.1）。`0x11000000` を読む必要は無くなった（§10.7.6 の訂正）。

足りないもの（**状態は 2026-09-22 に更新**）:

| # | 足りないもの | 状態 |
|---|---|---|
| B1 | `val_demux` の ioctl 契約（リング/バッファの取り方） | **解決**。呼び出し一覧は §20.6、作法は §16.1／§23.2、通る順番は §24.3 |
| B2 | 全 PID 素通し（フィルタ無し）の指定方法 | **筋は立った**。`MMAC_DEMUX_ControlHardwarePidFiltering` が存在する（§11.4）。まだ叩いていない |
| B3 | **スクランブル解除より上流でタップできるか** | **構造上は成立**（§11.5）。`DSCRAM` は PID 単位の独立した段なので、登録しなければ素通しする。実証は未 |
| B4 | `val_merge` を経由せず、自前で TS 入力〜DMA を組めるか | **解決（否）**。AV レジスタはメイン CPU から見えない＝裏 CPU 経由しか無い（§11.6） |

**B4 が段階 C の形を決める。** 2 つの道がある（**→ §11.6 で 2 に確定した**。
AV ハードのレジスタはメイン CPU から見えないので 1 は道が無い）:

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

### 9.5 見通し（**2026-09-22 に逆転した**）

**2026-09-21 の見立てはこうだった**（記録として残す）:

> 9.1（フロントエンド）は見通しが立っている。9.2（TS 経路）が唯一の本質的な
> 未知で、次に投資すべきは B1〜B3 の観察。フロントエンドは「詰めれば終わる」が、
> TS 経路は「できるかどうかがまだ分からない」。

**§10〜§26 で、これは**完全に逆になった**:

| | 2026-09-21 の見立て | 2026-09-22 の実測 |
|---|---|---|
| **TS 経路**（9.2） | 唯一の本質的な未知 | **ABI は割れた。ベンダアプリ抜きなら設定が最後まで通る**（§24.3） |
| **フロントエンド**（9.1） | 詰めれば終わる | **A1 が関門**。ファームは `blur` に埋まった MBIN で、切り出しも転送手順も未（§26） |

理由: **ベンダアプリと同居して TS を取る道が閉じた**（§25）。バッファ id は
平らな空間でベンダが押さえており、`blur` を落としても資源は返らない。
∴ **ベンダアプリ抜きで起動するしかなく、そうすると誰も MXL683 を初期化しない。**

∴ **次に投資すべきは A1** — と書いたが、**その A1 も 2026-09-22 中に片付いた**
（§27 で切り出しと容器、§28 で I2C 転送手順）。TS 経路側は「B2 の素通し指定」と
「B3 の実証」が残るが、どちらも**手が分かっている**。

∴ **2026-09-22 時点で、段階 C に残っているのは観察ではなく実装**。
フロントエンドとデマルチプレクサの両方について、実機で通る手順が揃っている（§28.3）。

## 10. TS データ経路（2026-09-21・実測で所在を特定）

§9.2 で「最大の壁」としていた部分。**TS がメモリのどこに置かれるかを特定した。**

### 10.1 `/dev/mem` は mmap なら通る

§4.4 に「`/dev/mem` から I2C のレジスタは覗けない」と書いたが、**それは `read()`
についてのみ正しい**。2.6.27 の MIPS は `valid_phys_addr_range()` で read/write を
RAM に限るが、**`mmap()` は制限していない**。ベンダアプリはこれを使っている。

`blur` の `/proc/<pid>/maps`（実測）:

```
2aab0000-2aab2000 rw-s 09000000 /dev/mem    制御ページ 8KB
2aab2000-2aab3000 rw-s 10009000 /dev/mem    XDMA レジスタ
2aab3000-2aab4000 rw-s 10110000 /dev/mem    TIMER/GPIO レジスタ
2aab4000-2aab5000 rw-s 10000000 /dev/mem    BHIF レジスタ
2aeac000-2be48000 rw-s 09464000 /dev/mem    約 16MB
2c008000-2c808000 rw-s 0a488000 /dev/mem    8MB
2cb1e000-2db1d000 rw-s 00000000 /dev/mem2   16MB
2dc07000-2ee07000 rw-s 0acea000 /dev/mem    18MB  ← TS リング
2ee07000-30007000 rw-s 0acea000 /dev/mem    18MB  ← 同じ物理を二重 mmap
```

**∴ 段階 C の userland からも、AV 予約領域と SoC レジスタに手が届く。**
`/proc/iomem` の System RAM は `0x08ffffff` までなので、`0x09000000` 以降は
カーネルが管理しない AV 専用領域（§1 の「free が 106MB しか見せない」の正体）。

`val_demux` の ioctl 引数に出る `0xa9......`（§10.3）は MIPS の KSEG1 で、
**物理 = アドレス − 0xA0000000**。`0xa9000058` → 物理 `0x09000058`＝この領域。

### 10.2 ★ TS リングは物理 `0x0acea000`

> **訂正（§10.7.4）**: ここを「復調器の取り込みリング」と読んだのは**誤り**。
> 実体は**トランスコーダ MB86H58A（`/dev/h57fmb0`）の受け渡しバッファ**である。
> 所在と性質の記述はそのまま正しいので残すが、位置づけは §10.7.4 を見ること。

自前ツール `tools/yb1-mem`（mmap して読むだけ）で、ベンダアプリに実際に配信させ
ながら AV 領域を全域走査した。

| 物理 | 大きさ | TS |
|---|---|---|
| `0x09000000` | 8 KB | 無し |
| `0x092a4000` | 1.8 MB | 無し |
| `0x09464000` | 16 MB | 無し |
| `0x0a400000` | 544 KB | 無し |
| `0x0a488000` | 8 MB | 無し |
| `0x0ac88000` | 392 KB | 無し |
| **`0x0acea000`** | **18 MB** | **有り** |

`0x0acea000` の先頭 4 MB を歩いた結果:

```
packets 22310   desync 0
PID: 0x0000(PAT) 0x0002 0x0003 0x0012(EIT) 0x0111 0x0130 0x0138 0x01f0 0x01ff 0x1fff(null)
```

- **188 バイト周期で切れ目なし**（4 MB ＝ 22,315 パケット分に対し 22,310 個、
  desync 0）＝**隙間なく詰まった素の MPEG-2 TS**。
- **`0x1fff`（null パケット）が入っている**＝特定 PID だけを抜いた出力ではなく、
  **TS がほぼそのまま**流れている。段階 C が欲しい形。
- **同じ物理を 2 回連続で mmap している**のは、リングの折り返しを連続アドレスに
  見せる定石。∴ これは**リングバッファ**。

### 10.3 ★ ここはデスクランブラの下流（重要・スコープ）

走査で数えたのは TS ヘッダの `transport_scrambling_control` だけだが、その結果:

```
scrambling_control: none=22310  reserved=0  even=0  odd=0
```

**全パケットが平文**だった。日本の地上デジタルは全番組がスクランブルされている
ので、これは**デスクランブル済み**を意味する。しかも**他の領域に TS が一切無い**
＝スクランブルされたままの TS は**そもそも DRAM に現れない**。

∴ 経路はこう読める:

```
MXL683 →（シリアル TS）→ SoC の TS 入力 →[MULTI2 デスクランブラ]→ DRAM リング 0x0acea000
                                              ↑ ベンダは有効化している
```

**§0 のスコープ上、この状態のリングを OpenIRD の TS 源にしてはならない。**
本文書の観察でも、PID と scrambling_control の**ヘッダだけ**を数え、内容は
一切取り出していない。

**ただしこれは段階 C にとって悪い話ではない。** デスクランブラは経路上の 1 段で、
**有効化するかどうかは設定**である（ベンダは SoftCAS の鍵を入れて有効にしている）。
OpenIRD は**その段を設定しない**だけでよく、そうすれば同じリングに
**スクランブルされたままの TS** が入る。CAS には一切触れずに px4_drv 互換の
chardev が成立する、という筋が立つ。

> **補足（§10.7.5）**: 「同じリングに」の部分は訂正が要る。このリングは
> トランスコーダのバッファなので、OpenIRD の取り込み先にはならない。
> ただし**デスクランブラは 1 段でしかなく、通さない構成なら生 TS が出る**という
> 筋自体は変わらない。取り込み先は自分で用意することになる。

### 10.4 リングは配信セッションが回す（2026-09-21・D4 の実測）

デスクランブラを設定しない状態を捕まえられないか試した。**捕まらなかった。**

- **選局だけではリングは動かない。** 503MHz / 557MHz に切り替えて 25 秒待っても、
  リングの内容は前回の配信の残りのまま（毎回 5577 パケット・同一）。
  ∴ **TS の取り込みを始めるのは配信セッションであって選局ではない。**
- **配信開始の瞬間から 14 回サンプリングしても、最初の 1 パケットから平文。**
  過渡でスクランブルされたものが残る窓は無かった。

∴ **ベンダの構成では、リングに入る時点で常にデスクランブル済み。** D4（スクランブル
されたまま取れるか）は**受動観察では決着しない**。決着させるには、デスクランブル段を
設定しない経路を自分で組む（＝D3）か、その段を止める書き込みを試すことになる。

なお論理としては、**OpenIRD は CAS の鍵を一切持たない**ので、どの道を通っても
出てくるのはスクランブルされた TS である。問題は「鍵の無いデスクランブラが素通し
するか、壊すか」であって、そこは実装時に確かめる。

### 10.5 D3 の道選び（2026-09-21・実測）

| 実験 | 結果 |
|---|---|
| **TS 入力のピン切替** | **GPL どおりで確定**。`BHIF_ALTERNATE0`(`0x10000028`) の実測値は `0x00084008`＝`ALT[19:18] = 10b`。u-boot のボードファイルが `GPIO[0~1]` と `GPIO[9~11]` を「TS SideA 入力」にする記述と一致（`Origin: gpl-derived` で書ける） |
| **XDMA(`0x10009000`) が TS を運ぶか** | **否**。配信中に先頭 128B を読んでもほぼゼロで、リングのアドレスは現れない。∴「メイン CPU が XDMA を直接回して TS を取る」道は、この証拠の範囲では無い |
| **制御ページ(`0x09000000`) の中身** | **ディスクリプタ表だった**。`[base][end][ptr][ptr][ptr]` の形が並ぶ:<br>`+0xb8: 09484000 094a3fff 09484000 09484000 09484000`<br>`+0xd0: 0a428000 0a42ffff 0a428000 0a428000 0a428000`<br>ほかに 64bit の時刻（`+0xa0`/`+0xa8`。当初「約 390MHz の自走カウンタ」と書いたが**誤り**。実測は 46.008MHz＝STC。§10.7.1）|

**ただし TS リング `0x0acea000` を指す語は、この 8KB の中に 1 つも無い。** 配信中に
動いた語も 2048 語中 14 語だけで、上の 2 つのディスクリプタは静止したまま
＝別用途のバッファ。**TS リングの制御ブロックはここではない。**

**得られた見通し**: AV サブシステムの口は「不透明なレジスタ群」ではなく
**共有メモリ上のディスクリプタ表**である。形が読めるので、**レジスタ仕様書が無くても
差分観察で構造を起こせる**。段階 C にとっては朗報。

**次の糸口**: `val_demux` の `nr=0x15` が渡す `0xa9000058`（§10.1）は物理
`0x09000058`＝**この制御ページの中**で、配信開始でこのバイトが `0x02` → `0xc3` に
変わった。∴ **val_demux の ioctl と制御ページは同じ構造を指している。** ここを
手繰るのが筋。

### 10.6 まだ足りないもの（→ §10.7.6 で更新）

ここに `/dev/mem2` と `0x09000058` と制御ページ全域の 3 本を「次にやると効くこと」
として挙げていた。**3 本とも実施した**。結果:

- `/dev/mem2` は**ただのカーネル RAM**を map していた（tmpfs の文字列が見える）。行き止まり。
- `0x09000058` は**状態フラグではなく値の変わるバイト**（`0x73`/`0x02`/`0xc8`/`0xc3`）。
  「配信開始で `0x02`→`0xc3`」は 2 点しか見ていなかったための早合点。
- 制御ページ全域の差分では、TS リングを指す語は**やはり 1 つも無かった**。

∴ 表は §10.7.6 に置き換えた。

### 10.7 リングの正体が分かった（2026-09-21・第 2 ラウンド）

§10.6 に挙げた 3 本を実施した結果、**`0x0acea000` は復調器の取り込みリングでは
なかった**。以下、確定した順に書く。

#### 10.7.1 `+0xa0`/`+0xa8` は STC と表示時刻（消費ポインタではない）

実機の `/proc/uptime` で 15 秒 × 4 区間を測った（host の往復を挟まない）:

```
区間1 dt=15.10s  +0xa0 +692897672 -> 45.887 MHz
区間2 dt=15.05s  +0xa0 +692897674 -> 46.040 MHz
区間3 dt=15.07s  +0xa0 +692897458 -> 45.979 MHz
区間4 dt=15.08s  +0xa0 +695562650 -> 46.125 MHz
全区間 dt=60.30s -> 46.008 MHz
```

- **46.008 MHz**。`90kHz × 512 = 46.08 MHz` と測定誤差の範囲で一致し、27 MHz とは
  70% 違う。∴ **単位は 90kHz の 1/512**（PTS を 9 ビットの小数付きで持つ形。300 倍
  ではなくシフトで済む表現）。
- 絶対値を 90kHz 換算すると 33bit PTS の範囲に収まる（約 23.7 時間／周期 26.5 時間）。
- **配信セッション中だけ有効**。待機・選局のみの状態では `0xbffffeff_efffffff` /
  `0xffffffff_ffffffff` という未初期化値を読む。
- `+0xa8` は `+0xa0` より **13〜47 ms 進んでいる**（フレーム間隔ぶんの揺れ）。

∴ `+0xa0`＝**STC**、`+0xa8`＝**表示時刻**。`val_demux` の `nr=0x21`(R)/`0x22`(W)
は**STC の読み書き**であって、D2 に書いた「消費ポインタ」ではない。**D2 は誤読。**

#### 10.7.2 GPL のヘッダに SoC の全体図があった（抽出漏れ）

`include/asm-mips/emma/emma3sep.h` は**手元の展開済みツリーに入っていなかった**
（§2.2 で一度踏んだのと同じ、部分展開の罠）。tarball から取ると:

| 名前 | 物理 | GPL に使う側があるか |
|---|---|---|
| `EMMA_RAM_SIZE` | `0x09000000`（144MB） | — **`0x09000000` 以降が AV 予約なのは GPL 記載の事実**になった |
| `EMMA_IO_BASE` | `0x10000000` 16MB | BHIF / BROI / XDMA / ETHER / PBRD / TIMER / FLIP / SATA。全部ドライバ有り |
| **`EMMA_GIO_BASE`** | **`0x11000000` 16MB** | **レジスタ定義ゼロ・利用者ゼロ**。名前だけの 16MB |
| `EMMA_BHIF_AVSWITCH0` | `0x10000024` | **定義だけ。GPL に利用者なし** |

**∴ 空き地は `0x11000000` の 16MB。** AV 系のレジスタがあるとすればここ。

#### 10.7.3 `blur` が直接触る SoC レジスタは配信で動かない

`blur` が mmap している MMIO は 3 ページだけ（§10.1）。GPL で名前の付いている
レジスタだけを 32bit 読みし、待機／選局のみ／配信中 ×2 で比べた:

| レジスタ | 値 | 4 状態で |
|---|---|---|
| `BHIF STRAP_0` | `c020310b` | 不変 |
| `BHIF ALTERNATE0` | `00084008` | 不変（§10.5 の TS 入力ピン設定） |
| **`BHIF AVSWITCH0`** | **`00000000`** | 不変 |
| **`XDMA DMAC_LIST_ENABLE`** | **`00000000`** | 不変 |
| `XDMA DMAC_MEM_LIST0..4` | `1c054000 10036400 00000000 30000000 059552c0` | 不変 |
| `TIMER +0x00..0x3c` | 全ゼロ | 不変 |

**`LIST_ENABLE = 0`** ＝ XDMA のリストモードは動いていない。§10.5 の「XDMA は TS を
運んでいない」を**別証拠で裏づけ**た（`MEM_LIST` に残っている値は有効な物理アドレス
の形をしておらず、残骸と見てよい）。

なお `/proc/vmallocinfo` に `ioremap` の行は 1 本も無いが、**MIPS では 512MB 未満の
物理は `ioremap()` が KSEG1 を返すだけで vmalloc を消費しない**ので、これは
「ベンダモジュールがレジスタを触っていない」証拠にはならない。

#### 10.7.4 ★ `0x0acea000` はトランスコーダ（MB86H58A）のバッファだった

ioctl トレースの**起動直後 6 行目**に答えがあった:

```
4.6474 [911] open /dev/h57fmb0 -> fd 6
4.6476 [911] ioctl /dev/h57fmb0 nr=0x1d size=16 in=[16]:
              00 00 00 00 | 00 3a c0 00 | aa ce a0 00 | 2a bd 60 00
4.6478 [911] ioctl /dev/h57fmb0 nr=0x1d size=16 in=[16]:
              00 00 00 01 | 00 3a c0 00 | ab 09 60 00 | 2a f8 20 00
4.6481 [911] ioctl /dev/h57fmb0 nr=0x1d size=16 in=[16]:
              00 00 00 02 | 00 3a c0 00 | ab 44 20 00 | 2b 32 e0 00
```

`{ 番号, 長さ 0x3ac000(3.85MB), KSEG1 の物理, ユーザ VA }` の 3 本立てで、物理は
`0x0acea000` / `0x0b096000` / `0x0b442000` ＝**連続した 11.55MB を 3 分割**。

- `/dev/h57fmb0` は **major 180（USB）minor 248**。∴ **MB86H58A は USB ぶら下がり**
  （SB-TV02-WFPL の Dione と同じ形）。
- **`h57fmb` は `/proc/modules` で `(P)` が付かない唯一のベンダモジュール**
  ＝GPL 互換ライセンスを宣言している。**しかし GPL tarball に該当ソースは無い**
  （`h57`/`h58`/`fmb`/`dione` で全件検索して 0 件）。**GPL 順守の欠落**として記録する。

∴ §10.2〜§10.3 で「TS リング」と呼んでいたものは、**復調器の取り込みリングでは
なく、トランスコーダの受け渡しバッファ**だった。中身が最初のパケットから平文
だったのも、`0x1fff` 込みでほぼ素の TS だったのも、これで説明が付く
（トランスコーダにはデスクランブル済みの TS を渡す）。

#### 10.7.5 ★ 生 TS は DRAM のどこにも現れない

AV 予約領域 **`0x09000000`〜`0x0fffffff`（112MB）を 4MB ずつ 28 回**、配信を張った
まま TS sync で掃いた:

```
0x09000000 〜 0x0a800000   TS 無し
0x0ac00000 〜 0x0bbfffff   TS 有り  ← h57fmb の 3 バッファ（0x0acea000〜0x0b7ee000）
0x0bc00000 〜 0x0fffffff   TS 無し（64MB 全域）
```

`blur` が map する全領域についても、**選局のみ**と**配信中**の両方で
`scrambling_control` を数えた。**TS があるのは h57fmb バッファだけ**で、しかも
両状態で内容が 1 バイトも変わらない（前のセッションの残り）。

∴ **スクランブルされたままの TS は、ベンダの構成では DRAM に一切現れない。**
`MXL683 →（シリアル TS）→ SoC の TS 入力 → デスクランブラ → デコーダ` が
**オンチップで完結**していて、途中で DRAM を経由しない。

**これで D4 の受動観察は完全に閉じた。** 観察できる場所に生 TS は無い。

#### 10.7.6 更新した「足りないもの」

| # | 内容 | 状態 |
|---|---|---|
| D1 | TS リングの制御ブロックの所在 | **解消。そんなものは無かった**（§10.7.4）。`0x0acea000` は h57fmb のバッファで、`nr=0x1d` の ioctl で登録される |
| D2 | `nr=0x21`/`0x22` は消費ポインタか | **誤読だった。STC の読み書き**（§10.7.1） |
| D3 | **TS 入力とデスクランブラを自前で設定する** | **未解決。ここが唯一の壁になった。** ピン切替は GPL で確定、しかし取り込みを始めるレジスタは GPL に無く、`0x11000000` の 16MB（§10.7.2）が唯一の候補地 |
| D4 | デスクランブラを通さない経路 | **受動観察では出ない**と確定（§10.7.5）。D3 が立ってから実装で確かめる話になった |

**残る問いは 1 つだけになった**: **SoC の TS 入力／デマルチプレクサのレジスタは
どこにあり、どう設定するか。**

> **→ §11 で答えが出た。** 1 番（`.ko` のデータ節）をやったところ、レジスタの
> アドレスは出てこなかった代わりに**構造が割れた**。**AV ハードのレジスタは
> メイン CPU からは見えない**（裏 CPU の領分）ので、この問い自体が立たない。
> 2 番（`0x11000000` を読む）も**やる必要が無くなった**。
> 段階 C の形は §11.6、残る唯一の未知は §11.7。


## 11. AV サブシステムの構造（2026-09-21・モジュールのシンボルから確定）

§10.7.6 の「次に効く手」の 1 番（ベンダ `.ko` のデータ節を見る）をやった。
**レジスタのベースアドレスは出てこなかったが、代わりに構造そのものが割れた。**

観察手段は clean-room で許された範囲のみ: `objdump -h`（`-ffunction-sections`
ビルドなので**節名＝関数名**が全部出る）、`nm -g --defined-only`、`strings`、実機の
`/proc/kallsyms` と `/sys/module/*/parameters`。**逆アセンブルはしていない。**

### 11.1 スタックの段

```
blur (userland)
  └─ val_merge.ko   MMAC_* API（エクスポート 669 本）
       └─ csd.ko    コマンドスタック（= 裏 CPU へのメールボックス）
            └─ 裏 CPU（AV ファーム。mtd5 から物理 0x00030000 へ・§6）
                 └─ demux / descrambler / decoder のハードウェア
```

**∴ メイン CPU は AV ハードのレジスタを直接叩いていない。** 共有メモリ上の
コマンドスタックに命令を積み、ドアベルを鳴らして裏 CPU にやらせている。
これで §9.2 の「GPL のレジスタ表に TS / demux / AV が 1 つも無い」も、
§10.7.3 の「`blur` が map する MMIO が配信で 1bit も動かない」も説明が付く。
**レジスタは裏 CPU の領分で、メイン CPU からは元々見えない。**

### 11.2 `csd` ＝ Command Stack Driver

`nm -g --defined-only csd.ko`（全 30 シンボル）:

| 種別 | シンボル |
|---|---|
| 裏 CPU | `Reset_SubCPU` |
| コマンドスタック | `init_command_stack` / `Request_CmdStackArea` / `Free_CmdStackArea` |
| 実行要求 | `Request_ApiExecute` / `Request_SpecialCmdExecute` |
| 割り込み受け | `read_sw_int_cmdstk` / `read_sw_int_event` / `read_sw_int_fifo` / `read_sw_int_special` / `assert_int` / `set_fifoctrl_flag` |
| コールバック | `Install_EventCallback` / `Uninstall_EventCallback` / `Install_DebugCallback` |
| その他 | `Get_CSD_Infomation` / `csd_ioctl` / `csd_mmap` / `CSD_BASE` / `CSD_DEVID` / `CSD_MODE` / `CSD_DEBUG` |

- **`sw_int` は GPL にあるレジスタ名と一致する**。`emma3sep.h` の
  `EMMA_BHIF_SW_INT` (`0x100001a0`) / `SW_INT_EN` (`0x100001a4`) /
  `SW_INT_CLR` (`0x100001a8`)。**∴ ドアベルは GPL 定義のレジスタ**で、
  `blur` が BHIF ページを mmap している理由もこれで説明が付く（§10.7.3）。
- `CSD_BASE` / `CSD_DEVID` / `CSD_MODE` / `CSD_DEBUG` は **module_param**。実機の
  `/sys/module/csd/parameters` を読むと `0 / 0 / NORMAL / 0`。
  **`CSD_BASE` はハードのベースアドレスではなくインスタンス番号**だった。
- `csd_mmap` は `mknod c 252 0` して offset 0 を map すると **物理 0 番地**が出る
  （MIPS の例外ベクタのコードが見える）。∴ csd は `/dev/mem` と同じく物理メモリを
  見せる口で、**コマンドスタックは RAM 上の領域**。

### 11.3 `val_merge` が持つ MMAC ブロック

節名から数えたブロックと関数本数（**数え落としが 5 つあった。全 28 ブロックの表は
§49.8**）:

| ブロック | 本数 | | ブロック | 本数 |
|---|---|---|---|---|
| VIDEO | 85 | | CAPTURE | 15 |
| **DEMUX** | **71** | | BITBLT | 15 |
| AUDIO | 63 | | PLAYBACK | 12 |
| SCALER | 47 | | DMA | 12 |
| VENC | 44 | | DCNV | 11 |
| PQC | 32 | | **SCRAM** | **10** |
| OSD | 31 | | SCARD | 10 |
| PVR | 27 | | **RECORD** | **10** |
| **DEMOD** | **22** | | ELU | 10 |
| PCMCIA | 18 | | **DSCRAM** | **10** |
| DISP | 16 | | **M2DSC** | **8** |

### 11.4 ★ DEMUX は普通の MPEG-2 デマルチプレクサだった

段階 C に効くものだけ抜き出す（全 71 本）:

| 用途 | 関数 |
|---|---|
| **入力源の選択** | `MMAC_DEMUX_SetDeviceInputSource` |
| **PID フィルタ** | `AllocateGeneralPidFilter` / `AllocateAvPidFilter` / `AllocatePcrPidFilter` / `SetPidValue` / `EnablePidFilter` / `DisablePidFilter` / `FreePidFilter` |
| **全 PID 素通し** | `ControlHardwarePidFiltering` |
| **DRAM リング** | `SetPidFilterBuffer` / **`GetPidFilterBufferWritePointerOffset`** / `SetPidFilterBufferWritePointerOffset` / `SetPidFilterBufferNearlyFullPointerOffset` / `ResetPidFilterBuffer` |
| ペイロード側 | `AllocatePayloadFilter` / `SetPayloadBuffer` / `GetPayloadBufferWritePointerOffset` / … |
| **TS 出し** | `OpenStreamOutDevice` / `SelectStreamOutGroup` / `StartStreamOut` / `StopStreamOut` |
| 時刻 | `GetStcValue` / `GetLatchedStcValue` / `GetPcrValue` / `SetStcControlValue` / `ControlStcEvent` |
| 状態 | `GetCapabilities` / `GetErrorStatus` / `GetPidPts` |

**`SetPidFilterBuffer` ＋ `GetPidFilterBufferWritePointerOffset` は、そのまま
「PID フィルタの出力を DRAM のリングに書き、書き込みポインタを読む」＝ px4_drv 互換
chardev のデータ経路そのものである。** ハードウェアにその機能がある、というのが
今回いちばん大きい。

`MMAC_RECORD_*`（`OpenDevice` / `EnablePIDFilter` / `GetPacketCount` /
`InstallCallback` / `Start` / `Stop`）も同じ性質の第 2 の経路。

### 11.5 ★ デスクランブラは PID 単位の独立した段

`MMAC_DSCRAM_*` は `MMAC_DEMUX_*` と**完全に別のブロック**で、自前の
`Initialise` / `SetConfiguration` / `SetPIDFilter` / `EnablePIDFilter` /
`DisablePIDFilter` / `SetDescrambleKey` / `SetKeyArea` / `SetDesKeyType` を持つ。

**∴ デスクランブルは「その PID をデスクランブラの PID フィルタに入れたとき」だけ
かかる。入れなければ素通しする。**

これで §10.4 と §10.7.5 が繋がる:

- ベンダは A/V の PID を `DSCRAM` に登録し、出力を `StreamOut` でトランスコーダへ
  流している。だから**生 TS が DRAM に出る局面が無い**（§10.7.5）。
- OpenIRD は `DSCRAM` を**一度も初期化しない**で、`DEMUX` の汎用 PID フィルタ
  （あるいは `ControlHardwarePidFiltering` による全 PID 素通し）を DRAM リングに
  向けるだけでよい。**出てくるのはスクランブルされたままの TS。**

**∴ D4 は構造上「成立する」と言える。** CAS の鍵にも手順にも一切触れずに
chardev の契約を満たせる。§0 のスコープと矛盾しない。

なお `MMAC_SCRAM_*`（スクランブラ）は録画時に自機の鍵で掛け直すための段と見られる。
**OpenIRD はどちらの段も使わない。**

### 11.6 段階 C の形が決まった

§9.2 の B4（自前で TS 入力を叩く / 裏 CPU を使ってその口だけ自前で叩く）は、
**後者しか無い**と確定した。前者は「AV ハードのレジスタはメイン CPU から見えない」
（§11.1）ので、そもそも道が無い。

∴ 段階 C の `ird-tuner-poe-br100-yb1` はこうなる:

1. **裏 CPU のファームは所有者の機器上のものを実行時に使う**（§1-5 と整合。
   SB-TV02 の Dione ファームと同じ扱い）。
2. **`csd` 相当のコマンドスタック層をクリーン実装する**。ドアベルは GPL 定義の
   `BHIF_SW_INT`、領域は共有メモリ。
3. **`MMAC_DEMUX` のうち §11.4 の 10 数本ぶんだけをクリーン実装する**
   （入力源 → 汎用 PID フィルタ → DRAM リング → 書き込みポインタ）。
   VIDEO/AUDIO/SCALER/OSD/… の 400 本超は**要らない**。
4. **`MMAC_DSCRAM` / `MMAC_SCRAM` / `MMAC_SCARD` / `MMAC_M2DSC` は実装しない**
   （§0）。

### 11.7 残る唯一の未知＝コマンドの線上表現（→ §12 で所在と形まで判明）

構造は割れたが、**コマンドスタックに積む語の並び（コマンド ID と引数のレイアウト）
はまだ分かっていない**。これが最後の壁。

観察の道筋はある。`val_*` の ioctl は 1 デバイス 1 type バイトで整理されていて
（`0xc3`=`val_demux`、`0xc4`=`val_disp`、`0xcb`=`val_scaler`、`0xcd`=`val_videnc`、
`0xdb`=`val_m2dsc`、`0xae`=`h57fmb`、`0xb3`=`io_controller`）、引数は 40B 固定。
**ioctl を 1 本発行するたびに共有メモリを差分すれば、ioctl ↔ コマンド語の対応が
取れる。** `tools/yb1-trace` と `tools/yb1-mem` は揃っているので、着手できる。

### 11.8 観察できた ioctl の全景（起動〜選局〜配信 1 回ぶん・130,464 行）

`tools/yb1-trace` のログを `cmd` で数えた（上位のみ。全 61 種）:

| 回数 | デバイス | cmd | type/nr | size |
|---|---|---|---|---|
| 55,743 | `io_controller` | `0x8004b303` | b3/03 W | 4 |
| 48,032 | `val_demux` | `0x4028c32f` | c3/2f R | 40 |
| 20,171 | `io_controller` | `0xc004b302` | b3/02 WR | 4 |
| 16,732 | `io_controller` | `0xc004b304` | b3/04 WR | 4 |
| 4,486 | `val_dcnv` | `0x4028d506` | d5/06 R | 40 |
| 2,314 | `h57fmb0` | `0xc00cae1a` | ae/1a WR | 12 |
| 484 | `val_demux` | `0x8028c322` / `0x4028c321` | c3/22 W・c3/21 R | 40 |
| 5 | `h57fmb0` | `0x8010ae1d` | ae/1d W | 16 |

読み取れること:

- **1 デバイス＝ 1 type バイト**で整理されている:
  `0xc3`=`val_demux`(と `val_demux1`)、`0xc4`=`val_disp`、`0xcb`=`val_scaler`、
  `0xcd`=`val_videnc`、`0xd5`=`val_dcnv`、`0xdb`=`val_m2dsc`、`0xae`=`h57fmb`、
  `0xb3`=`io_controller`。
- **`val_*` の引数は例外なく 40 バイト固定**。可変長構造を持たない＝
  「コマンド語 ＋ 引数の配列」を素直に詰めた形と見てよい。
- `val_demux` で実際に使われた nr は
  `05 0b 0c 0d 0e 10 14 15 16 17 19 1a 21 22 24 26 29 2a 2b 2e 2f 30 32 33 34 64 65 6a c9`
  の 29 種だけ。**§11.4 の DEMUX 関数 71 本のうち、実際に動いているのはこの範囲。**
- `c3/2f`(R) が 48,032 回＝**ポーリング**。`c3/21`(R) と `c3/22`(W) が同数 484 回＝
  **読んで書き戻す対**（§10.7.1 の STC）。
- `b3/03`(W, 4B) が 55,743 回＝`io_controller` への書き込みポーリング。
  `io_controller_drv.ko` には `_emma_reg_rw` という関数がある（節名から）。

## 12. コマンドスタックの所在と形（2026-09-21・実測）

§11.7 の「残る唯一の未知」に取り掛かった。**共有メモリの所在と、エントリ表の形まで
割れた。** 語の意味（コマンド ID の割り当て）はまだ。

観察手段: `/proc/kallsyms`（`CONFIG_KALLSYMS=y`）で `csd_info` の番地を引き、
**`/dev/kmem`（`CONFIG_DEVKMEM=y`）を read で**読む。モジュールの変数は vmalloc
なので物理アドレスが無く mmap では届かないが、`/dev/kmem` の read 経路は `vread()`
を通るので vmalloc も読めて、未マップ部分はゼロ埋めされる＝安全。
`tools/yb1-mem` に `kread` を足した。**逆アセンブルはしていない。**

### 12.1 `csd_info` ＝ 共有メモリのポインタ表

```
c00d02ac: a0010000 a0010400 a0010800 a0018800
c00d02bc: a0019000 a001d000 a001f000 00020000
c00d02cc: 00000000 00000011 a0014dd0 000000cc
```

`0xa0......` は **KSEG1（非キャッシュ）** ＝ 物理 = アドレス − `0xA0000000`。
裏 CPU と共有するメモリなので非キャッシュなのは筋が通る。

| csd_info | 値 | 物理 | 大きさ | 見立て |
|---|---|---|---|---|
| `+0x00` | `a0010000` | `0x00010000` | 1 KB | ブロック 0（起動後は全ゼロ） |
| `+0x04` | `a0010400` | `0x00010400` | 1 KB | **API エントリ表**（§12.2） |
| `+0x08` | `a0010800` | `0x00010800` | 32 KB | **リクエスト/レスポンスのデータリング** |
| `+0x0c` | `a0018800` | `0x00018800` | 2 KB | |
| `+0x10` | `a0019000` | `0x00019000` | 16 KB | |
| `+0x14` | `a001d000` | `0x0001d000` | 8 KB | |
| `+0x18` | `a001f000` | `0x0001f000` | 4 KB | FIFO/イベント側（API 中に +0x67c 付近の 2 語がカウントアップする） |
| `+0x1c` | `00020000` | — | 128 KB | 全体の大きさ |
| `+0x24` | 可変（0〜31） | — | — | エントリ番号 |
| `+0x28` | `a0014dd0` 他 | — | — | **`data_wp`**（データリングの書き込みポインタ） |
| `+0x2c` | 可変 | — | — | 残り（`buf_remain`） |

**∴ 共有領域は物理 `0x00010000`〜`0x0002ffff` の 128 KB。** カーネルイメージ
（物理 `0x02280000`）よりずっと下で、**裏 CPU のファームを置く `0x00030000`（§6）の
すぐ手前**に接している。§10 で AV 予約領域（`0x09000000` 以降）ばかり掃いていたが、
**コマンドスタックは低位 RAM 側にあった。**

### 12.2 API エントリ表（物理 `0x00010400`・32 バイト刻み）

```
+0x00: 00000000  191b01bd  a00166cc  a00166d0
+0x10: 7fffffff  00000000  00000008  00000000
+0x20: 00000000  193101ae  a0016628  a0016630
+0x30: 7fffffff  00000000  00000008  00000000
```

| 語 | 例 | 見立て |
|---|---|---|
| 0 | `00000000` | 状態/フラグ |
| 1 | `191b01bd` / `193101ae` | 上位 16bit と下位 16bit の対。下位は API ID と見える（データリングにも `0x0100` `0x0102` `0x010f` `0x001f` `0x0021` といった値が並ぶ） |
| 2 | `a00166cc` | **リクエストの番地**（KSEG1・データリング内） |
| 3 | `a00166d0` | **レスポンスの番地**（リクエストの +8） |
| 4 | `7fffffff` | タイムアウト/番兵 |
| 6 | `00000008` | 大きさ（8 バイト） |

- **1 KB ÷ 32 B ＝ 32 エントリ。** `csd_info+0x24` が常に 0〜31 に収まるのと一致。
- csd の printk 書式に `invalid req+res size (%d:%d)` があり、**リクエストと
  レスポンスで別々の大きさを持つ**設計と整合する。

### 12.3 裏 CPU → メイン CPU の通知は BHIF のソフト割り込み

`/proc/interrupts` の実測（`BHIF-SW` チップ）と GPL の番号づけが完全に噛み合う。
GPL の `emma3sep.h` は `EMMA_IRQ_BASE = MIPS_CPU_IRQ_BASE + 8`、`NUM_EMMA_IRQ = 96`、
`EMMA_SW_IRQ_BASE = EMMA_IRQ_BASE + 96`、`NUM_EMMA_IRQ_SW = 32`。実測から
`EMMA_IRQ_BASE = 8` が確定する（nand=`INT(1)`→9、USB=`INT(32)`→40、
Ether=`INT(36)`→44、PIIC0/1=`INT(56/57)`→64/65 が全部合う）。
∴ **`EMMA_SW_IRQ_BASE = 104`**。

| SW int | IRQ | 名前 | 起動〜観察時点の回数 |
|---|---|---|---|
| 0 | 104 | `CSD_API_RESPONSE` | 119,514 |
| 1 | 105 | `CSD_SPECIAL` | 1 |
| 2 | 106 | `CSD_FIFOEVENT` | 0 |
| 3–7 | 107–111 | （未登録） | — |
| 8–31 | 112–135 | `CSD_INTEVENT00`〜`23` | 00 が 10.1M、03/07/11 のみ非ゼロ、他は 0 |

24 + 8 = 32 ＝ `NUM_EMMA_IRQ_SW` にちょうど収まる。
**∴ 通知側は GPL 定義のレジスタと番号で全部書ける**（`EMMA_BHIF_SW_INT` /
`SW_INT_EN` / `SW_INT_CLR` ＝ `0x100001a0` / `a4` / `a8`）。

`CSD_INTEVENT00` は**待機中でも 80 Hz で鳴っている**（5 秒で 399 回）。AV の定常
イベントと見られる。

### 12.4 API が走る条件（実測）

| 操作 | API 呼び出し数 |
|---|---|
| 何もしない（5 秒） | **0** |
| `X_GetOfdmTunerInfo` / `X_GetTuning` / `GetProtocolInfo` | **0** |
| `X_SetTuning` 1 回 | **25〜177**（`data_wp` が +320 B〜+2596 B 進む） |

- **状態読み出しは API を 1 回も使わない。** ∴ 復調器の状態は `blur` が I2C で
  直接読んでいる（§2.2 の MxLWare）という結論が、別経路で裏づけられた。
- **選局は AV パイプラインを触るので API が走る。** 25 回で 320 バイト
  ＝ 1 リクエストあたり平均 13 バイト程度＝ §12.2 の「大きさ 8」と整合。

### 12.5 残り（→ §13 で語の意味まで確定）

- **コマンド ID の割り当て**（どの ID が `MMAC_DEMUX_*` のどれか）。
  エントリ表の語 1 の下位 16bit を、ioctl 1 本ずつと突き合わせれば起こせる。
  いまは選局 1 回で 25 本まとめて走ってしまうので、**1 本ずつ発行できる
  `val_*` ioctl の発行ツール**が要る。
- ブロック 0 / 3 / 4 / 5 の用途（起動直後は全ゼロ）。
- `Request_SpecialCmdExecute`（`CSD_SPECIAL` は起動時に 1 回だけ）の用途。

## 13. コマンドスタックの中身を、ベンダ自身に喋らせた（2026-09-21）

§12.5 の「コマンド ID の割り当て」に取り掛かった。**エントリの語の意味は割れた。**
ioctl との対応づけはまだ。

### 13.1 `CSD_DEBUG` はレベルではなくビット集合だった

`csd` の `CSD_DEBUG` は `module_param(int)` で、sysfs では `0440`＝読み取り専用。
∴ **insmod 時にしか渡せない**。そして `csd` は `val_merge` に、`val_merge` は
`blur` の fd 21 本に掴まれているので、**blur が動いている限り入れ替えられない**。

`blur` が自分で `/sbin/insmod /app/bin/drivers/%s` を呼んでモジュールを load して
いる（`setup.sh` が insmod するのは `h57fmb.ko` と `8192du.ko` だけ）。`/sbin/insmod`
は busybox への**シンボリックリンク**なので、そこに bind mount でシムを被せる手は
使えない（`/bin/busybox` に当たってしまう）。

**採った手**: `rcS` に汎用フック（`/app/setting/openird-pre.sh` があれば source）を
足したイメージを焼き、そのフックで**裏 CPU ファームの転送と `csd` の load を自分でやる**。
`csd` はモジュール依存が無いので単独で load でき、あとから `setup.sh` が同じファーム
を同じ番地に書き直しても内容が同一なので影響が無い。`blur` の `insmod csd.ko` は
"File exists" で失敗するが、**blur はそれを無視して普通に起動する**（実機で確認）。

- `CSD_DEBUG=1` … 何も出ない
- **`CSD_DEBUG=65535` … 全部出る**

∴ レベルではなくビット集合。`/app/setting/openird-csddebug` の中身が値になる。

### 13.2 出てきた足跡（1 回の API 呼び出し）

```
Free_CmdStackArea:846 / free_entry:362 / set_apictrl_flag:107 entry = 21
Request_CmdStackArea:637
alloc_entry:321 (entry=22)
alloc_entry:338   Alloc entry (22) (cnt = 1)
entry = 22 entry_in = 23 alloc_cnt = 1 alloc_data_buf:396
buf_remain_b = 0x00001540 buf_remain_a = 0x00006ac0 csd_info.data_wp = 0xa0011d40
  buf_tail = 0xa0011d40 head = 0xa0010800 tail = 0xa0018800 alloc_data_buf:455
set_apictrl_block:157
Request_ApiExecute:762 / :777  API type : normal / :794  execute type : normal
```

**`head = 0xa0010800` / `tail = 0xa0018800` がドライバ自身の口から出た**＝§12.1 で
`csd_info` から起こした 32KB のデータリングの位置が、独立に裏づけられた。

手順は 1 呼び出しにつき: **エントリ確保 → データリングからリクエスト/レスポンス領域を
切り出す（`data_wp` が進む）→ エントリに書く → 実行要求（ドアベル）→
`CSD_API_RESPONSE` で応答 → エントリ解放**。

起動時には `Execute: USR_INIT_CODE` と **`4KE POWER-ON`** が出る。
∴ **裏 CPU は MIPS 4KE コア**で、起動は `Request_SpecialCmdExecute`
（`CSD_SPECIAL` ＝ SW int 1 が起動時に 1 回だけ鳴るのと一致）。

### 13.3 ★ エントリの語の意味（確定）

§12.2 で「見立て」としていた 32 バイトエントリが決まった。

| 語 | 意味 |
|---|---|
| 0 | 状態/フラグ |
| **1** | **`(コマンド ID << 16) | 連番`** |
| 2 | リクエストの番地（KSEG1・データリング内） |
| 3 | レスポンスの番地 |
| 4 | タイムアウト（`0x7fffffff`） |
| 5 | 0 |
| **6** | **リクエスト長 ＋ レスポンス長** |
| 7 | 0 |

根拠:

- 語 1 の下位 16bit は**枠をまたいで単調増加**する（`015f 0160 0161 0162 0163 0164
  0165 …`）＝連番。上位 16bit は同じ値が何度も現れる＝コマンド ID。
- **リクエスト長 ＝ 語3 − 語2**。語 6 はそれ以上になることがあり、差がレスポンス長。
  csd の printk に `invalid req+res size (%d:%d)` があるのと整合する。

### 13.4 選局 1 回のコマンド列（実測・連番順）

```
194e 1915 1931 1932 1916 190b 194d 190c 1932 1932 1932
1915 1931 1932 191b 1915 1916 190b 190c 1917 190d
```

| 観察 | 内容 |
|---|---|
| 本数 | **21 本**（同じ選局で userland の `val_*` ioctl は 48 本） |
| カテゴリ | **全部 `0x19`**。上位バイトがブロック、下位が機能と見える |
| 長さ | ほとんど 8 バイト。`0x1915` だけ 52 バイト（`0x34`）、`0x194d`/`0x194e`/`0x190d` は 4 バイト |
| **応答があるのは 1 本だけ** | `0x191b` のみ 語6(8) > リクエスト長(4)。残り 20 本は**書きっぱなし**＝printk の `API type : write only` と一致 |

起動時には `0x18xx` と `0x2bxx` のカテゴリも現れる（別ブロック）。

### 13.5 ioctl との対応は**まだ取れていない**

`val_demux` の ioctl `nr` と、コマンド ID の下位バイトは**別の番号体系**だった。
同じ選局での順序列を並べると:

```
ioctl nr : 34 15 2e 2f 16 0c 33 0d 6a 6a 2f 2f 2f 15 2e 2f 15 16 0c 0d ...  (48 本)
cmd 下位 : 4e 15 31 32 16 0b 4d 0c 32 32 32 15 31 32 1b 15 16 0b 0c 17 0d   (21 本)
```

頭の 10 本ほどは対応が付くように見え（`15→15`、`16→16`、`2e→31`、`2f→32`、
`0c→0b`、`0d→0c`、`34→4e`、`33→4d`）、そのあと崩れる。**本数が違う**（48 対 21）
ので、**コマンドを生まない ioctl がある**（`nr=0x2f` のポーリングは csd を通らず
共有メモリから答えていると見られる）。

∴ **位置合わせでは決まらない。** 決めるには **`val_*` の ioctl を 1 本ずつ発行して、
その 1 本でエントリ表がどう動くかを見る**しかない。ここが次。

### 13.6 いま揃っている観察の道具立て

| 何 | どこ | 何が採れるか |
|---|---|---|
| `CSD_DEBUG=65535` | `/app/setting/openird-csddebug` に値、`openird-pre.sh` が insmod | 1 呼び出しごとの確保〜実行〜解放の足跡 |
| エントリ表 | 物理 `0x00010400`（`yb1-mem ... words`） | コマンド ID・連番・長さ・リクエスト番地 |
| データリング | 物理 `0x00010800`（32KB） | リクエストの中身そのもの |
| ioctl トレース | `/app/setting/openird-trace` → `/tmp/yb1-trace.log` | userland 側の呼び出し列と引数 40B |
| 待機時の静粛性 | 実測 API 0 件/5 秒 | **1 操作だけを切り出せる** |

**汎用フックを足したので、以後の実験は焼き直し無しでファイルを置くだけで済む**
（`/app/setting` は yaffs2 rw で再起動を跨ぐ。telnetd と getty はフックより前に
起動するので、フックが壊れても実機には入れる）。

## 14. ioctl → 裏 CPU コマンドの対応表（2026-09-21・実測）

§13.5 で「位置合わせでは決まらない」とした対応を、**ioctl を 1 本ずつ発行して**
決めた。道具は `tools/yb1-ioctl`（要求を 1 回だけ出して前後のバッファを見せる）。

**成立する理由**: 待機中はベンダアプリが API を 1 本も出さない（実測 0 件/5 秒・
§12.4）。∴ 1 発打ってエントリ表（物理 `0x00010400`）を差分すれば、**その 1 本が
生んだコマンドだけ**が残る。

引数は全部ゼロで打った。**多くは検証で弾かれる（`ret = -3`）が、弾かれても
コマンドは積まれるので ID は読める。**

### 14.1 `val_demux`（カテゴリ `0x19`）

| ioctl nr | dir | ret | 生まれたコマンド |
|---|---|---|---|
| `0x02` | R | -3 | `0x1901` (req 4, +resp 4) |
| `0x05` | W | -3 | `0x1904` (req 8) |
| `0x0b` | W | -3 | `0x190a` (req 16, +resp 4) |
| `0x0c` | W | -3 | `0x190b` (req 8) |
| `0x0d` | W | -3 | `0x190c` (req 8) |
| `0x0e` | W | -3 | `0x190d` (req 4) |
| **`0x10`** | W | **40** | **無し（`val_merge` 内で完結）** |
| `0x14` | W | -3 | `0x1914` (req 8, +resp 4) |
| **`0x15`** | W | **40** | **無し** |
| `0x16` | W | -3 | `0x1916` (req 8) |
| `0x17` | W | -3 | `0x1917` (req 8) |
| **`0x19`** | W | **40** | **無し** |
| **`0x1a`** | W | **40** | **無し** |
| **`0x21`** | R | **40** | **無し** ← STC 読み（§10.7.1） |
| **`0x22`** | W | **40** | **無し** ← STC 書き |
| `0x24` | R | -3 | `0x1926` (req 8) |
| `0x26` | W | -3 | `0x1929` (req 12, +resp 4) |
| `0x29` | W | -3 | `0x192c` (req 8) |
| `0x2a` | R | -3 | `0x192d` (req 8) |
| `0x2b` | W | -3 | `0x192e` (req 12, +resp 4) |
| `0x2e` | W | -3 | `0x1931` (req 8) |
| `0x2f` | R | -3 | `0x1932` (req 8) ＋ `0x191b` (req 4, +resp 4) |
| `0x32` | W | -3 | `0x194c` (req 8) |
| `0x33` | W | -3 | `0x194d` (req 4) |
| `0x34` | W | -3 | `0x194e` (req 4) |
| `0x64` | W | **0** | `0x1913` (req 12) |
| `0x65` | W | **0** | `0x1919` (req 12) |
| `0x6a` | W | 0 | 無し |
| `0xc9` | W | 0 | 無し |

### 14.2 読めること

1. **カテゴリ ＝ AV ブロック**。`val_demux`/`val_demux1` → `0x19`、
   `val_m2dsc` → `0x2b`、`val_dcnv` → `0x18`。§13.4 で起動時に `0x18xx` と
   `0x2bxx` が現れたのと一致する。
2. **ioctl nr とコマンドの下位バイトは別番号だが、順序は同じ**。ずれは区間ごとに
   一定（`0x02`〜`0x0e` は −1、`0x14`〜`0x17` は ±0、`0x24`〜`0x2e` は +2〜+3、
   `0x32`〜`0x34` は +26）。∴ **両者は同じ API を同じ順に数えた 2 つの列**で、
   途中に出入りがあるだけ。
3. **コマンド ID の下位は `0x01`〜`0x4e`（78 通り）で、`MMAC_DEMUX_*` の関数 71 本
   とほぼ同数**（§11.4）。∴ **下位バイトは MMAC_DEMUX の関数表の番号**と見てよい。
4. **`val_merge` 内で完結する ioctl がある**（`0x10` `0x15` `0x19` `0x1a` `0x21`
   `0x22` `0x6a` `0xc9`）。裏 CPU を呼ばず `ret = 40`（引数の大きさ）を返す。
   **`0x21`/`0x22` がここに入るのは §10.7.1 の裏づけ**＝STC は共有メモリ上の値で、
   コマンドを出さずに読み書きしている。
5. **`0x2f`（ポーリング）は 2 本のコマンドを生む**。`0x1932`（書きっぱなし）→
   `0x191b`（応答 4 バイト）。「要求を出してから結果を読む」の 2 段。
   §13.4 で選局 1 回のうち応答があるのが `0x191b` だけだったのと符合する。

### 14.3 ★ これは**ベンダアプリの状態を壊す**

掃き終えたあと `X_SetTuning` が `501 Action Failed` を返すようになった。ただし:

- `csd` と裏 CPU は**無傷**。API 呼び出しは流れ続け（1434 → 1454）、
  `dmesg` に timeout も oops も無い。
- `blur` も生きている（75 スレッド）。

∴ 壊れたのは**ドライバではなく `blur` 自身のセッション状態**。`blur` が持っている
ハンドルに対して外から勝手にコマンドを出したため。**電源を入れ直せば戻る**
（実測: 再起動後に `X_SetTuning` が通り、`Cn=30 / Ber=0 / SignalStrength=80` で
ロックした）。

なお**起動直後の 1 分では `X_SetTuning` はまだ `501` を返す**。`blur` の初期化が
終わっていないだけで、2 分ほど置けば通る。再起動して直ったかを判定するときは
待つこと（これで一度「直っていない」と誤判定しかけた）。

**この掃き出しは 1 ブート 1 回きりの破壊的な操作**として扱うこと。読み取り系
（`0x21` など `val_merge` 内で完結するもの）は安全だが、それ以外は掃いたら再起動する。

### 14.4 まだ残っていること

| # | 内容 |
|---|---|
| E1 | コマンド ID ↔ `MMAC_DEMUX_*` の**名前**の対応。番号が関数表の順なら、`MMAC_DEMUX_*` を宣言順に並べれば当てられる可能性がある（節名の並びは `objdump -h` で採れる） |
| E2 | リクエストの中身（引数のレイアウト）。データリング（物理 `0x00010800`）に生で載っているので、**正しい引数で成功させた 1 本**を読めば決まる |
| E3 | `MMAC_DEMUX_SetDeviceInputSource` / `AllocateGeneralPidFilter` / `SetPidFilterBuffer` / `GetPidFilterBufferWritePointerOffset`（§11.4）が、この表のどれか |

## 15. 引数の形から意味を当てる（2026-09-21）

§14.4 の E1（コマンド ID ↔ `MMAC_DEMUX_*` の名前）に取り掛かった。
**番号だけでは決まらなかったが、引数の形から意味が読めた。**

### 15.1 節の並びは「ソースの逆順」だった（が番号は一致しない）

`objdump -h` で `val_merge.ko` の `.text.MMAC_DEMUX_*` を並べると 71 本で、
末尾が `Initialise`、その手前が `OpenDevice` / `CloseDevice` /
`SetDeviceInputSource` / `CpuInputPlay` / `GetCapabilities` / `GetVersion` …
＝**ファイル順はソース定義順の逆**。∴ ソース順の番号は `72 − 節の位置`。

仮説「コマンド ID の下位バイト ＝ ソース順の番号」を §14.1 の表に当てると:

| 一致する | 合わない |
|---|---|
| `0x0a` = `AllocateGeneralPidFilter`（10 番目）| `0x32` = `SelectStreamOutGroup`（50 番目）になるが、`nr=0x2f` は**セッション中 194 回ポーリングされてオフセットを返す**要求で、意味が合わない |
| `0x0b` = `SetPidValue`（11）| コマンド ID は `0x4e`（78）まで出るが、関数は 71 本しかない |
| `0x0c` = `EnablePidFilter`（12）| |
| `0x0d` = `DisablePidFilter`（13）| |

**∴ 部分的にしか成立しない。** 0x0a〜0x0d の並びは引数の形とも合う（下記）ので
偶然とは考えにくいが、全域には広げられない。**番号による同定はここで打ち切る。**

### 15.2 ★ ハンドルは `(種別 << 16) | 番号`（→ §22.2 で**不完全**と判明）

> **訂正**: 正しくは **`(デバイス << 24) | (種別 << 16) | 番号`**。下の表は
> デバイス `0x00`（ベンダが最初に開くもの）だけを見ていた。ベンダは 2 つ目の
> デバイス `0x02010002` も開いており、その下のハンドルは `0x0210006b` /
> `0x02080068` のようになる（§22.2）。我々が開くと `0x01......` になる（§23.2）。
> **なお第 4 のハンドル空間としてバッファオブジェクト `0x000001xx` があり、
> これだけはデバイスに属さない平らな空間**（§25.1）。

選局＋配信を 1 回まわして `val_demux` の ioctl 400 本の引数を並べた結果、
第 1 引数に出る値が 3 種類に分かれた。

| 形 | 実例 | 見立て |
|---|---|---|
| `0x0001xxxx` | `0x00010000` | **デバイス**のハンドル |
| `0x0008xxxx` | `0x00080022` / `0x00080023` | **ペイロード/セクションフィルタ**のハンドル |
| `0x0010xxxx` | `0x0010001f` / `0x00100021` | **PID フィルタ**のハンドル |

### 15.3 ★ 引数から意味が付いたもの

| ioctl | 引数（40B の先頭から） | 回数 | 見立て |
|---|---|---|---|
| `nr=0x21` (R) | `{0x00010000, 0xa90000a0}` | 2 | **`GetStcValue`**。制御ページ **+0xa0** への KSEG1 ポインタを返す＝§10.7.1 の STC |
| `nr=0x22` (W) | `{0x00010000, 0xa90000a8}` | 2 | 同じく **+0xa8**（表示時刻） |
| `nr=0x0b` (W) | 16B 入力 → 4B 応答 | — | **`AllocateGeneralPidFilter`**。応答 4B ＝ **ハンドルを返す** |
| `nr=0x0d` (W) | `{PID フィルタ, 1}` | 25 | **`EnablePidFilter`**（第 2 引数が常に 1） |
| `nr=0x0e` (W) | `{PID フィルタ, 0}` | 13 | **`DisablePidFilter`**（常に 0） |
| `nr=0x2f` (R) | → `{0x00000100, 0x00002fe0}` | **194** | **書き込みポインタのポーリング**。第 2 引数は `0x10` 刻みで進み `0` に折り返す＝**リングのオフセット** |
| `nr=0x2a` (R) | → `{0x00080023, 0x0001f800}` | 2 | `0x1f800` ＝ **126KB** ＝バッファの大きさ/終端 |
| `nr=0x15` (W) | `{0xa9000058, 0x00000100}` | 34 | 第 1 引数が**制御ページ +0x58 へのポインタ**（§10.6 で追いかけていた番地） |
| `nr=0x16`/`0x17` | `{PID フィルタ, 0x0100 or 0x0102}` | 22 / 12 | 第 2 引数は PID かストリーム ID |

**`nr=0x21`/`0x22` が制御ページの `+0xa0`/`+0xa8` を指しているのは、§10.7.1 の
「あれは STC と表示時刻」という結論の 3 度目の裏づけ**（1 度目はレートの実測、
2 度目は §14.1 で「コマンドを生まない」こと）。**ポインタを返すだけなので
裏 CPU を呼ぶ必要が無い**＝両方の観察が同じ 1 つの事実を指している。

### 15.4 E3 に効く形（段階 C の経路）

§11.4 で挙げた「chardev に要る 5 本」のうち、**引数の形から当たりが付いたのは
3 本**:

| 欲しい関数 | 対応しそうな ioctl | 根拠 |
|---|---|---|
| `AllocateGeneralPidFilter` | `nr=0x0b` | 16B 入力 → 4B のハンドル応答。コマンド `0x0a` はソース順 10 番目＝同名 |
| `EnablePidFilter` | `nr=0x0d` | 第 2 引数が常に 1。コマンド `0x0c` は 12 番目＝同名 |
| `GetPidFilterBufferWritePointerOffset` | `nr=0x2f` | 194 回ポーリングされ、`0x10` 刻みで進み折り返すオフセットを返す |
| `SetPidFilterBuffer` | **未同定** | 起動時トレースに `nr=0x26` が `{0x00080023, 0xa9464000, 0x00020000, 0xa90000b8}` ＝**番地 ＋ 0x20000(128KB) ＋ 制御ページ +0xb8** を渡す例がある。ペイロード側かもしれず、確証はまだ |
| `SetDeviceInputSource` | **未同定** | |

### 15.5 次

- **E2（引数のレイアウト）へ**。`nr=0x0b` を**正しい引数で成功させて**、
  データリング（物理 `0x00010800`）に載ったリクエスト 16B をそのまま読む。
  ハンドルの割り当てが見えれば、そこから芋づるに辿れる。
- `nr=0x26` が `SetPidFilterBuffer` か `SetPayloadBuffer` かを、渡す番地と
  `nr=0x2a`/`0x2f` が返す値の対応で決める。

## 16. ★ PID フィルタを自分で確保・有効化できた（2026-09-21）

§15.5 の E2（引数のレイアウト）に取り掛かったところ、**そのまま E3 まで通った**。
段階 C の経路が実機で動くことを確かめた最初の例。

### 16.1 ioctl 1 本がコマンドになるまで（完全に追えた）

`nr=0x0b` に `{6, 0xa9000040, 0x00010000, 0xa9000044}` を渡して `ret = 0`:

```
ioctl 引数   : {種別, 入力ポインタ(KSEG1), デバイス, 出力ポインタ(KSEG1)}
      ↓ val_merge
コマンド 0x190a のリクエスト 16B:
              00000006  a0015890  00010000  01010100
                          └─ リクエスト自身の第 4 語を指す（＝ +0x0c）
      ↓ 裏 CPU
レスポンス 4B: 00080005                    ← 新しいハンドル
      ↓ val_merge
制御ページ +0x44: 01100037 → 00080005      ← 出力ポインタの先へ書き戻し
```

**読めること**:

- `val_merge` は**入力の構造体を制御ページからデータリングへ複写し、ポインタを
  リクエスト内を指すように書き換える**。∴ 裏 CPU から見ると引数は全部
  リクエストの中に閉じている。
- 応答はハンドルで、`val_merge` が ioctl の出力ポインタの先へ書き戻す。
- ∴ **ioctl の 40B は「裏 CPU のリクエスト」そのものではない**。
  `val_merge` が詰め替えている。段階 C は ioctl ではなく**リクエストの形**を作る。

### 16.2 ★ 種別 7 が PID フィルタ

`nr=0x0b` の第 1 引数（種別）を 0〜15 で掃いた。**通るのは 6 と 7 だけ**:

| 種別 | ret | 返ったハンドル |
|---|---|---|
| 6 | **0** | `0x00080004` … 種別 `0x08` ＝ ペイロード/セクションフィルタ |
| **7** | **0** | **`0x00100003` … 種別 `0x10` ＝ PID フィルタ** |
| その他 | -3 | （失敗。応答の枠にはリングの残骸が見えるだけ） |

起動時のトレースで `val_demux1 nr=0x0b` が使っていたのも 6 と 7 で、一致する。
§15.2 のハンドル種別（`0x0008xxxx` / `0x0010xxxx`）とも一致する。

### 16.3 ★ 確保したハンドルが実際に使えた

種別 7 で取った `0x00100002` に対して:

| ioctl | ret | コマンド | リクエスト |
|---|---|---|---|
| `nr=0x0d` `{h, 1}` | **0** | `0x190c` | `{00100002, 00000001}` |
| `nr=0x0e` `{h}` | **0** | `0x190d` | `{00100002}` |
| `nr=0x0c` `{h, 0x100}` | **-8** | `0x190b` | `{00100002, 00000100}` |
| `nr=0x2f` `{h}` | -3 | `0x191b` | `{00100002}` |

**`0x190a` 〜 `0x190d` は、ソース順の 10〜13 番＝`AllocateGeneralPidFilter` /
`SetPidValue` / `EnablePidFilter` / `DisablePidFilter` と完全に並ぶ。**
§15.1 で「部分的にしか成立しない」とした仮説が、この範囲では
**3 つの独立な根拠（番号の並び・引数の形・実際に成功する振る舞い）で裏づけられた。**

∴ **OpenIRD は PID フィルタを自分で確保し、有効化し、無効化できる。**
これは段階 C の chardev 経路のうち、**フィルタ側が成立することの実機証明**である。

### 16.4 残り（**F1〜F3 は解決済み**。下表に結果を併記）

| # | 内容 |
|---|---|
| F1 | `nr=0x0c`（`SetPidValue`）が `-8` で弾かれる。順序の制約と見られる<br>**→ 解決。順序の問題だった**（確保 → PID → … の順なら通る・§19.2）。**加えて `-8` は「その PID を既に他のフィルタが持っている」の意味も持つ**（§19.4／§20.5） |
| F2 | **`SetPidFilterBuffer`**（リングの番地と大きさを渡す）の同定。`nr=0x26` が起動時に `{0x00080023, 0xa9464000, 0x00020000, 0xa90000b8}` ＝番地＋128KB＋制御ページ +0xb8 を渡す例があり有力。ソース順では 38 番＝`0x1926`、これは `nr=0x24` が生むコマンドと一致する |
| F3 | `SetDeviceInputSource`（ソース順 4 番＝`0x1904`、`nr=0x05` が生む）の引数<br>**→ 解決。`{デバイス, 3}`**（§22.1）。実際に `ret=0` で通る（§24.3） |
| F4 | ここまでを繋いで、**スクランブルされたままの TS を自前のリングに落とす**（D4 の実証）<br>**→ 未達。** 設定は全部通るが（§24.3）、ベンダアプリ抜きでは復調器が動かず TS が無い（§25.3）。**A1 待ち** |

**F2 の手がかり**: ソース順 38 = `SetPidFilterBuffer` = `0x1926` で、§14.1 では
**`nr=0x24` が `0x1926` を生む**。∴ **`SetPidFilterBuffer` は `nr=0x24`** の可能性が
高い（`nr=0x26` ではない）。次はここを確かめる。

> **訂正（§17.1）**: この予想は**外れ**。`nr=0x24` は `dir=R` の要求で、書き込みと
> して出すと `val_merge` が `-7`（`E2BIG`）で弾く。**自前のリングの番地を実際に
> 受け取ったのは `nr=0x26`** だった。∴ ソース順の番号から名前を当てる筋は、
> §15.1 に書いたとおりここでも当てにできない。

### 16.5 注意（実験の後始末）

確保したフィルタは解放していないので、**掃いたあとは電源を入れ直す**こと
（§14.3 と同じ。`blur` のセッションも壊れる）。

## 17. ★ 自前のリングをハードウェアに登録できた（2026-09-21）

§16.4 の F2。**`SetPidFilterBuffer` 相当の呼び出しを見つけ、自前のバッファを
AV ハードウェアのディスクリプタに書き込ませることに成功した。**

### 17.1 ソース順からの予想は外れた

§16.4 で「ソース順 38 = `SetPidFilterBuffer` = `0x1926` ＝ `nr=0x24`」と予想したが
**外れ**。`nr=0x24` は `dir=R` の要求で、書き込みとして出すと `val_merge` が
`-7`（`E2BIG`）で弾く。**引数を実際に受け取ったのは `nr=0x26`**（コマンド `0x1929`）。

∴ §15.1 に書いたとおり、**ソース順の番号から名前を当てる筋はやはり当てにできない。**
以後は「実際に何を受け取り何を返すか」だけで同定する。

### 17.2 ★ ハンドルの種別が合っていないと通らない

`nr=0x26` に `{ハンドル, リングの番地(KSEG1), 大きさ, ディスクリプタの置き場}` を
渡す。**ハンドルの種別が効く**:

| 渡したハンドル | ret | 結果 |
|---|---|---|
| `0x00100004`（種別 7 ＝ PID フィルタ） | **-3** | 応答 `0x00000000`。リングの番地を変えても同じ |
| **`0x00080003`（種別 6 ＝ ペイロード/セクションフィルタ）** | **0** | **成功。応答 `0xb0033420`** |

起動時トレースでベンダが `nr=0x26` に渡していたのも `0x00080023` ＝**種別 6 の
ハンドル**で、一致する。∴ **`nr=0x26` はペイロード側のバッファ設定**であって、
PID フィルタ側のバッファ設定は別の呼び出し（未同定）。

### 17.3 ★ 制御ページ `+0xb8` の正体＝バッファディスクリプタ

種別 6 のハンドルで成功した瞬間、**ディスクリプタの置き場として渡した
`0xa90000b8`（＝制御ページ +0xb8）に、我々のリングが書き込まれた**:

```
0x090000b8: 09484000 -> 0c000000     base  ← 我々が渡した番地
0x090000bc: 094a3fff -> 0c01ffff     end   ← base + 0x20000 - 1（渡した大きさ）
0x090000c0: 09484000 -> 0c000000     ptr
0x090000c4: 09484000 -> 0c000000     ptr
0x090000c8: 09484000 -> 0c000000     ptr
```

**§10.5 で「`[base][end][ptr][ptr][ptr]` の形が並ぶが用途不明」としていたものは、
バッファディスクリプタだった。** しかも**我々が置き場所と中身の両方を決められる**
（ベンダは `+0xb8` と `+0xd0` を使っていたが、それは `val_merge` に渡す引数でしか
ない）。§10.5 の宿題がここで片付いた。

∴ **段階 C は、共有メモリ上に自前のディスクリプタを作って、そこへリングを
登録できる。** 応答の `0xb0033420` は物理 `0x10033420` ＝ SoC の I/O 窓
（`EMMA_IO_BASE` の中）を指す値で、何を意味するかは未確定。

### 17.4 まだ TS は落ちていない

`enable` が `-8` で弾かれ、リング（物理 `0x0c000000`）は書かれていない
（全域 `0xff` のまま＝未初期化 DRAM）。順序か、足りない設定がある。

現時点で通る/通らないの切り分け:

| 呼び出し | 種別 7（PID フィルタ） | 種別 6（ペイロード） |
|---|---|---|
| `nr=0x0b` 確保 | **0** | **0** |
| `nr=0x0c` PID 設定 | **0** | **0** |
| `nr=0x26` バッファ | -3 | **0** |
| `nr=0x0d` 有効化 | **0**（バッファ無しでも通る） | -8 |
| `nr=0x2f` 書き込みポインタ | -3 | -3 |

**読めること**: PID フィルタ側は「確保・PID 設定・有効化」まで通るがバッファを
付ける呼び出しが未同定。ペイロード側はバッファまで通るが有効化が通らない。
**両者をつなぐ段（PID フィルタの出力をペイロードバッファへ流す設定）が
抜けている**と見るのが自然。

### 17.5 次（**G1〜G3 は解決済み**。下表に結果を併記）

| # | 内容 |
|---|---|
| G1 | **PID フィルタとペイロードバッファを結ぶ呼び出し**<br>**→ 解決。`nr=0x16`**（§19.1）。ただし**これは 1 つめのデバイス（テーブル系）専用**で、2 つめのデバイス側は `nr=0x30` でバッファを受け取る（§23.4） |
| G2 | ペイロード側 `enable` の `-8`<br>**→ 解決。`nr=0x29`（リセット）を呼んでいなかった**（§19.2） |
| G3 | `nr=0x05`（`SetDeviceInputSource` 候補）。入力源の選択が要るか<br>**→ 解決。要る。`{デバイス, 3}`**（§22.1／§24.3） |
| G4 | 通ったら **PID を映像 PID にして `scrambling_control` を見る**＝D4 の実証<br>**→ 未達**（§25.3・A1 待ち） |

**掃き方の教訓**: この種の実験は**選局せずに待機のまま**やること。選局すると
ベンダが API を出し始め（§12.4）、エントリ表の差分が汚れて別のハンドルを
拾ってしまう（一度踏んだ）。リクエストに自分のハンドルが入っているエントリだけを
採るようにした。

## 18. G1〜G4 の試行と、総当たりを打ち切った理由（2026-09-21）

§17.5 の G1（PID フィルタとペイロードバッファを結ぶ段）を探した。**見つからず、
総当たりは割に合わないと分かった。** 得られた事実と、次に採るべき道を残す。

### 18.1 確保の `params` は 3 バイトしか複写されない

確保（`nr=0x0b`）のリクエスト第 4 語 `params` は、**入力ポインタの先から複写される**。
複写の大きさを測ると:

| 入力ポインタの先の値 | リクエストの `params` |
|---|---|
| `0x010101ff` | `0x01010100` |
| `0x00100005` | `0x00100000` |

**下位 1 バイトが落ちる＝複写は 3 バイト**。∴ **`params` にハンドルを載せることは
できない**（ハンドルの下位＝通し番号が失われる）。「確保のときに PID フィルタの
ハンドルを渡して結びつける」という筋は**否定された**。

### 18.2 有効化は「呼び出しが違う」のではなく「前提が足りない」

`nr=0x0d`（有効化）の戻り値は種別で違う:

| ハンドル | ret | 意味 |
|---|---|---|
| 種別 7（PID フィルタ） | **0** | 通る（バッファ未設定でも） |
| 種別 6（ペイロード）＋バッファ設定済み | **-8**（`ENOEXEC`） | **ハンドルは受け付けたうえで「実行できない」** |

`-8` は `-3`（`ESRCH` ＝ ハンドル違い）とは別物。∴ **`nr=0x0d` はペイロード側でも
正しい呼び出しで、足りないのは前提**。

候補の `nr`（`0x11`〜`0x20`）を種別 6 のハンドルで掃いたが、**全部 `-7` か `-3`**
＝別の有効化呼び出しは無い。

### 18.3 TS 入力が動いていないせいでもなかった

§10.4 の「取り込みを始めるのは配信セッション」に従い、**ベンダに選局＋配信させた
状態**で同じ手順を踏んだ。結果は**同じ `-8`**。リングも書かれない。
∴ **前提は TS 入力の有無ではない。**

### 18.4 ★ 総当たりは実機を落とす

`nr` を総当たりすると**実機が再起動する**。確認できた範囲では:

- **`nr=0x06` に `{ハンドル, 番地, 大きさ, 置き場}` を渡すと落ちる**（再現 1 回）
- `nr=0x00`〜`0x04` の範囲でも落ちた（`{h7, h6, 0, 0}` を掃いていて 1 回）

1 回落ちるごとに復旧（電源＋ベンダアプリの初期化待ち）で 2 分かかるので、
**総当たりは割に合わない**。**この方向は打ち切る。**

### 18.5 次に採る道＝ベンダの初期化列をそのまま写す

総当たりの代わりに、**ベンダ自身がペイロードフィルタを立ち上げる手順を、引数ごと
そのまま採って写す**のが筋。材料は揃っている:

- `tools/yb1-trace` は `val_demux` の ioctl を**引数 40B ごと**記録できる（§11.8）。
- §15.3 でやったのは「`nr` ごとの引数の形」までで、**順序つきの完全な初期化列**は
  まだ採っていない。
- 起動〜最初の配信までを 1 本の列として採り、**`nr=0x0b`（確保）から
  `nr=0x0d`（有効化）が通るまでに何を挟んでいるか**を見れば、足りない前提が分かる。

∴ 次は**総当たりではなく、順序つきトレースの取得と読解**。

### 18.6 いまの到達点（段階 C から見て）

| できること | 状態 |
|---|---|
| フィルタの確保（PID 用・ペイロード用） | **実機で成功** |
| PID の設定 | **実機で成功** |
| **自前のリングをハードのディスクリプタに登録** | **実機で成功**（§17.3） |
| PID フィルタの有効化 | **実機で成功**（ただしバッファ未接続） |
| ペイロードフィルタの有効化 | **前提が足りず `-8`** |
| リングに TS が落ちる | **未達** |

**残っているのは「有効化の前提」1 点**で、経路の部品はすべて実機で動かせている。

## 19. ★ ベンダの初期化列を写して `enable` を通した（2026-09-22）

§18.5 の方針どおり、総当たりをやめて**ベンダ自身の初期化列を順序つきで採り、
そのまま写した**。**`-8` の前提が分かり、有効化まで通った。**

### 19.1 ベンダの列（`val_demux` の ioctl 640 本から）

第 1 引数（ハンドル）ごとにまとめると、**PID フィルタは例外なく同じ 3 手**:

```
nr=0x16  {PID フィルタ 0x0010xxxx, バッファ 0x000001xx}   ← 結びつけ
nr=0x0c  {PID フィルタ, PID}                              ← PID 設定
nr=0x0d  {PID フィルタ, 1}                                ← 有効化
その後   nr=0x2e/0x2f を **バッファ 0x000001xx の側**に対してポーリング
```

実例（1:1 に対応する）:

| PID フィルタ | `nr=0x16` の相手 | `nr=0x0c` の PID |
|---|---|---|
| `0x00100021` | `0x00000100` | `0x0000` |
| `0x0010001f` | `0x00000102` | `0x0010` |
| `0x00100012` | `0x0000010f` | `0x0012` |
| `0x0010001a` | `0x00000107` | `0x0902` |
| `0x0010001e` | `0x00000103` | `0x0010` |
| `0x0010001d` | `0x00000104` | `0x0011` |
| `0x0010001b` | `0x00000106` | `0x0024` |
| `0x00100018` | `0x00000109` | `0x0023` |

**`nr=0x0c` の第 2 引数は本物の ARIB の PID だった**: `0x0000`(PAT)、`0x0010`(NIT)、
`0x0011`(SDT)、`0x0012`(EIT)、`0x0014`(TDT)、`0x0023`/`0x0024`/`0x0028`/`0x0029`
(SDTT/BIT/CDT)、`0x0900`/`0x0902`。∴ **`nr=0x0c` ＝ `SetPidValue` が確定。**

∴ **`nr=0x16` は「PID フィルタをバッファに結びつける」**＝§17.5 の G1 の答え。
`nr=0x17` は同じ組で呼ばれており、外すほうと見られる。

**`0x000001xx` は第 4 のハンドル種別**（§15.2 の 3 種に加えて）で、
**バッファ側のオブジェクト**。`nr=0x2e`/`0x2f` はこちらに対して呼ぶ。

### 19.2 ★ ペイロード側は `reset` が前提だった

ペイロードフィルタ（`0x0008xxxx`）のベンダの列は `nr=0x16` を使わず:

```
nr=0x29 {h}        ← リセット      ★ こちらが呼んでいなかった
nr=0x2a {h}        ← 書き込みポインタ
nr=0x0c {h, PID}   ← PID 設定
nr=0x0d {h, 1}     ← 有効化
```

**同じ順で写したら全部 `ret = 0` になった**（`enable` を含む）:

```
alloc(kind=6)   ret=0   → 0x00080005
buffer          ret=0
reset (nr=0x29) ret=0
setpid 0x0000   ret=0
enable          ret=0   ★
```

∴ **§18.2 の `-8` の正体は「リセットしていない」だった。**

### 19.3 ★ ハードウェアが我々のメモリに書いた

`reset` を呼んだ瞬間、物理 `0x0c000000` のリングが
**`ff ff ff …`（未初期化 DRAM）から `00 00 00 …` に変わった**。

∴ **AV サブシステムが、我々が指定したメモリに実際に書き込んでいる。**
§17.3 でディスクリプタに登録できたことと合わせて、**経路がハードウェアまで
届いていることの直接の証拠**。

### 19.4 ★ PID は 1 つのフィルタにしか渡せない

配信セッションを張った状態で同じ手順を踏むと、**PID によって結果が変わる**:

| PID | `enable` |
|---|---|
| `0x0000`（PAT・**ベンダが掴んでいる**） | **-8** |
| `0x0111`（ベンダが使っていない） | **0** |

∴ **`-8` は「その PID は既に他のフィルタが持っている」の意味でもある。**
待機中に `0x0000` が通ったのは、ベンダがまだ掴んでいなかったから。

### 19.5 まだ TS は落ちていない（が理由の見当はついた）

`enable = 0` のあと 6 秒で**実機が再起動**し、リングは `0x00` のまま（リセット後の
状態が再起動を跨いで残っていた＝DRAM の内容は消えていない）。∴ **その間に
TS は書かれていない。**

見当: **種別 6 は「セクション/ペイロードフィルタ」**で、PSI のセクションを
取り出す口と見られる。`0x0111`（映像の PES）を入れても出るものが無い。
**生の TS が欲しいなら、ベンダが映像に使っている経路＝`nr=0x16` で
バッファオブジェクト `0x000001xx` に結びつける PID フィルタ（種別 7）側**である。

∴ 次の問いは **`0x000001xx` のバッファオブジェクトはどこで作られるか**。
`val_demux` の `nr=0x0b` は `0x0008xxxx` と `0x0010xxxx` しか返さないので、
**別のデバイス（`val_video` / `val_capture` / `val_record` など）か `val_demux1`**
が作っていると見られる。

### 19.6 段階 C から見た到達点

| | |
|---|---|
| フィルタの確保（2 種別） | **成功** |
| 自前リングのディスクリプタ登録 | **成功**（§17.3） |
| リセット・PID 設定・**有効化** | **成功**（§19.2） |
| **ハードが自前メモリに書く** | **確認**（§19.3） |
| 生 TS がリングに落ちる | 未達（§19.5 の見当） |

**ABI の作法は割れた**（確保 → バッファ → リセット → PID → 有効化、
ハンドルは 4 種別、PID は排他）。残るのは**どのオブジェクトに結びつけると
生 TS が出るか**の 1 点。

### 19.7 運用メモ

- **ツールは `/app/setting/` に置く**。`/tmp` は再起動で消えるうえ、この実験は
  実機をよく落とすので毎回 push し直すことになる。`/app/setting` は yaffs2 rw
  で再起動を跨ぐ（`yb1-ioctl` はここに置いた）。
- **リングの書き込みは再起動を跨いで残る**（DRAM は消えない）。∴ 落ちても
  取り込めたかどうかは後から確認できる。

## 20. 生 TS の経路を全部通したが、データが流れない（2026-09-22）

§19.5 の問い（`0x000001xx` のバッファオブジェクトはどこから来るか）を追い、
**経路の呼び出しは全部見つけて通した。しかしデータが落ちない。**

### 20.1 `0x000001xx` は確保されるものではなかった

全デバイスの ioctl を時刻順に見て、各バッファ ID が**最初に現れる行**を探した。
結果は 10 個すべて同じ:

```
nr=0x15  {0xa9000058, 0x000001xx}
```

`nr=0x15` は §14.1 で **`ret=40` を返しコマンドを生まない**＝`val_merge` 内で
完結する呼び出しと分かっている。∴ **これは確保ではなく問い合わせ**で、
`0x000001xx` は**固定の番号**（`xx` は `0x00`〜`0x0f` の 16 個）と見られる。

第 1 引数の `0xa9000058` は**制御ページ +0x58** ＝ §10.6 で長く追いかけていた番地。
ここに結果が書かれる口だった。

### 20.2 ★ バッファオブジェクトに自前のリングを与える呼び出し

`0x000001xx` を第 1 引数に取る `nr` を掃いて見つけた:

| `nr` | `{buf, ring, size}` | 効果 |
|---|---|---|
| **`0x2b`** | **ret=0** | **ディスクリプタに自前のリングが入る** |
| `0x2c` / `0x2d` | ret=0 | ディスクリプタは我々の値のまま（ポインタ側の設定と見られる） |
| `0x1a` | ret=0 | ディスクリプタは変わらず |
| `0x30` | ret=40 | `val_merge` 内で完結 |
| `0x2e` | ret=-8（この引数では） | 別途 `{buf, 0}` なら ret=0 |

`nr=0x2b` 実行後の制御ページ:

```
+0xb8: 0c000000 0c01ffff 0c000000 0c000000 0c000000
```

∴ **`nr=0x2b` ＝ バッファオブジェクト版の「バッファ設定」**（`nr=0x26` が
ペイロードフィルタ版だったのと対になる）。

### 20.3 経路は全部 `ret=0` で通る

ベンダの列をそのまま写した順番:

```
nr=0x2b {buf, ring, size, desc}   ret=0   ← 自前のリングを与える
nr=0x2e {buf, 0}                  ret=0   ← バッファ開始
nr=0x0b {7, in, dev, out}         ret=0   ← PID フィルタ確保 → 0x00100005
nr=0x16 {filter, buf}             ret=0   ← 結びつけ
nr=0x0c {filter, 0x0111}          ret=0   ← PID
nr=0x0d {filter, 1}               ret=0   ← 有効化
nr=0x2f {buf}                     ret=0   ← 書き込みポインタ
```

**全部通る。**（ベンダに選局＋配信させて TS 入力が動いている状態で実施。）

### 20.4 ★ しかしデータが落ちない

- `nr=0x2f` が返す書き込みポインタは **4 回読んでも `0x00000000` のまま**
  （ベンダのバッファでは `0x1c40`・`0x2fe0` などと進み折り返す）。
- リング（物理 `0x0c000000`）に TS は無い。

∴ **ABI の作法は正しいが、まだ「取り込みが始まる条件」を満たしていない。**
考えられる先:

| # | 内容 |
|---|---|
| H1 | **バッファ id `0x0108` が実在の口でない**可能性。ベンダが使うのは `0x0100`〜`0x0107` / `0x0109`〜`0x010c` / `0x010f` で、**`0x0108` だけ使われていない**。空いているから選んだが、**そもそも無効な番号**かもしれない |
| H2 | PID `0x0111` が、この分岐点では流れていない可能性 |
| H3 | 入力源の選択（`SetDeviceInputSource` 相当・§16.4 F3）が要る可能性 |
| H4 | `nr=0x2c`/`0x2d`（読み出しポインタ側と見られる）の初期化が要る可能性 |

### 20.5 ★ PID の排他は実測で 2 度確認した

同じ PID で 2 回続けて組むと、**2 回目の `nr=0x0d` が `-8`** になる。1 回目の
フィルタを解放していないため。**解放しない限り再起動まで PID は掴まれたまま。**

∴ 実験のたびに電源を入れ直すこと。§19.4 で「ベンダが掴んでいる PID は使えない」
と書いたのと同じ規則が、**自分が漏らしたフィルタにも効く**。

### 20.6 いま分かっている呼び出し一覧（`val_demux` / カテゴリ `0x19`）

**この表がこの文書の実用的な索引。** 2026-09-22 に §22〜§24 の分を足した。

| `nr` | 役割 | 根拠 |
|---|---|---|
| **`0x01`** | **Initialise（デバイス全体）。これを呼ばないと以降が全部 `-2`** | §24.2 |
| **`0x02`** | **デバイスを開く。呼ぶたびに別の実体を返す**（`{1, ptr}` → ptr にハンドル） | §22.1 / §23.2 |
| **`0x05`** | **入力源の選択**（`{dev, 3}`） | §22.1 |
| `0x0b` | フィルタ確保（種別 6=ペイロード / 7=PID） | §16.2 |
| `0x0c` | PID 設定 | ARIB の PID と一致（§19.1） |
| `0x0d` | 有効化 | §16.3 |
| `0x0e` | 無効化 | §16.3 |
| `0x15` | バッファオブジェクトの問い合わせ（`val_merge` 内） | §20.1 |
| `0x16` | **PID フィルタをバッファに結びつける** | §19.1 |
| `0x17` | 同じ組で呼ばれる（外す側と見られる） | §19.1 |
| `0x21` / `0x22` | STC の読み/書き（共有メモリ・コマンド無し） | §15.3 |
| `0x26` | **ペイロードフィルタのバッファ設定** | §17.2 |
| `0x29` | ペイロードフィルタのリセット（**有効化の前提**） | §19.2 |
| `0x2a` | ペイロードフィルタの書き込みポインタ | §15.3 |
| `0x2b` | **バッファオブジェクトのバッファ設定** | §20.2 |
| `0x2e` | バッファ開始 | §20.3 |
| `0x2f` | バッファの書き込みポインタ（**読み手ごとの手元にある量**） | §19.1 / §21.3 |
| **`0x30`** | **確保系。第 5 引数の先に種別 `0x2000` のハンドルを書く。起動時 1 回きり** | §23.6 / §24.4 |
| **`0x32`** | **有効化の前に呼ぶ**（2 つめのデバイス側の列に出る） | §22.4 / §24.3 |
| **`0x48`** | **デバイスを開いた直後に呼ぶ**（`{dev, 1}`） | §22.1 / §24.3 |
| `0x06` | **実機が落ちる。触らないこと** | §18.4 |

**通る順番**（ベンダアプリ抜き・全部 `ret=0`）は §24.3。
**同居はできない**（§25）。

**戻り値の読み方**（実測で意味が割れたもの。切り分けに効く）:

| ret | 読み |
|---|---|
| `40` | `val_merge` 内で完結した（引数の大きさを返している）。**裏 CPU を呼んでいない** |
| `-2` | AV サブシステムが未初期化＝`nr=0x01` を呼んでいない（§24.2） |
| `-3` | ハンドルが違う（種別・デバイス・存在しない id）（§17.2／§25.1） |
| `-7` | その `nr` は無い、または方向・大きさが違う（`dir=R` の要求を W で出した等・§17.1） |
| `-8` | 呼び出しは受け付けたが実行できない。**前提不足**（リセット未・§19.2）か**資源の取り合い**（PID が先取りされている・§19.4／`nr=0x30` が取られている・§24.3） |

## 21. 候補を 3 つ潰した（2026-09-22）

§20.4 の H1〜H4 を順に当たった。**3 つ潰れて、まだ流れない。** 否定の記録。

### 21.1 H1（バッファ id が無効）は否定

`nr=0x15` は `val_merge` 内で完結し副作用が無い（§20.1）ので、安全に掃ける。

| id | `nr=0x15` |
|---|---|
| `0x0100`〜**`0x0113`** | **全部 `ret=0`** |
| `0x0000` / `0x00ff` / `0x0200` | `-3` |

∴ **`0x0108` は実在する。** id の範囲は少なくとも `0x0100`〜`0x0113`（20 個）で、
16 個ではなかった。**H1 は否定。**

### 21.2 H2（PID が流れていない）も弱い

選局のみの状態で **PID `0x0000`（PAT・どの TS にも必ずある）** を、ベンダが
使っていないバッファ `0x0113` に向けて 8 回ポーリングした。**全部 0。**
∴ PID の選び方の問題ではない。

### 21.3 ★ 対照実験＝ベンダのバッファを我々が読むと 0

ここが効いた。**ベンダが現に配信できていることを先に確認**した:

- HTTP で **12.5 MB** 受信、この間の API 呼び出し **+422**
- トランスコーダのリング（`0x0acea000`）に TS が 5575 個

その状態で、**ベンダが使っているバッファ `0x0100` を我々の道具で読むと `0`**。
一方トレースでは、**同じデバイス・同じ `nr=0x2f`・同じ id に対して**ベンダは
`0x2fe0` / `0x9f00` / `0x7c00` と**非ゼロを受け取っている**（同じ時間帯）。

∴ **`nr=0x2f` が返すのは「読み手ごとの手元にある量」**であって、バッファの
絶対的な書き込み位置ではない。**我々の文脈にはデータが無いから 0。**

### 21.4 H「状態は open ごと」は実装して確かめ、否定

上を「fd ごとに文脈が違うからでは」と考え、**`tools/yb1-ioctl` に
スクリプト形式（1 つの fd の上で複数の要求を順に出す）を足した**。

```
yb1-ioctl <device> @<script>
  <cmd> [word ...]     その要求を出す（引数は 32bit 語で書ける）
  hold <seconds>       fd を開いたまま待つ
```

配信中に**同じ fd で**ベンダのバッファ `0x0100` を 6 回読んだ。**全部 0。**
∴ **fd を跨がないようにしても変わらない。この仮説は否定。**

ただし**道具としては残す価値がある**（この ABI は文脈を持つので、経路を組む
には 1 つの fd で一気に流せる形が要る）。

### 21.5 いま言えること

- **呼び出しは全部正しく通る**（`ret=0`）。作法は §20.6 の表のとおり。
- **ハードは我々のメモリを見ている**（§17.3 のディスクリプタ登録、§19.3 の
  リセットによるゼロ埋め）。
- **しかし取り込みは始まらない**。`nr=0x2f` が読み手ごとの値である以上、
  **0 は「まだ何も入っていない」であって「読み方が違う」ではない。**

∴ 残る筋は **H3（入力源の選択が要る）**が本命。`nr=0x05` が
`SetDeviceInputSource` の候補（§16.4 F3）で、まだ引数が分かっていない。
ベンダが起動時に `nr=0x05` へ何を渡しているかを、**引数つきで採る**のが次。

### 21.6 新しく見えたこと

トレースの `nr=0x2f` の応答には `0x00000105` のような形に混じって
**`0x01000123`** という別の形も出る。∴ バッファ id の空間は
`0x000001xx` だけではない。`nr=0x15` が `0x0100`〜`0x0113` を受けたのとも
合わせて、**id の体系はまだ全部見えていない。**

## 22. ★ デバイスが 2 つあった（2026-09-22）

§21.5 の本命 H3（入力源の選択）を追って、起動直後のトレースを引数つきで採った。
**`nr=0x05` の正体が分かり、同時に「こちらがずっと間違ったデバイスを使っていた」
ことが判明した。**

### 22.1 起動直後のデバイス初期化列

```
nr=0x01  {0, 0, 0, 0}
nr=0x02  {1, 0xa9000000}  (R)        ← デバイスを開く。ハンドルが返る
nr=0x05  {0x00010000, 0x00000003}    ← ★ 入力源の選択（源 = 3）
nr=0x65  {0, 1} / {1, 1}
nr=0x64  {0, 1} / {1, 1}
nr=0x02  {1, 0xa9000000}  (R)        ← ★ 2 つめのデバイスを開く
nr=0x48  {0x02010002, 1}
nr=0x05  {0x02010002, 0x00000003}    ← 2 つめにも入力源を設定
```

∴ **`nr=0x02` ＝ デバイスを開く**（`OpenDevice`）、
**`nr=0x05` ＝ 入力源の選択**（`SetDeviceInputSource`、引数は `{デバイス, 3}`）。

**デバイスは 2 つある**: `0x00010000` と **`0x02010002`**。

### 22.2 ★ ハンドルはデバイスを含んでいる

2 つめのデバイスの下で確保されたフィルタのハンドルは:

```
0x0210006b  0x0210006a  0x02100069   ← 種別 0x10（PID フィルタ）
0x02080068  0x02080067  0x02080066   ← 種別 0x08（ペイロード）
```

こちらが今まで見ていたのは `0x0010001f` / `0x00080023`。
∴ **ハンドルは `(デバイス << 24) | (種別 << 16) | 番号`** の形で、
**上位バイトがどのデバイスに属するかを表す**（`0x00` と `0x02`）。

§15.2 で「ハンドルは `(種別 << 16) | 番号`」と書いたのは**不完全**だった。

### 22.3 ★ 実サービスの PID は 2 つめのデバイスに載る

起動時に `nr=0x0c` で設定される PID を並べると:

| ハンドル | PID | |
|---|---|---|
| `0x0210006a` | `0x01f0` | |
| `0x02100069` | `0x01ff` | |
| `0x02080068` | **`0x0100`** | 映像 |
| `0x02080067` | **`0x0110`** | 音声 |
| `0x02080066` | `0x0111` | |
| `0x02080065` | `0x0130` | |
| `0x02080064` | `0x0138` | |
| `0x02080063`〜`0x0208005e` | `0x0112`〜`0x0117` | |
| `0x0210005d` | `0x0012` | EIT |

**番組の実 PID（映像 `0x0100`・音声 `0x0110` ほか）は全部デバイス `0x02010002`
の側**に載っている。§19.1 で見ていた `0x0010xxxx` の側は、PAT/NIT/SDT/TDT など
**テーブル用の別系統**だった。

**∴ §20〜§21 で TS が落ちなかった最大の理由はこれ。** 確保のとき第 3 引数に
`0x00010000` を渡していた（＝テーブル側のデバイス）。ベンダは実サービスには
`0x02010002` を渡している。

### 22.4 もう 1 つ抜けていた手順

2 つめのデバイス側では、有効化（`nr=0x0d`）の**前に `nr=0x32 {handle, 1}`**
を全ハンドルに対して呼んでいる。こちらは一度も呼んでいない。

また `val_demux1`（別の chardev）で確保し、その後の `nr=0x0c` / `nr=0x0d` は
`val_demux` に出している。**デバイスノードも使い分けている。**

### 22.5 次

1. **`nr=0x02` で自分のデバイスを開く**（`{1, ポインタ}`、返りはハンドル）。
2. **`nr=0x05 {自分のデバイス, 3}`** で入力源を設定。
3. `val_demux1` で `nr=0x0b {7, in, 自分のデバイス, out}` としてフィルタを確保。
4. `nr=0x2b` で自前のリングを与え、`nr=0x16` で結びつけ、`nr=0x0c` で PID、
   **`nr=0x32 {h,1}`**、`nr=0x0d {h,1}`。
5. `nr=0x2f` で読む。

これを**1 つのスクリプトで一気に**流す（§21.4 の道具）。ただしハンドルは実行時に
決まるので、**確保の応答を制御ページから読んで次の行を組み立てる**必要があり、
スクリプト 1 本では完結しない。**道具側に「前の応答をそのまま次の引数に使う」
仕組みが要る**（例: `$h` のような置き換え）。

## 23. 自分のデバイスを作れた（残るは `nr=0x30`）（2026-09-22）

§22.5 の段取りを実施した。**道具に置き換えの仕組みを足し、ベンダの列を
最後まで写した。デバイスの作成までは通り、最後の 1 手で止まっている。**

### 23.1 道具に「前の応答を次の引数に使う」を足した

確保の応答（ハンドル）は**共有メモリに書かれる**のであって ioctl のバッファには
入らない。∴ 次の行を先に書いておけない。`tools/yb1-ioctl` のスクリプト形式に:

```
load $N <phys>     物理メモリの 32bit 語を $N に読む（/dev/mem を mmap）
$N                 引数のどこにでも置ける
```

を足した。これで**確保 → ハンドル取得 → それを使う**までを 1 つの fd で流せる。

### 23.2 ★ `nr=0x02` は新しいデバイス実体を作る

```
0x4028c302 {1, 0xa9000000}   → 制御ページ +0x00 に 0x01010001
```

ベンダは `0x00010000` と `0x02010002` を持つ。**我々が開くと `0x01010001`**。
∴ 番号は `(n << 24) | 0x010000 | n` の形で、**`nr=0x02` は開くたびに別の実体**を
返す（`ret` は `-6` だがハンドルは書かれる）。

**そして我々のデバイスの下で確保したフィルタは `0x01100035`**
＝ `(デバイス 0x01 << 24) | (種別 0x10 << 16) | 番号`。
**§22.2 の「ハンドルはデバイスを含む」が、作る側から裏づけられた。**

### 23.3 通ったところ

| 呼び出し | 引数 | ret |
|---|---|---|
| `nr=0x02` 開く | `{1, ptr}` | -6（ただしハンドルは入る） |
| **`nr=0x48`** | `{dev, 1}` | **0** ← ベンダが開いた直後に呼ぶ。新発見 |
| `nr=0x05` 入力源 | `{dev, 3}` | **0** |
| `nr=0x0b` 確保 | `{7, in, dev, out}` | **0** → `0x01100035` |
| `nr=0x0c` PID | `{h, 0x0100}` | **0** |
| `nr=0x2b` バッファ | `{buf, ring, size, desc}` | **0** |

### 23.4 ★ 止まっているところ＝`nr=0x30`

```
nr=0x30  {dev, 1, 2, 5, 0xa90000b8}   ret = -8
nr=0x32  {h, 1}                       ret = -3   （0x30 が通らない結果）
nr=0x0d  {h, 1}                       ret = -8   （同上）
```

ベンダは 2 つめのデバイスで `nr=0x30 {0x02010002, 1, 2, 5, 0xa90000e8}` を
**フィルタの PID 設定の後・有効化の前**に呼んでいる。引数の `1, 2, 5` が何かは
未解明。**`-8`（`ENOEXEC`）＝呼び出し自体は受け付けたが実行できない。**

なお**この経路では `nr=0x16`（結びつけ）も `nr=0x2b` も使われていない**。
∴ 2 つめのデバイス側は、バッファを `nr=0x30` の第 5 引数（ディスクリプタの
置き場）で受け取る作りと見られる。§19.1 で見た `nr=0x16` の経路は
**1 つめのデバイス（テーブル系）専用**。

### 23.5 残る 1 問

**`nr=0x30 {dev, 1, 2, 5, desc}` の `1, 2, 5` は何か。** ここが通れば
`nr=0x32` → `nr=0x0d` と繋がり、`nr=0x30` に渡したディスクリプタへ TS が
落ちるはず。

採り方: ベンダは起動時に 1 回しか呼ばない。**選局やチャンネル切り替えのたびに
呼び直すか**をトレースで見れば、引数が何に対応して変わるか（サービス数・
ストリーム数・PID 本数など）が分かる。`1, 2, 5` という並びは
**「何かの数」** に見える（このとき PID フィルタは 14 本張られていた）。

### 23.6 `nr=0x30` は起動時 1 回だけで、ハンドルを返す

§23.5 の採り方（選局のたびに呼び直すか）を実施した。**呼び直していない。**
選局を 3 回繰り返したトレース 6810 行で:

| 呼び出し | 回数 |
|---|---|
| `nr=0x0c`（PID 設定） | 79 |
| `nr=0x0d`（有効化） | 73 |
| **`nr=0x30` / `nr=0x32` / `nr=0x48`** | **0** |

∴ **`nr=0x30` は起動時のデバイス設定で 1 回だけ**。引数 `1, 2, 5` はチャンネルに
よらない固定の設定値で、**選局トレースからは意味を採れない。**

なお選局時の `nr=0x0c` は `0x0010xxxx` と `0x0210xxxx` の**両方**に出る
＝ベンダは既にあるフィルタの PID を差し替えているだけで、作り直していない。

**代わりに分かったこと**: ベンダの `nr=0x30` の第 5 引数は `0xa90000e8`
＝制御ページ **+0xe8**。§10.5 の実測で **+0xe8 の値は `0x00002002`** だった。
そして §15.3 で `nr=0x33` / `nr=0x34` が第 1 引数に取っていたのが、まさに
**`0x00002002`**。

∴ **`nr=0x30` は確保系の呼び出しで、第 5 引数の先にハンドルを書く**
（`nr=0x0b` と同じ作法）。返るハンドルは種別 `0x2002` の第 5 の空間で、
`nr=0x33` / `nr=0x34` がそれを使う。**`{dev, 1, 2, 5}` は確保の諸元。**

### 23.7 `-8` の読み

`nr=0x30` が `-8`（`ENOEXEC`）を返すのは、**資源が 1 つしかなく既にベンダが
確保している**ためと見るのが自然（`nr=0x30` は起動時 1 回きり、ベンダの
デバイス 2 が既に `0x00002002` を持っている）。

∴ 次に確かめること:

| # | 内容 |
|---|---|
| I1 | **ベンダを止めた状態で `nr=0x30` を呼ぶ**と通るか。通れば「資源の取り合い」で確定。§18.5 の汎用フックで `setup.sh` の最終行（`blur`）だけ飛ばせば、モジュールは載ったまま `blur` 抜きの状態を作れる（§22 の知見で、必要な初期化は自分で流せる見込みが立った） |
| I2 | `{1, 2, 5}` の意味。`nr=0x33`/`0x34` が `0x00002002` に何をするかから逆に読む |

## 24. ★ ベンダアプリ抜きで、デマルチプレクサの設定一式が通った（2026-09-22）

§23.7 の I1（`nr=0x30` の `-8` は資源の取り合いか）を検証した。**そのとおりだった。**
さらに**足りなかった最初の 1 手**も見つかり、**ベンダアプリを 1 つも動かさずに
設定が最後まで通った。**

### 24.1 blur 抜きのリグ

`setup.sh` は app squashfs 上の**通常ファイル**なので bind mount で差し替えられる
（`/sbin/insmod` は busybox へのシンボリックリンクで不可だった・§13.1）。
`tools/vendor-rootfs/mkshell.sh` の汎用フック（§18.5）に `openird-noblur` を足した:

1. 裏 CPU ファームを `dd` で置く
2. `csd.ko` を `CSD_DEBUG` つきで load
3. **`blur` が load していた 8 本**を順に load
   （`i2c-algo-emma` → `i2c-emma` → `val_merge` → `ei2c_ofdm_drv` →
   `io_controller_drv` → `fe_common_drv` → `fe_main_drv` → `mem2`）
4. `setup.sh` の最終行（`./bin/blur`）をコメントアウトした写しを bind mount

実機で全部成功した（`ps | grep -c blur` = **0**）。

**⚠ 運用上の注意**: **この状態では telnet に入れない。** IP を取る `udhcpc` は
`blur` 側が起動するため。**シリアル（`host/openird-serial`）が唯一の口**になる。

### 24.2 ★ 足りなかったのは `nr=0x01`（Initialise）

blur 抜きで §23 の列をそのまま流すと、**全部 `-2`（`ENOENT`）** になり、
制御ページも `0xffffffff` のままだった＝AV サブシステムが初期化されていない。

起動トレースの**いちばん最初の行**が `nr=0x01 {0,0,0,0}` で、これを飛ばしていた。
**先頭に足したら全部通った。**

### 24.3 ★ 通った列（ベンダアプリ無し・全部 `ret=0`）

```
nr=0x01  {0}                                  Initialise        ret=0
nr=0x02  {1, 0xa9000000}                      デバイスを開く     ret=0  → 0x00010000
nr=0x48  {dev, 1}                                                ret=0
nr=0x05  {dev, 3}                             入力源             ret=0
nr=0x0b  {7, in, dev, out}                    PID フィルタ確保   ret=0  → 0x00100023
nr=0x0c  {h, 0x0100}                          PID（映像）        ret=0
nr=0x30  {dev, 1, 2, 5, 0xa90000b8}                              ret=0  ★
nr=0x32  {h, 1}                                                  ret=0
nr=0x0d  {h, 1}                               有効化             ret=0
```

∴ **§23.4 の `-8` は資源の取り合いだった**（`nr=0x30` は起動時 1 回きりの確保で、
ベンダが既に取っていた）。**I1 は確定。**

デバイスのハンドルは **`0x00010000`**（ベンダが動いていないので我々が 1 番目）。

### 24.4 ★ `nr=0x30` は確かに確保系だった

`nr=0x30` の第 5 引数に渡した `0xa90000b8`（制御ページ +0xb8）を読むと:

```
+0xb8: 00002000
```

**種別 `0x2000` のハンドルが書かれていた。** ベンダのは `0x00002002`、我々は
1 番目なので `0x00002000`。**§23.6 の読み（`nr=0x30` は確保系で第 5 引数の先に
ハンドルを書く）が実測で確定した。**

### 24.5 いまの位置

| | |
|---|---|
| ベンダアプリ無しで AV を初期化 | **成功** |
| デバイスを開く・入力源の選択 | **成功** |
| PID フィルタの確保・PID 設定・有効化 | **成功** |
| `nr=0x30` の確保 | **成功** |
| 自前リングの登録 | 成功（§17.3・この列には未挿入） |
| **TS が落ちる** | **未達。ただし理由は明確**＝復調器が動いていない |

**blur を外したので、復調器（MXL683）を誰も初期化していない**＝TS 入力そのものが
無い。∴ 次は **§4.3 の I2C 手順で MXL683 を自分で選局させる**こと。
周波数レジスタ（`reg 0x3029 = round(f_MHz × 64)`）は §4.3 で確定しており、
`tools/yb1-i2c` もある。

**∴ 段階 C の 2 本の柱が、それぞれ単独では実機で動くところまで来た**:
**フロントエンド（I2C・§4.3）** と **デマルチプレクサ（この節）**。
残るのは**両方を同時に動かして TS を流すこと**。

## 25. ベンダアプリと同居する道は閉じた（2026-09-22）

§24 で「ベンダアプリ抜きなら設定が全部通る」「ただし復調器が動いていないので
TS が無い」ところまで来た。**では同居できないか**を 2 通り試し、**どちらも否定**した。

### 25.1 バッファ id はデバイス付きではない

§21.6 でトレースに `0x01000123` という形が出たので、**バッファ id も
`(デバイス << 24) | …` ではないか**と考えた。`nr=0x15`（副作用の無い問い合わせ）
で確かめた:

| id | `nr=0x15` |
|---|---|
| `0x00000113` | **0** |
| `0x01000113` / `0x01000100` / `0x02000100` | **-3** |

∴ **バッファ id は `0x000001xx` の平らな空間**で、デバイスごとには分かれていない。
**仮説は否定。** ベンダのデバイスが `0x0100`〜`0x0113` を押さえている以上、
**後から開いたデバイスに割り当てられるバッファは無い。**

### 25.2 ★ `blur` を落としても資源は返ってこない

「選局だけベンダにやらせ、そのあと `blur` を落として資源を奪う」を試した。
手順は実機側の 1 本のスクリプトにし、ログは `/app/setting`（再起動を跨ぐ）へ。

結果:

```
ps | grep -c blur   75 → 0          （kill は効く）
nr=0x01  ret=-1                      （Initialise は通らない）
nr=0x02  ret=-6  → 0x01010001        （★ まだ「2 番目のデバイス」になる）
nr=0x30  ret=-8                      （★ 資源は握られたまま）
nr=0x16  ret=-3
```

∴ **`blur` の死は `val_merge` の状態を解放しない。** プロセスが消えてもデバイスは
登録されたままで、`nr=0x30` の資源も空かない。そのうえ**約 20 秒後に実機が
再起動する**（§8.2 の再現）。

**∴ ベンダアプリと同居して TS を取る道は閉じた。**

### 25.3 残る道は 1 本

| 条件 | デマルチプレクサ | TS 入力 |
|---|---|---|
| ベンダアプリあり | **資源が無い**（§25.1/§25.2） | ある |
| **ベンダアプリ無し** | **全部通る**（§24.3） | **無い**（復調器が未初期化） |

∴ **ベンダアプリ抜きで起動し、MXL683 を自分で立ち上げる**しかない。
これは §9.1 の **A1（チップ内 RAM へのファーム転送）** そのもので、
「これが無いとチップが動かない・必須」と書いた項目。**そこが次の関門になった。**

材料はある:

- I2C の手順とレジスタの作法は §4.3 で確定（16bit reg / 16bit value、
  周波数は `reg 0x3029 = round(f_MHz × 64)`）。
- `tools/yb1-i2c` で読み書きできる。
- 起動時のバストレースは**すでに採れる**（§4.2 の GPL ドライバのトレース）。
  §9.1 に「48B ブロックが連番で流れるところまでは観測済み」とある。
- ファーム本体は**所有者の機器上のもの**を実行時に使う（§1-5 と整合）。
  `blur` の中にあるので、**どこから来てどうやって流れるか**を観察で起こす。

∴ 次の作業は **「起動時の I2C バストレースを採り、MXL683 へのファーム転送の
構造（ブロックの区切り・アドレス・開始/完了の合図）を仕様化する」**。

## 26. MXL683 のファームは区画上のファイルではない（2026-09-22）

§25.3 で次の関門になった A1（チップ内 RAM へのファーム転送）について、
**ファームの所在だけ**先に確かめた。

### 26.1 どの区画にも無い

| 区画 | 中身 | MXL683 のファーム |
|---|---|---|
| mtd3 `FileSystem(SQUASHFS)` | rootfs | **無し**（`/lib/firmware` 相当も無い） |
| mtd4 `App_binary(SQUASHFS)` | `/app/bin` | **無し**。`.bin` は `mb86h57_h58_aac_{trc,idle}.bin`（**トランスコーダ**のファーム）と `spirom_writer_usb_pixela_102Dh.bin` だけ |
| mtd5 `EMMA_FW(SQUASHFS)` | `emma3sep_driver_std_op0.mem` ＋ 2 本のスクリプト | **無し**（これは**裏 CPU** のファーム・§22） |
| mtd6 `Initial setting` | 先頭が `PIX`、`POE-BR100-YB1`、`NOT USE A PANEL` 等 | **無し**（板の素性と設定・128KB） |
| mtd7/8/9 | ログ / `/app/setting` / MassStorage（yaffs2） | — |

### 26.2 ★ `blur` の中にある

`blur`（10.2MB）の文字列に:

```
Ctrl_DownloadFirmwareInMbin: Invalid file header ID (%c)
Ctrl_DownloadFirmwareInMbin: Invalid segment header ID (%c)
Err : MxLWare68x_API_CfgDevFirmwareDownload %d
Download firmware success.
```

∴ **ファームは MaxLinear の `MBIN` 形式**（ファイルヘッダ ID ＋ セグメント
ヘッダ ID を持つ、MxLWare 標準のコンテナ）で、**`blur` に埋め込まれている**。
`MxLWare68x_API_CfgDevFirmwareDownload` がそれを I2C で流す。

**∴ 「区画上のファイルとして置かれている」という見立ては外れ。**

### 26.3 段階 C への含意

- ファームは**所有者の機器上の `blur` の中**にある。§1-5 のとおり
  **配布物には含めない**。所有者が自機の `blur` から実行時に取り出す形になる。
- `MBIN` は**構造の決まったコンテナ**なので、**`blur` を走査してヘッダを探す**
  だけで取り出せる見込みがある。これは**データの観察**であって逆アセンブルでは
  ない（`.clean-room` の許す範囲）。
- ∴ A1 は「①`blur` から MBIN を切り出す ②`MBIN` の構造を仕様化する
  ③I2C でどう流すかを起動時バストレースから起こす」の 3 つに分かれる。
  **①と②は §27 で片付いた。③ は §4.2 のトレースで既に採れる状態。**

## 27. ★ MBIN を切り出し、構造を仕様化した（2026-09-22）

§26 で所在だけ分かっていた MXL683 のファームについて、**A1 の ① と ② を片付けた。**

**仕様は独立した文書に分けた＝[mbin-firmware-container.md](mbin-firmware-container.md)**
（MBIN は MaxLinear の形式で本機固有ではないため、既存の形式仕様と同じ扱いにした）。
ここには要点と、本機での所在だけ残す。

### 27.1 切り出せた

`blur`（10,247,076 バイト・sha256 `eca5748c…`）の **`.rodata` に生で 1 個だけ**あり、
ファイル内オフセット `0x7e1950`・全長 **13,224 バイト**。取り出しは
`scripts/extract-mbin.py`（依存なしの Python 3・**オフセットは決め打ちせず走査**）。

直前の `.rodata` が `MxLWare68x_API_CfgDevFirmwareDownload` /
`MxLWare68x_API_CfgDevXtal` / `downloadFirmware` などの API 名の並びで、
位置としても整合する。

### 27.2 構造は自己検証できる形だった

| | |
|---|---|
| ファイルヘッダ | 16 バイト・全部 big-endian（`'M'` `'1'` / header_len / num_segments / entry_address / image_size24 / checksum / reserved） |
| セグメント | ヘッダ 8 バイト（`'S'` / len24 / address32）＋ データ。**次は 4 バイト境界から** |
| チェックサム | 本体 `image_size24` バイトの 8bit 総和。**実物で一致した** |

本機の中身は **3 セグメント**（`0x3ffe0000` 4784B / `0x3ffe2900` 601B /
`0x40000010` 7796B）、`entry_address` は `0x40010000` で**どのセグメントの範囲にも
入らない**（用途未解明）。

4 バイト整列の規則は**実測で裏が取れた**: `24 + 4784 + 604 + 7796 = 13208` が
`image_size24` と一致し、パディング無し（13205）では合わない。

構造の呼び名はメインライン Linux の公開 GPL 定義
（`drivers/media/dvb-frontends/mxl5xx_defs.h` の `MBIN_FILE_HEADER_T` ほか）に拠った。
**`blur` の逆アセンブルはしていない**（バイト列の走査と突き合わせだけ）。

### 27.3 残るのは ③＝I2C 転送手順

**容器は割れたが、バス上にどう出るかはまだ観察していない。**
§4.3 で観測済みの「起動時に `0x60` 宛てへ連続する 48 バイト書き込み」が
これだとみて整合するが、**突き合わせていない**。

いちばん効く検査は **seg[0] の先頭バイト列 `70 00 00 10 3f fe 10 64` を
起動時バストレースの中に探すこと**。見つからなければ **32bit ワードを入れ替えた
`10 00 00 70 64 10 fe 3f`** も探す（メインラインの転送は `convert_endian()` で
32bit ワードを入れ替える＝ホストが LE 前提。**本機のホストは MIPS BE** なので、
どちらで出るかは実測でしか決まらない）。

確かめる項目は mbin-firmware-container.md §7 の A1-a〜A1-d。
**この観察は blur を動かした状態で行う**（blur 抜きでは誰もファームを流さない）＝
`openird-noblur` は**置かない**まま、`openird-i2ctrace` を置いて再起動する。

## 28. ★ A1 が片付いた＝MXL683 のファーム転送を全部割った（2026-09-22）

§27 で容器を切り出したのに続き、**起動時の I2C バストレースを採って突き合わせた。
転送の形は完全に割れた。** 詳細は
**[mbin-firmware-container.md](mbin-firmware-container.md) §7**。ここには要点だけ。

### 28.1 採り方

§4.2 の経路（GPL の `i2c-algo-emma` を建て直して先に load）をそのまま使ったが、
**`EMMA_TRACE_MAX` を 32 → 64 に上げた**。バス上の転送は 48 バイト単位なので、
32 のままだと毎回切れて突き合わせができない。

リグは `/app/setting` に置くだけで済ませた（**焼き直し不要**）:
建て直した `.ko` を `/app/setting/i2c-algo-emma.ko` に置き、`openird-pre.sh` から
`insmod … trace=1` し、**16KB しかないログ環を 20ms ごとに `dmesg -c` で tmpfs へ
吸い出す**ループを背後で回す。`blur` は動かしたまま（でないと誰もファームを流さない）。
1 回の起動で 6,378 メッセージが採れた。

### 28.2 ★ 何が分かったか

| 問い | 答え |
|---|---|
| **A1-a** 48 バイト書き込みはファームか | **そのとおり。** `0x02` コマンド 301 本のペイロードを連結すると **13,181 バイト**になり、§27 の 3 セグメントのデータと**バイト単位で完全一致**した |
| バイト順 | **格納されているまま。32bit ワードの入れ替えは無い**（ホストが MIPS BE だから） |
| **A1-b** 48 バイトの内訳 | **4 バイトヘッダ ＋ 最大 44 バイトのデータ**。ヘッダは `{0xE0\|連番, opcode, ペイロードの 8bit 総和, ペイロード長}`。**305 本すべてで総和と長さが一致** |
| opcode | `0x01`=転送先アドレス（セグメントごとに 1 回）／`0x02`=データ（301 本）／`0x03`=開始 |
| **A1-c** 前後に要るもの | 前＝`0x3002/0x3003 <- 0x0000/0x2008` で終わる前置き列（リセットは I2C に出ない＝GPIO）。後＝`0x03` の直後に**レジスタ `0xa8cc` が `0xabcd` を返す**のが起動の合図 |
| **A1-d** `entry_address` は要るか | **要る。`0x03` コマンドの引数がまさに `0x40010000`** |

付随して分かったこと:

- **4 バイト整列のパディングは線上に流れない**（13,181 であって 13,184 ではない）。
  整列は容器の都合。
- 連番は **0〜14 の 15 進**（`0xF` は使わない）で、`0x01`/`0x03` のたびに 0 に戻る。
  **§4.3 が「`e4` から始まる」と書いていたのは、ログ環が溢れて途中から採れていたため**
  ＝`0xe4` は連番 4 であって開始点ではない。§4.3 のその記述は本節で訂正。
- **`0xfffd` は 4 バイト読みの選択レジスタ**（`0xfffb` の 2 バイト読みと対）。
  全トレース中 2 回、開始コマンドの前後にだけ現れる。
- 低位レジスタ（`0x0001`〜`0x006d` 台）を触る前に必ず `0x0000` へ 0 か 1 を書く＝
  **バンク選択とみられる**（未確定）。

### 28.3 段階 C の位置

**フロントエンド（§9.1 の A1）は、観察としては終わった。** 立ち上げ手順は
mbin-firmware-container.md §7.5 に 5 ステップで書いてある。∴ いま段階 C に
**残っているのは観察ではなく実装**で、2 本の柱は両方とも手順が揃っている:

| 柱 | 状態 |
|---|---|
| フロントエンド（MXL683） | **手順が全部揃った**（リセット→前置き→ファーム転送→開始→選局列） |
| デマルチプレクサ | **設定一式がベンダアプリ抜きで通る**（§24.3）。自前リングの登録も成功（§17.3） |

次は**その 2 つを同じ起動の中で順に実行して TS を流すこと**。
これは `openird-noblur` のリグ（§24.1）の上でできる。

## 29. MXL683 のコールドスタート列（2026-09-22・実測）

§28 のトレースから、**電源投入後まっさらな状態から選局までの手順**をそのまま起こした。
段階 C のフロントエンド実装はこの節だけを見て書く。

**レジスタの形**（§4.3）: 16bit アドレス・16bit 値。書きは 4 バイト
`[reg_hi reg_lo val_hi val_lo]`、読みは `0xfffb` に対象番号を書いてから 2 バイト読む。
**`0xfffd` に書けば 4 バイト読み**（§28.2）。

> **観察された構造の見立て（未確定・実装は依存しないこと）**
> - `0x3002/0x3003` のように**偶数=上位・奇数=下位の 16bit 対**で 32bit 値を
>   なすように見える（`0x3028/0x3029` が周波数の 32bit 対なのは §4.3 で確認済み）。
> - ファーム転送**前**は、低位レジスタ（`0x0001`〜`0x01xx`）を触るたびに直前に
>   `0x0000` へ 0 か 1 を書いている。**転送後は書いていない**。バンク選択とも
>   取れるが確証は無い。**列をそのまま再生すれば足りる**ので、意味は詰めていない。

### 29.1 A. 転送前（チップに最初に触ってから転送開始まで）

リセットは I2C に出ない（GPIO の `TU_RST#`）。バスに最初に出るのは読みである。

```
R 0x300c -> 0x0000        R 0x300d -> 0x0000
W 0x300c <- 0x0000        W 0x300d <- 0x0003
R 0x3002 -> 0x0000        R 0x3003 -> 0x0008
W 0x3002 <- 0x0100        W 0x3003 <- 0x0008
W 0x3002 <- 0x0000        W 0x3003 <- 0x0008
R 0x3001 -> 0x1012                                  ← 素性らしき固定値
W 0x0000 <- 0x0000   W 0x000e <- 0x0014
W 0x0000 <- 0x0000   R 0x0001 -> 0x008a
W 0x0000 <- 0x0000   W 0x0001 <- 0x0005
W 0x0000 <- 0x0000   R 0x0002 -> 0x0000
W 0x0000 <- 0x0000   W 0x0002 <- 0x0000
W 0x0000 <- 0x0000   W 0x006d <- 0x008a
W 0x0000 <- 0x0000   W 0x0003 <- 0x0000
W 0x0000 <- 0x0000   W 0x0003 <- 0x0001
W 0x0000 <- 0x0000   W 0x0003 <- 0x0000
W 0x0000 <- 0x0000   W 0x000b <- 0x0001
W 0x0000 <- 0x0000   W 0x0012 <- 0x0001
W 0x0000 <- 0x0001   R 0x00d5 -> 0x0005                ┐ 3 回読む。値は 5,7,0x0a と
W 0x0000 <- 0x0001   R 0x00d5 -> 0x0007                │ 増える＝自由走行の量に見える。
W 0x0000 <- 0x0001   R 0x00d5 -> 0x000a                ┘ 待ちとみて値には依存しない
R 0x3002 -> 0x0000        R 0x3003 -> 0x0008
W 0x3002 <- 0x0000        W 0x3003 <- 0x2008           ← ★ 直後に転送が始まる
R 0x302a -> 0x0000        R 0x302b -> 0x0000
R 0x300a -> 0x0000        R 0x300b -> 0x1000
```

### 29.2 B. 転送

[mbin-firmware-container.md](mbin-firmware-container.md) §7 のとおり。要約:

```
for each segment:
    DL 0x01 <segment address, 4 bytes BE>
    DL 0x02 <44 bytes> ... （端数は最後だけ短い）
R32 0x0003 -> 0xff000000
DL 0x03 <entry_address, 4 bytes BE>
```

### 29.3 C. 転送後（起動確認から選局の直前まで）

```
R32 0x0003 -> 0xff000000
R   0xa8cc -> 0xabcd                                  ← ★ ファームが起きた合図
R 0x300a -> 0x0270   R 0x300b -> 0x0000
W 0x300a <- 0x0270   W 0x300b <- 0x0000
R 0x3002 -> 0x0000
W 0x3002 <- 0x0000   W 0x3003 <- 0x0f89
R 0x3002 -> 0x0000   R 0x3003 -> 0x0f89
W 0x3002 <- 0x0000   W 0x3003 <- 0x0f89
W 0x0012 <- 0x0000
R 0x300a -> 0x0270   R 0x300b -> 0x0000
W 0x300a <- 0x0270   W 0x300b <- 0x0000
R 0x3002 -> 0x0000   R 0x3003 -> 0x0f89
W 0x3002 <- 0x0000   W 0x3003 <- 0x0f89
W 0x0012 <- 0x0000
R 0x302a -> 0x0000   R 0x302b -> 0x0003
R 0x300a -> 0x0270   R 0x300b -> 0x0000
W 0xa85c <- 0x01da
W 0xe034 <- 0x0000
R 0xa85c -> 0xffff
W 0xa85c <- 0x0000
R 0x302a / 0x302b / 0x300a / 0x300b（同じ値）
W 0x000b <- 0x0001
R 0x0196 -> 0x0000   W 0x0196 <- 0x0000
R 0x1122 -> 0x0000   R 0x11ad -> 0x0000
W 0x11ad <- 0x0000   W 0x1122 <- 0x0220
R 0x300c -> 0x0000   R 0x300d -> 0x0003
W 0x300c <- 0x0000   W 0x300d <- 0x0003
R 0xa80a -> 0x0000
R 0x301e -> 0x0000   R 0x301f -> 0x0000
W 0x301e <- 0x0000   W 0x301f <- 0x0000
R 0x3002 / 0x3003    W 0x3002 <- 0x0000  W 0x3003 <- 0x0f89
W 0x0012 <- 0x0000
```

**低位レジスタの前に `0x0000` を書かなくなっている**（§29 冒頭の見立て）。

### 29.4 D. 選局

§4.3 の列そのまま。本トレース（起動直後の 1 回目＝473 MHz）で確認した並びは:

```
W 0x3028 <- 0x0000   W 0x3029 <- 0x0000        ← 周波数を 0 に落とす
W 0x007c <- 0x001f   W 0x0131 <- 0x0081
W 0x0049 <- 0x001c   W 0x00db <- 0x0017
R 0x00ea -> 0x0020   W 0x00ea <- 0x0000   W 0x00eb <- 0x00d8
R 0x00b6 -> 0x0040   W 0x00b6 <- 0x0040
R 0x015f -> 0x0037   W 0x015f <- 0x0037
R 0x0160 -> 0x0037   W 0x0160 <- 0x0037
W 0x3029 <- 0x7649                                ← ★ 周波数（473 MHz）
R 0x3028 -> 0x0000   W 0x3028 <- 0x0000
W 0xe033 <- 0x0000
R 0x101c -> 0x0000   W 0x101c <- 0x0004
R 0x101b -> 0x0000   W 0x101b <- 0x0000   W 0x101b <- 0x0001   ← 起動トリガ
R 0xa80a（繰り返し）                              ← 状態ポーリング
```

周波数の値は §4.3 のとおり `0x3029 = round(f_center_MHz × 64)`、地上波は
`f_center = X + 1/7`。整数だけで書くなら

```
reg = (448 * X + 64) / 7        （切り捨て。X = 整数 MHz の呼び値）
```

で観測 7 点すべてに一致する（473→0x7649, 479→0x77c9, 503→0x7dc9, 509→0x7f49,
521→0x8249, 545→0x8849, 557→0x8b49）。

### 29.5 実装への注意

- **列をそのまま再生する。** 読みの戻り値に条件分岐しない（`0xa8cc == 0xabcd` の
  確認と `0xa80a` のポーリングだけが例外）。意味の分かっていない読みも、
  副作用があるかもしれないので**省かない**。
- 転送は**ベンダアプリ抜きの起動**（§24.1 の `openird-noblur`）で行う。
  ベンダアプリが動いていると資源を握られる（§25）。

## 30. ★ 自前のコードで復調器を立ち上げ、受信させた（2026-09-22・実機）

**§29 の仕様だけを見て書いたツールで、ベンダアプリを 1 つも動かさずに MXL683 を
コールドスタートさせ、選局して信号を測るところまで通った。** OpenIRD のコードが
電波を受けた最初の記録。

道具は `tools/yb1-mxl`（静的・libc 非依存・9.6KB、`scripts/build-yb1-mxl.sh`）。
**ファームは `/app/bin/blur` を走査して実行時に取り出す**＝配布物には入らない（§1-5）。

### 30.1 通った列

`openird-noblur` のリグ（§24.1）で起動し、telnet から:

```
$ yb1-mxl 1 probe
  0x3001 = 0x1012                     ← ベンダ抜きでもチップは応答する

$ yb1-mxl 1 load /app/bin/blur
  container at offset 0x007e1950, total 13224 bytes
  entry 0x40010000, 3 segments
A: cold-start sequence
    waited 30 ms for the chip at operation 29 (register 0x0000)
  sequence A: 47 operations ok
B: firmware transfer
  transferred 13181 bytes in 301 data blocks + 3 address commands
  firmware alive: 0xa8cc = 0xabcd (after 50 ms)      ← ★
C: post-transfer sequence
  sequence C: 54 operations ok

$ yb1-mxl 1 tune 503
  sequence D: 24 operations ok
```

### 30.2 ★ 受信できていることの確認

信号のある 503 MHz と、ベンダでもロックしなかった 479 MHz（§4.3）を比べた:

| | 479 MHz（信号なし） | 503 MHz（信号あり） |
|---|---|---|
| `0x112d` | **`0x0000` のまま** | `0x0213`〜`0x027a` で**揺れる**＝実測値 |
| `0x0033` | 0x00f9〜0x00fb（約 250） | 0x00bb〜0x00be（約 190） |

3 チャンネルの `0x112d` は **568 / 715 / 960**（503 / 521 / 545 MHz）で、
§4.3 でベンダが出していた範囲（567〜1094・C/N 28〜33 dB）にそのまま乗る。
∴ **復調器はロックし、チャンネルごとの品質を測っている。**

### 30.3 ★ 仕様に足りていなかったこと

- **`0x112d == 0` が「ロックしていない」の判定になる。** 非ゼロならロック、
  値が小さいほど C/N が良い（§4.3）。**chardev の lock 判定はこれで足りる。**
- **`0x0003` へ 1 を書いてから 0 に戻すと、チップは約 30 ms バスから消える**
  （§29.1 の操作 29 で NAK）。ベンダのトレースに失敗が 1 件も無かったのは、
  ベンダ側のコードがそこを跨ぐのに十分遅かったためで、**列を詰めて流すと必ず踏む**。
  ∴ **実装はレジスタアクセスを再試行すること**（`yb1-mxl` は 10 ms 間隔で最大 4 秒）。
- **`0x3028` は書いた値が読み戻らない**（0 を書いても `0x00c0` が返る）。§4.3 は
  「`0x3029` と対になる 32bit の上位ワード」と読んだが、**単なる保持レジスタでは
  ない**。0 を書く動作自体は列のとおりで足りるので、実装には影響しない。
- `0xa80a` は選局直後の 1 回目だけ `0x0014` を返し、以後 `0x0000`。
  **読むと消える**たぐいに見える（未確定）。

### 30.4 残り

**フロントエンドは終わり。** 段階 C の 2 本の柱のうち 1 本が、仕様 → 実装 → 実機で
閉じた。残りは**デマルチプレクサ側（§24.3 の列＋自前リングの登録）を同じ起動の中で
動かし、TS を取り出すこと**。

## 31. デマルチプレクサ側＝設定は全部通るが、バッファだけ作れない（2026-09-22）

§30 でフロントエンドが動いたので、**同じ起動の中で**デマルチプレクサ側
（§24.3 の列）も流し、TS を取り出そうとした。**設定は全部通る。落ちているのは
バッファオブジェクトを 1 つ作ること、その 1 点だけ。**

### 31.1 通ったもの（ベンダアプリ無し・冷起動から）

```
nr=0x01  {0}                                ret=0
nr=0x02  {1, 0xa9000000}                    ret=0  → dev  0x00010000
nr=0x48  {dev, 1}                           ret=0
nr=0x05  {dev, 3}                           ret=0
nr=0x65  {0,1} {1,1}                        ret=0
nr=0x64  {0,1} {1,1}                        ret=0
nr=0x0b  {7, in, dev, out}                  ret=0  → filt 0x00100023
nr=0x0c  {filt, 0x0000}                     ret=0   （PID 0 = PAT）
nr=0x30  {dev, 1, 2, 5, 0xa90000b8}         ret=0  → 0x00002000
nr=0x32  {filt, 1}                          ret=0
nr=0x0d  {filt, 1}                          ret=0
```

§24.3 の再現に加えて **`nr=0x64` / `nr=0x65`（§22.1 の起動列にあって飛ばしていた
2 本）も `ret=0`** で通ることを確かめた。

### 31.2 ★ 落ちているところ＝バッファオブジェクトが存在しない

```
nr=0x2b {0x0100|0x0101|0x0110|0x0113, ring, size, desc}   すべて ret=-3
nr=0x2e {0x0100|0x0113}                                   ret=-3
nr=0x16 {filt, 0x0100}                                    ret=-3
nr=0x2f {0x0100|0x0113}                                   ret=-3
nr=0x2b {0x00002000, ring, size, desc}                    ret=-3
```

∴ 2 つのことが分かった:

- **`0x000001xx` のバッファ id は、ベンダアプリが作らない限り存在しない。**
  §20.1 と §25.1 は「確保されるものではない平らな空間」と読んだが、**それは
  ベンダアプリが動いている状態での観察だった**。冷起動直後には 1 つも無い。
  **§20.1／§25.1 のその部分は本節で訂正する**（§25 の結論＝同居はできない、
  は変わらない。理由がより鋭くなっただけ）。
- **`nr=0x30` が返す種別 `0x2000` のハンドルは、バッファ系の呼び出しが受け取る
  ものではない**（`-3`）。`nr=0x30` は別の資源の確保。

`nr=0x64` / `nr=0x65` を足してもバッファは生えなかった。∴ **作る呼び出しは
まだ同定できていない。**

### 31.3 実験のコスト（運用メモ）

**AV サブシステムの状態はプロセスの終了では消えない。** 2 回目に同じ列を流すと
`nr=0x01` が `-1`、`nr=0x02` が 2 つめのデバイス、`nr=0x30` が `-8`（資源は
取られたまま）になる。§25.2 が `blur` について見たのと同じことが**自分の
プロセスについても起きる**。∴ **1 回の試行ごとに電源を入れ直すこと。**

### 31.4 次

**バッファオブジェクトを作る呼び出しを同定する**こと。手がかりは、ベンダの
起動〜配信 1 回ぶんの ioctl トレース（§11.8）の中で、**`0x113` あるいは
`0x0100` が最初に現れる行**。そこが作る側の呼び出しである見込みが高い。
これは実機不要で、記録済みのトレースを読み直せば済む。

∴ 段階 C の位置は:

| 柱 | 状態 |
|---|---|
| フロントエンド（MXL683） | **完了**（§30。冷起動 3 回とも再現） |
| デマルチプレクサ | 設定は全部通る。**バッファ作成の 1 本だけ未同定** |

## 32. ★ バッファを作る呼び出しは `nr=0x14` だった（2026-09-22）

§31 で「バッファオブジェクトを作る 1 本が未同定」としていた件。**ベンダの
起動トレースを採り直して同定した。`nr=0x14` が確保系。**

### 32.1 採り方（§31.4 の「実機不要」は誤り）

§11.8 のトレースは実機の `/tmp` にあって電源で消えていた。∴ 採り直した:
段階 B のリグ（`openird-pre.sh.bak`・`openird-trace`）に戻して起動し、
**ベンダ自身の UPnP で選局させた**（`X_SetTuning tb 503`）。

> ついでの確認: このときベンダは **`Cn=33 / Ber=0 / SignalStrength=89`** を返した。
> 同じ 503 MHz で我々が測った `0x112d ≈ 0x0225` は §4.3 の対応表で 33 dB 相当＝
> **自前の測定がベンダの値と独立に一致した**（§30.2 の裏付け）。

### 32.2 ★ ベンダが 1 バッファごとに繰り返す組

デバイス 1（`0x00010000`）に対して、この 5 本が**バッファの数だけ繰り返される**:

```
nr=0x0b {7, 0xa9000040, dev, 0xa9000044}    PID フィルタ確保（ハンドルは +0x44 へ）
nr=0x14 {0, 0xa9000054, dev, 0}             ★ バッファオブジェクト確保（id は +0x54 へ）
nr=0x1a {bufid, 0xa900008c}
nr=0x2b {bufid, ring, size, 0xa90000d0}     リングを与える
nr=0x15 {0xa9000058, bufid}                 問い合わせ
```

**返る id は `0x0100`, `0x0101`, `0x0102` … と順に増える。** ∴ `0x000001xx` は
**`nr=0x14` が順番に配る id** であって、固定の資源でも平らな空間でもない。
§20.1／§25.1／§31.2 の読みは、いずれもベンダが確保済みの状態を見ていた。

**`nr=0x15` の引数は `{出力先ポインタ, id}` の順**（§25.1 は `{id}` 1 語で呼んで
いた＝呼び方自体が違っていた）。

### 32.3 ★ 自前で全部通った

ベンダアプリ抜き・冷起動から、**`nr=0x32` を除く全部が `ret=0`**:

```
nr=0x01 {0}                                    ret=0
nr=0x02 {1, ptr}                               ret=0  → dev 0x00010000
nr=0x05 {dev, 3}                               ret=0
nr=0x65 {0,1} {1,1} / nr=0x64 {0,1} {1,1}      ret=0
nr=0x0b {7, 0xa9000040, dev, 0xa9000044}       ret=0  → filt 0x00100023
nr=0x14 {0, 0xa9000054, dev, 0}                ret=0  → ★ buf 0x00000100
nr=0x1a {buf, 0xa900008c}                      ret=0
nr=0x2b {buf, 0xac000000, 0x20000, 0xa90000d0} ret=0  ← ★ 自前のリングが通った
nr=0x15 {0xa9000058, buf}                      ret=0
nr=0x0c {filt, 0x0000}                         ret=0   （PID 0 = PAT）
nr=0x16 {filt, buf}                            ret=0  ← ★ フィルタとバッファが結びついた
nr=0x32 {filt, 1}                              ret=-3  （下記）
nr=0x0d {filt, 1}                              ret=0
nr=0x2e {buf}                                  ret=0  ← バッファ開始
nr=0x2f {buf}                                  ret=0  ← 書き込みポインタが読める
```

`nr=0x32` の `-3` は**想定どおり**。トレース上 `nr=0x32` はデバイス 2 のハンドル
（`0x0210xxxx` / `0x0208xxxx`）にしか使われておらず、**デバイス 1 の経路には
出てこない**。∴ この列には要らない。

### 32.4 それでも TS は落ちない

**`nr=0x2f` が返す書き込みポインタは 0 のまま**で、リング（phys `0x0c000000`）は
`0xff` のまま変わらない。∴ **経路は全部開いたが、TS が入ってきていない。**

配線はこれで揃ったので、残るのは**入力側**に絞られた。トレースで我々がまだ
写していないものは:

- **`/dev/val_demux1`**（2 つめのデバイスノード。ベンダは `open` して `nr=0x02` を出す）
- **3 つめのデバイス** `0x01010001` と **`nr=0x05 {dev3, 4}`**＝**入力源 4**
  （我々が使っているのは源 3）
- `nr=0x33 {0x00002002, 0}`
- `nr=0x30`（デバイス 2 側。今回は外した）
- 復調器側で TS 出力を有効にするレジスタ（§9.1 の A3 に残っている 19 本のどれか）

∴ **次は「入力源」まわり**。いちばん安い検査は `nr=0x05` の第 2 引数を 4 にして
みることと、`/dev/val_demux1` 側も開いてベンダの列を写すこと。

## 33. ベンダの「データが来る」周期を丸ごと写したが、まだ入力が無い（2026-09-22）

§32 でバッファが作れるようになったので、**トレースを丸ごと手元に落として**
（`gzip` して `uuencode -m` で吸い出す）、**実際に書き込みポインタが動いている
バッファはどれか**を調べ、その周期をそのまま写した。

### 33.1 ★ データが来ているのはどれか

`nr=0x2f`（書き込みポインタ）の実測値でバッファを仕分けると:

| バッファ | フィルタ | PID | ポインタ |
|---|---|---|---|
| **`0x00000100`** | `0x00100021` | **`0x0000`（PAT）** | **36 回中 18 回が非ゼロ**（`0x2f00`/`0x20`/`0x2fe0`…） |
| `0x00000102` | `0x0010001f` | `0x0010`（NIT） | 9 回中 1 回 |
| `0x00000105` | `0x0010001c` | `0x0014`（TDT/TOT） | 3 回中 2 回 |
| `0x01000123` | `0x01100038` | `0x0014` | 2 回中 2 回 |

∴ **デバイス 1・PID フィルタ・PID 0 → バッファ `0x0100` という、我々が組んだのと
まったく同じ構成にデータが来ている。** 経路の選び方は合っていた。

### 33.2 ★ ベンダの読み出し周期（そのまま）

```
nr=0x15 {0xa9000058, buf}      問い合わせ
nr=0x2e {buf}                  ★ バッファを先に開始する
nr=0x2f {buf}                  書き込みポインタ
nr=0x16 {filt, buf}            結びつけ
nr=0x0c {filt, PID}            ★ PID は結びつけた後
nr=0x33 {h30, 0}               ★ nr=0x30 が返す種別 0x2000 のハンドルを使う
nr=0x0d {filt, 1}              有効化
nr=0x6a {0} ×2                 ★ 蹴り
   … 約 90 ms …
nr=0x2f {buf}                  → 0x20（＝32 バイト来た）
nr=0x17 {filt, buf} / nr=0x0e {filt, 0}   外して止める
```

**我々が間違えていたのは 3 点**: ①`nr=0x2e` を最後に呼んでいた（**先**である）
②`nr=0x33` を呼んでいなかった ③`nr=0x6a` を呼んでいなかった。
**訂正して流し直したところ、この周期は全部 `ret=0` で通る。**

### 33.3 それでもポインタは 0 のまま＝TS が入ってきていない

デマルチプレクサ側は**ベンダと同じ呼び出しが同じ順で全部通る**のに、
`nr=0x2f` は 0 のままでリングも変わらない。∴ **残っているのは入力側だけ。**

### 33.4 ★ 入力ポートらしき 3 本を見つけたが、まだ呼べない

トレースの `t=10.50`、デバイス 1 のバッファを全部作り終えた直後に、
**それ以外では 1 回ずつしか出てこない 3 本**が並んでいる:

```
nr=0x0a {0, 0xa9000038, dev1, 0xa900003c}              確保系（出力先が 2 つ）
nr=0x47 {dev1, 0, 0x00006978, 0x00000003, 0x00000064}  ← 0x6978 = 27000（27 MHz?）, 源 3
nr=0x67 {dev1, 0, 0x00000001, 0x00000003, 0x00000000}  ← 1 = 有効化?, 源 3
```

数字の形（27000 と「源 3」）からして **TS 入力ポートの設定と開始**に見える。
ところが**我々が同じ引数で呼ぶと 3 本とも `-3`**、`nr=0x34 {h30, 0}` は `-8` になる。

`-3` は「ハンドルが違う」。デバイスの値（`0x00010000`）はベンダと同じなので、
**まだ作っていない何かに依存している**とみるのが自然。いちばん疑わしいのは:

- **`/dev/val_demux1`**（2 つめのノード）を開いていないこと。ベンダはデバイス 2 の
  フィルタと、デバイス 3 そのものを**こちら側の fd で**作っている。
- **デバイス 2（`0x02010002`）とデバイス 3（`0x01010001`・入力源 4）**が無いこと。
  ベンダの `nr=0x30` は**デバイス 2**に対して呼ばれ、返るハンドルは `0x00002002`。
  我々のはデバイス 1 の `0x00002000` で、`nr=0x34` はこれを受け取らない（`-8`）。

### 33.5 ついでに分かったこと

- **`/dev/h57fmb0`（Dione / MB86H58）が `t=5.5` に `nr=0x1d` を 3 回呼び、
  その引数に §10.2 で見つけた TS リング `0x0acea000` がある**
  （`{0, 0x003ac000, 0xaacea000, 0x2abd6000}` ほか 3 本）。∴ あのリングは
  **デマルチプレクサではなくトランスコーダ側の持ち物**だった。
- ベンダが触るデバイスノードの全体像（この 1 起動ぶん）:
  `val_demux` / `val_demux1` / `io_controller`（`nr=0x03` を 1,259 回）/ `val_m2dsc` /
  `val_disp` / `val_dcnv` / `val_videnc` / `val_osd` / `val_jpeg` / `val_prcg` /
  `mem2` / `h57fmb0`。

### 33.6 次

**ベンダの起動列を「3 つのデバイスを 2 つのノードで作る」ところまで丸ごと写す。**
具体的には `/dev/val_demux1` も開き、デバイス 2（源 3）とデバイス 3（源 4）を作り、
`nr=0x30` をデバイス 2 に対して呼んでから、§33.4 の 3 本を出す。それが通れば
§33.2 の周期にデータが来るはず。

## 34. 3 デバイス・2 ノードまで写したが、入力ポートの 3 本は通らない（2026-09-22）

§33.6 の筋を実装して確かめた。**構造はベンダと完全に同じところまで再現できたが、
`nr=0x0a` / `nr=0x47` / `nr=0x67` だけはどうしても `-3` を返す。**

### 34.1 道具: `yb1-ioctl` が複数ノードを跨げるようになった

ベンダは **`/dev/val_demux` と `/dev/val_demux1` を同時に開いて使い分ける**
（ドライバの状態は open ごと・§20 脚注）。`tools/yb1-ioctl` のスクリプトに

```
dev <path>     そのノードに切り替える（開いた fd は閉じずに保持する）
```

を足した。**開き直すと状態が消える**ので、パスごとに 1 回だけ開いて覚える。

### 34.2 ★ ベンダと同じ構造まで再現できた

```
[val_demux]  nr=0x01 → nr=0x02 → dev1 0x00010000 → nr=0x05 {dev1,3} → 0x65/0x64
[val_demux]  nr=0x02 → dev2 0x02010002 → nr=0x48 → nr=0x05 {dev2,3}
[val_demux1] nr=0x0b {7,…,dev2} → 0x0210006b   nr=0x0b {6,…,dev2} → 0x0208006a
[val_demux]  nr=0x0c → nr=0x30 {dev2,1,2,5,…} → 0x00002002 → nr=0x32 → nr=0x0d
[val_demux]  nr=0x33 {0x00002002, 0}                                    ret=0 ★
[val_demux1] nr=0x02 → dev3 0x01010001
[val_demux]  nr=0x05 {dev3, 4}
[val_demux]  nr=0x0b {6,…,dev1} → 0x00080023 → nr=0x19 → nr=0x26        ret=0
[val_demux]  nr=0xc9 {7,1} / {8,1}                                      ret=0
```

**ハンドルの値がベンダと 1 つ残らず一致する**（`0x00010000` / `0x02010002` /
`0x01010001` / `0x0210006b` / `0x0208006a` / `0x00080023` / `0x00002002`）。

- **解決**: §33.4 で `-8` だった `nr=0x33 {0x00002002}` は、**デバイス 2 の
  フィルタを先に作れば通る**（確保 → PID → `nr=0x32` → `nr=0x0d`）。
  §33 で「デバイス 1 のハンドル `0x2000` なら通る」と書いたが、正しくは
  **デバイス 2 側を揃えれば `0x2002` で通る**。

### 34.3 ★ それでも `-3`

```
nr=0x0a {0, 0xa9000038, dev, 0xa900003c}              -3
nr=0x47 {dev, 0, 0x00006978, 0x00000003, 0x00000064}  -3
nr=0x67 {dev, 0, 0x00000001, 0x00000003, 0x00000000}  -3
```

**潰した可能性**（どれも `-3` のまま変わらない）:

| 変えたもの | 結果 |
|---|---|
| `dev` をデバイス 1 / 2 / 3 のどれにしても | 3 つとも `-3` |
| `/dev/val_demux` / `/dev/val_demux1` のどちらから出しても | `-3` |
| ペイロードフィルタの経路（`0x0b`{6} → `0x19` → `0x26`）を先に通しても | `-3` |
| `nr=0xc9 {7,1}` `{8,1}` を先に出しても | `-3` |
| `nr=0x30`/`nr=0x33` がデバイス 2 で成立した状態でも | `-3` |

トレース上、ベンダの `nr=0x0a` の**直前は 28 個目のバッファ群で、特別な前置きは
無い**（`L370`–`L381`）。∴ 近傍に手がかりは無い。

### 34.4 ★ 追加で潰したもの

| 試したこと | 結果 |
|---|---|
| **`/dev/io_controller` を開いて `nr=0x03 {0x0c}` → `nr=0x01 {1}` を出してから** | 両方 `ret=0`。**それでも 3 本は `-3`** |
| `nr=0x0a` の第 1 語を `0` / `1` / `6` / `7` に変えて | **全部 `-3`**。∴ この語は `nr=0x0b` の「種別」のようには振る舞わない |
| `nr=0x47` の第 2 語を `0` → `1` に変えて | `-3` |

∴ **`-3` は引数にも、我々が作れる状態にも反応しない。**

**解決したもの**: `nr=0x34 {0x00002002, 0}` は `nr=0x33` と同じく
**デバイス 2 のフィルタを先に作れば `ret=0`**。これでトレースに出てくる
「前向きの」呼び出しのうち、**`nr=0x0a` / `nr=0x47` / `nr=0x67` の 3 本以外は
すべて自前で通せるようになった。**

### 34.5 残る筋（優先順）

1. **`-3` の意味を疑う。** 戻り値の読み（§20.6）は実測からの当てはめで、
   この 3 本については「ハンドル違い」ではなく「そのオブジェクトが無い」
   （`ESRCH`）かもしれない。そうだとすると、**このオブジェクトを作るのは
   `val_demux` の外**＝`val_merge` の初期化や、ベンダが起動の早い段階で
   `h57fmb0` / `io_controller` に対してやっている何か。
2. ~~**バッファの数。**~~ **→ §37.2 で消えた**（30 個積んでも 30 回とも `-3`）。
3. ~~**そもそも `nr=0x0a`/`0x47`/`0x67` を飛ばす。**~~ **→ §35 で消えた。** この 3 本は TS 入力ポートの
   設定に見えるが、**推測である**。§33.1 のとおりデータが来るのはデバイス 1 の
   PID フィルタなので、入力が通らない理由が別（復調器の TS 出力が無効、
   SoC 側の TS 受け口が未初期化）である可能性は残っている。
   **復調器側で TS 出力に関わるレジスタ（§9.1 の A3 に残る 19 本）を洗う**のは、
   デマルチプレクサ側とは独立に進められる。

**2026-09-22 追記**: **2 は §37.2 で、3 は §36 で実測により消えた**（配信セッション中の
バスを実際に採り、復調器に書くもの・読むものが増えないことを確かめた）。
∴ **残るのは 1 だけ**＝`-3` の意味を疑うこと。続きは §37.3。

## 35. 復調器側は「ロックして取り込み中」でも同じだった（2026-09-22）

§4.3 と §29 のレジスタは**すべて「選局まで」**で採ったもので、§10.4 が言う
「リングを回すのは配信セッションであって選局ではない」が正しければ、**いくら
この列を再現しても TS は来ない**ことになる。確かめた。

### 35.1 採り方

**ロックした走行**と**ロックしなかった走行**を、同じ条件で突き合わせた。

| | 走行 A | 走行 B |
|---|---|---|
| 選局 | ベンダが起動時に 473 MHz（**ロックせず**） | そこから UPnP で 503 MHz（**ロック**） |
| 実測 | — | `Cn=32 / Ber=0` |
| 取り込み | 無し | **有り**（§33.1 のとおり、この状態でベンダは PAT をバッファに取っている） |

**採取の健全性も確かめた**: 走行 B のログにもファーム転送 301 ブロックが揃って
おり、中身は MBIN とバイト単位で一致＝ログ環の溢れで落ちた区間は無い。

### 35.2 ★ 結果＝同じだった

```
走行 A で書くレジスタ : 36 種
走行 B で書くレジスタ : 37 種
B にしか無いもの      : 0xe029        ← §29.4 の選局列に元からある（0xe028 と対）
A にしか無いもの      : 無し
同じレジスタで B だけが書く値 : 0x3029 <- 0x7dc9（503 MHz の同調語。当然）
B にしか無い読み      : 無し
```

∴ **取り込みが始まっても、復調器に書くものも読むものも増えない。**
`tools/yb1-mxl` が §29 の列で書いているもので**全部**である。

### 35.3 含意

- **§34.5 の仮説 3 は「ロック／非ロック」の軸では消えた。** ただし
  **比べたのは「ロックした／しなかった」であって「配信した／しなかった」ではない。**
  どちらの走行も配信セッションは開いていないので、**配信時にだけ書かれるレジスタが
  あれば、この比較では検出できない**（§35.4）。∴ 言えるのは
  「選局してテーブルを取り込むまでの経路について、復調器側に書き漏らしは無い」
  までで、**TS 出力ピンの駆動が配信時に有効化される可能性は残る**。
  C/N が出て `Ber=0` なのは「復調できている」ことしか示さず、TS 出力が
  駆動されているかは別の事実である。
- **ベンダは選局だけで PAT を取り込んでいる**（§33.1 のデータは UPnP 選局の直後に
  来ており、配信セッションは開いていない）。∴ 我々が再現しようとしている経路に
  ついては、**配信セッションは前提ではない**。
- ∴ **差は SoC 側にしか無い**＝§34 の `nr=0x0a` / `nr=0x47` / `nr=0x67` に戻る。

### 35.4 配信セッションについて（まだ観察していない）

**トランスコードを伴う本当の配信セッション中のバスは、まだ一度も見ていない。**
上の結論は「選局＋テーブル取り込み」までの話で、**配信時に復調器の設定が変わる
可能性は残っている。これは優先度が低い残件ではなく、本命の仮説のひとつである。**

理由: セクション抽出（PAT/NIT）の経路と、TS を取り出す経路が**別々に有効化される
作り**なら、「選局だけでセクションは来るが TS は来ない」が成り立つ。§33.1 で見た
データはセクション側であって、我々が欲しい生 TS ではない。

観察の手立ては次のとおり（§5.7）:

| 経路 | ペアリング | こちらから駆動できるか |
|---|---|---|
| `X_SetTuning` / `X_GetOfdmTunerInfo` | 不要 | **できる**（本節はこれで採った） |
| `X_StartChannelScan` | 不要 | **できる**（多数の周波数で実際に取り込む） |
| `live.TB.….at1` の視聴 | **要**（`X_RequestPairing` → `X_AuthenticatePairing`） | **できない**。ペアリングは DRM 隣接なので解析しない（§5.7・§1-2） |

∴ 本当の配信セッションを観察するなら、**純正 PC クライアントで再生を開始して
もらい、その間にトレースを採る**しかない。

## 36. ★ 配信セッション中も復調器は同じだった（2026-09-22・本命仮説の決着）

§35 の比較は「ロックした／しなかった」であって「配信した／しなかった」ではなく、
**配信時にだけ TS 出力が有効化される**可能性が残っていた（§35.4）。
**純正 PC クライアントで実際に視聴してもらい、その間のバスを採って決着させた。**

### 36.1 採り方

段階 B のリグで **I2C と ioctl を同時に記録**する長時間キャプチャを張り
（`openird-pre-capture.sh`・吸い出しループ 20 分）、**視聴開始の直前に行数の
マーカーを取ってから**、所有者に純正クライアントで地上波を 1 局再生・30 秒・停止
してもらった。

> **運用上の教訓**: 最初に張ったリグは 5 時間放置したせいで、吸い出しループが
> とっくに終了し、ioctl ログが `/tmp`（52MB）を食い切って**何も記録できない状態**に
> なっていた。**張ってから使うまでを短く保つこと。**

### 36.2 ★ 結果＝配信しても増えない

マーカー（I2C 1,556 行）の前後で比べた:

```
マーカー前に書くレジスタ : 37 種
マーカー後（配信ぶん）   : 21 種   ← 前の部分集合
配信中にだけ書くレジスタ : 無し
配信中にだけ読むレジスタ : 無し
同じレジスタで配信中だけの値 : 0x3029 <- 0x8b49
```

`0x8b49` は §29.4 の式で **557 MHz**＝視聴のために選ばれた局
（§5.7 の一覧で NHK 総合）。**つまり「選局し直した」以外に何も起きていない。**

配信開始時にバスへ出たのは、`0xe028`/`0xe029` の対を繰り返したあと、
**§29.4 の選局列がそのままの順で 1 回**（`0x301e`/`0x301f` → `0x3002`/`0x3003` →
`0x0012` → `0x3028`/`0x3029` → `0x007c` … → `0x3029 <- 0x8b49` → `0x101b <- 1`）。

∴ **復調器は、配信していてもいなくても、まったく同じに駆動される。**
`tools/yb1-mxl` が §29 の列で書いているもので**全部**である。
**§34.5 の仮説 3 は、これで完全に消えた。**

### 36.3 副産物＝配信の構造

同時に採った ioctl から、配信が誰を動かすかが見えた（この 1 セッションぶん）:

| ノード | 目立つ呼び出し |
|---|---|
| `val_demux` | **`nr=0x2f` が 3,871 回**（書き込みポインタの常時ポーリング）、`nr=0x21`/`0x22`（STC 読み書き）各 39、`nr=0x16`/`0x17` 各 39 |
| `h57fmb0` | `nr=0x02` 743 / `nr=0x03` 726 / `nr=0x11` 19 — **トランスコーダが主役** |
| `val_dcnv` | `nr=0x02`/`0x04`/`0x05`/`0x07` 各 180、`nr=0x06` 360 |
| `val_m2dsc` | `nr=0x05` 21（デスクランブラ。**スコープ外**・§0） |

∴ 配信経路は **demux → デスクランブラ → トランスコーダ → 網**。我々が欲しい
生 TS はその上流で、**この経路を再現する必要はない**（§4.1）。

## 37. `nr=0x0a`/`0x47`/`0x67` は起動時 1 回きりで、個数でもなかった（2026-09-22）

### 37.1 配信とは無関係

配信を含むトレースでも、この 3 本は **`t=12.76` に 1 回ずつ出るだけ**で、
**視聴中には一度も呼ばれない**。∴ 起動時の初期化であって配信の一部ではない。
我々が呼べない理由は配信とは関係が無い。

### 37.2 ★ 個数でもない

ベンダの走行で `nr=0x0a` が成功する直前の状態を数えた:

```
nr=0x0a 成功の直前   バッファ 28 個 / フィルタ 45 個
```

そこで **「PID フィルタ 1 個＋バッファ 1 個を確保するたびに `nr=0x0a` を叩く」を
30 回繰り返す**スクリプトを流した（1 回の走行で個数を二分できる）。

```
nr=0x0a の試行     : 30 回
  ret=0            : 0
  ret=-3           : 30
確保できたフィルタ : 33（全部 ret=0）
確保できたバッファ : 30（全部 ret=0）
```

**バッファ 30 個・フィルタ 33 個まで積んでも、`nr=0x0a` は一度も通らない。**
∴ **§34.5 の仮説 2（個数）も消えた。**

### 37.3 残るのは 1 つだけ

§34.5 の 3 つのうち、**2 と 3 は実測で消え、残るのは 1＝`-3` の意味を疑うこと**。

我々が `-3` を「ハンドル違い」と読んだのは実測からの当てはめ（§20.6）で、
この 3 本については **`ESRCH`＝そのオブジェクトが存在しない**の可能性がある。
そうだとすると、**`nr=0x0a` が名指すものを作るのは `val_demux` の外**＝
`val_merge` の初期化か、ベンダが起動のもっと早い段階で
`h57fmb0`（`t=5.5` に `nr=0x1d` を 3 回）や `io_controller` に対してやっている何か。

**次に見るべきは、ベンダ起動トレースの `t=12.76` より前で、
`val_demux` 以外のノードに出ている呼び出し**。`h57fmb0` の `nr=0x1d`（3 回・
引数に §10.2 の TS リング `0x0acea000` が入る）と `nr=0x08`、`mem2` の `nr=0x00`、
`io_controller` の最初の数本が候補。

## 38. 起動初期の非 demux 呼び出しを洗い出し、丸ごと再生した（2026-09-22）

「差は demux ioctl として現れない SoC 側の物理的な有効化で、それは選局・配信より
前の起動初期に済んでいる」という見立てに沿って、**ベンダ全起動トレースを
『各 (ノード, nr) が初めて出た瞬間』に畳み**、`val_demux` を除いて差分した。

### 38.1 ゲート候補＝15 個

`nr=0x0a` が成功する `t=12.76` より前に、非 demux ノードへ初めて出る呼び出し:

```
t=5.274   h57fmb0   nr=0x1d {0, 0x003ac000, 0xaacea000, 0x2abd6000}  ×3
t=5.275   h57fmb0   nr=0x08 (R) {0x06b8, 0x102d, 3}
t=11.245  val_prcg  nr=0x01      t=11.310  val_osd    nr=0x01
t=11.317  val_jpeg  nr=0x01      t=11.326  val_videnc nr=0x0f / nr=0x1d
t=11.328  val_disp  nr=0x03 / nr=0x05 / nr=0x07
t=11.514  val_m2dsc nr=0x04 / nr=0x06        （デスクランブラ・スコープ外）
t=12.175  io_controller nr=0x01 {1}
t=12.661  mem2      nr=0x00 (R) {0xa6400000}
t=12.700  io_controller nr=0x03 {0x0c}
```

### 38.2 起動を丸ごと再生した

`t<=12.77` のベンダ呼び出し **329 本**を、そのまま流せるスクリプトに起こした
（我々の確保順はベンダと同じでハンドルの値も一致するので、引数は literal で書ける
・§34.2）。道具も広げた: `yb1-ioctl` の `dev` は**最大 16 ノード**、スクリプト
バッファは **64KB**。

**結果: `nr=0x0a`/`0x47`/`0x67` は変わらず `-3`。** 415 行のうち 105 本が非ゼロで、
内訳は「我々の状態では出せない呼び出し」だった:

| 失敗 | 数 | 理由 |
|---|---|---|
| `h57fmb0` の 4 本 | 4 | **ノードが開けない**（`-19` ENODEV）。§38.3 |
| `val_videnc` / `val_disp` / `val_osd` / `val_jpeg` / `val_prcg` | 12 | 表示・符号化系。`val_merge` 側の初期化が足りない |
| `io_controller` `nr=0x01` | 55 | |
| `mem2` `nr=0x00` | 1 | `-14`（EFAULT） |

### 38.3 ★ `/dev/h57fmb0` が開けない理由＝Dione は USB デバイスで、消える

`dmesg` にそのまま出ていた:

```
usbcore: registered new interface driver h57fmb
usb 1-1: new high speed USB device using emma-ehci and address 2
h57fmb:  vendor : 0x06b8     device : 0x102d     speed : 0x3
h57fmb: 248:USB fmb device now attached to h57fmb   major 180  minor 248
usb 1-1: USB disconnect, address 2                  ← ★
h57fmb: 248:USB fmb device now detached
```

∴ **`/dev/h57fmb0` は USB キャラクタデバイス**（major 180）で、**Dione が
繋がっている間しか開けない**。トレースの `pid 967` は blur（`pid 1080`）ではなく、
`setup.sh` の

```sh
insmod /app/bin/drivers/h57fmb.ko
/app/bin/dione_transcode -w -P $DIONE_SEL_PWR_CTRL     # LITE は 1
```

の **`dione_transcode`** だった。SPI-ROM を書いて終わると Dione が切れるので、
その後に開こうとしても `ENODEV` になる。∴ **これは「我々に欠けているゲート」では
なく、再生の時機が違うだけ**。

`nr=0x1d` ×3 が Dione に渡す DMA 領域に **§10.2 の TS リング `0x0acea000`** が
入っていることも、これで筋が通る（§36.3 の「あのリングはトランスコーダの持ち物」
の裏づけ）。

### 38.4 ★ 新しい手がかり＝`dione_transcode` は TS 入力ピンを扱う

同じ実行ファイルの usage に:

```
-t  send input data by TS Input pin
-d  send input data by DMA (USB Bulk Transfer)
-p <count>   repeat dione power on/off
-P <selPwrCtrl>
-w           Write firmware to SPI-ROM
```

**`-t` が「TS 入力ピンで入力を送る」**と明言している。∴ **SoC の TS 入力ブロックを
触る実行ファイルが、`val_demux` の外に、起動初期に走っている**。
「差は demux ioctl として現れない」という見立てと合う。

### 38.5 観測の盲点（道具の限界）

**いまのトレーサは `ioctl` と `open`/`close` しか見ていない。**

- `write()` / `read()` は既定で記録しない（`openird-trace-rw` で有効化できる。
  起動だけなら量は問題にならない）。
- **`/dev/mem` を mmap して直接レジスタを叩く経路は、原理的に見えない。**
  `pid 967` は `/dev/mem` を 2 回開いてから Dione を触っており、blur も
  `t=3.24` / `t=11.21` / `t=11.49` / `t=12.68` に開いている。**ここで SoC の
  TS 入力ブロックのクロック／電源を叩いていても、トレースには何も出ない。**

∴ 盲点を埋める道具として、**ベンダ起動後と noblur 起動後で SoC のレジスタ窓を
そのまま突き合わせる**のが要る（`yb1-mem <phys> <len> words`）。ベンダが map する
窓は BHIF `0x10000000` / XDMA `0x10009000` / timer `0x10110000`（`tools/yb1-mem`
の由来コメント）。**差分が出た窓が、そのまま有効化ビットの在り処になる。**

### 38.6 次

1. **`dione_transcode` 自身をトレースする。** 別プロセスなので `LD_PRELOAD` を
   効かせれば `ioctl`＋`open` は採れる。`openird-trace-rw` も併せて立てる。
   **`-t`（TS 入力ピン）で何をするかが本命。**
2. **レジスタ窓の差分**（§38.5）。トレーサに映らない経路を捕まえる唯一の手。
   これは **`-3` の謎を迂回して答えに届く可能性がある**。

## 39. ★ レジスタ窓の差分で、SoC 側の未設定が 24 語に絞れた（2026-09-22）

§38.5 の盲点（`/dev/mem` 経由の書き込みはトレーサに映らない）を埋めるため、
**ベンダ起動と noblur 起動で SoC のレジスタ窓をそのまま突き合わせた。**

### 39.1 採り方＝再現する語だけを見る

素の 1 対 1 比較では 96 語が違ったが、大半は自走カウンタや残留データだった。
**2 段のふるい**をかけた:

1. **走行内**で 2 枚撮り（5 秒あけ）、値が動く語を落とす
2. **noblur は 2 回起動**して、両方で同じ値になる語だけを「設定」とみなす

どちらも **503 MHz に選局済み**（noblur 側は `yb1-mxl` ＋ `stage-c-demux.script`、
ベンダ側は UPnP で `Cn=32 / Ber=0`）。窓はベンダが map することが分かっている
BHIF `0x10000000` / XDMA `0x10009000` / timer `0x10110000` の 3 つ（各 4KB）。
クロックの来ていないブロックを読むとバスが止まる恐れがあるので広げていない。

```
比較した語            : 3072
再現する語            : 2986
そのうち値が違うもの  : 24
```

### 39.2 ★ 24 語

```
=== BHIF ===
  0x10000024  noblur=00000002  vendor=00000000
  0x10000110  noblur=00000080  vendor=000964a5
  0x10000114  noblur=00000000  vendor=00000040     ← 我々だけゼロ
  0x10000118  noblur=00000eee  vendor=00003eee
  0x100001e4  noblur=00000000  vendor=ffffffff     ← ★ 我々だけゼロ
=== TIMER ===
  0x10110900  noblur=ffff0000  vendor=afc800c7
  0x10110d04  noblur=00000014  vendor=00100014
  0x10110d34  noblur=0ffffffb  vendor=0feffffb
=== XDMA ===
  0x10009314  noblur=09c04800  vendor=015fb000
  0x10009318  noblur=0358c400  vendor=015fa800
  0x1000931c  noblur=00c03000  vendor=015fa800
  0x10009340  noblur=00000000  vendor=015fb000     ← ★ 我々だけゼロ
  0x10009344  noblur=00000000  vendor=015fa800     ← ★ 我々だけゼロ
  0x10009380  noblur=0b184e00  vendor=0000c800
  0x1000950c  noblur=00000000  vendor=00040000     ← ★ 我々だけゼロ
  0x1000953c  noblur=00000000  vendor=00040000     ← ★ 我々だけゼロ（同じ値・0x30 離れ）
  0x10009590  0x10009710  0x1000971c  0x10009730
  0x10009770  0x10009778  0x10009788  0x100097e4   （細かい差）
```

### 39.3 見立て（未確認）

一貫した形が 3 つ読める:

- **XDMA に 2 つのアドレスが仕込まれている**: `0x015fb000` と `0x015fa800`
  （0x800 = 2KB 違い）が `0x10009314/18/1c` と **`0x10009340/44`** に現れる。
  **我々は `0x9340/0x9344` がゼロ**＝**DMA の基底が置かれていない**形。
- **`0x1000950c` と `0x1000953c` が同じ `0x00040000`（256KB）**で 0x30 離れ＝
  **2 本のチャネルの長さ**に見える。**我々は両方ゼロ。**
- **`0x100001e4` がベンダでは `0xffffffff`、我々はゼロ**＝**全ビット許可の
  マスクに見える**（割り込みか経路の許可）。

∴ **「DMA の基底と長さが置かれ、マスクが開いている」対「全部ゼロ」**という
読みが素直で、**これはトレーサに一切映らない**（`/dev/mem` 経由だから）。
§38 で demux ioctl を丸ごと写しても TS が来なかったことと矛盾しない。

### 39.4 ★ この手法の意味

**ioctl を畳んで差分する手法には構造的な天井がある**（§38.5）。レジスタ窓の
差分はその外側を見る唯一の道具で、実際に**信号は demux ioctl ではなく SoC の
DMA 設定に出た**。「差は demux ioctl として現れない」という見立ては正しかった。

### 39.5 次

1. **ベンダ側をもう 1 回起動して 24 語が再現するか確かめる**（いまベンダ側は
   1 起動ぶんしか無い。noblur 側は 2 起動で確認済み）。
2. 再現したら、**noblur リグでその語を書いてみる**。`yb1-mem` は読み専用なので
   書ける道具（`yb1-poke` 相当）が要る。**電源の入れ直しで戻せるので試せる。**
   まず `0x10009340`/`0x10009344`（基底）と `0x1000950c`/`0x1000953c`（長さ）、
   次に `0x100001e4`（マスク）。
3. 窓を広げる余地もある（PBRD `0x10100000` 台など）。ただしクロックの来ていない
   ブロックを読む危険があるので、広げるなら u-boot のヘッダで裏を取ってから。

## 40. 差分の語を書いても TS は来ない＝あれは結果であって原因ではない（2026-09-22）

§39 の 21 語のうち「我々だけゼロ」の 5 語を、`tools/yb1-poke`（新規・静的・
`/dev/mem` を mmap して 32bit を書く）で noblur リグに書き込んでから
`stage-c-demux.script` を流した。

### 40.1 ベンダ側の再現性は確認できた

ベンダをもう 1 回起動して突き合わせ直した。**2 回のベンダ起動と 2 回の noblur
起動の全部で再現する差は 21 語**（§39 は 24 語だったが、3 語はベンダ側の
起動ごとに揺れていた）。**決め手の 5 語は全部残った。**

### 40.2 書き込みの結果

DMA の宛先は**ベンダの `0x015fb000` ではなく自前の `0x0c000000` 台**に向けた。
ベンダのそれは我々の起動ではカーネル RAM の中で、DMA を向けると壊すため。

```
0x100001e4 : 00000000 -> ffffffff   効いた
0x1000953c : 00000000 -> 00040000   効いた
0x10009340 : 00000000 -> 0c001000   効いた
0x10009344 : 00000000 -> 0c000800   効いた
0x10009314/18/1c                    効いた
0x1000950c : 00000000 -> 00000000   ★ 書いても入らない
```

**そのうえで `nr=0x2f` は 0 のまま、リングも変わらない。基板は無事。**

### 40.3 ★ 分かったこと

- **あの 21 語は結果であって原因ではない。** 値を置いても経路は開かない。
  ベンダのそれらは、もっと手前の「本当の有効化」の結果として書かれている。
- **`0x1000950c` は書いても入らないのに `0x1000953c` は入る**（0x30 離れた対）。
  ∴ 片方は読み取り専用か、その面がまだ有効化されていない。
  **これ自体が「手前に何かある」ことの傍証。**
- `nr=0x0a`/`0x47`/`0x67` も相変わらず `-3`。

### 40.4 ★ 次＝窓が足りていない

比べたのは BHIF / XDMA / timer の 3 窓だけだった。**GPL のカーネルヘッダ
（`include/asm-mips/emma/`）に、まだ比べていない窓がある**:

```
0x10000000  BHIF          ← 比較済み
0x10000400
0x10009000  XDMA          ← 比較済み
0x10012000                ← 未
0x10100000  PBRD          ← 未（§4.4 の PIIC0/1/2 はこの中）
0x10110000  timer         ← 比較済み
0x10113000                ← 未
0x10200100                ← 未
0x11000000                ← 未
```

∴ **未比較の 5 窓を同じ手順（走行内 2 枚＋起動 2 回のふるい）で差分する。**
窓の素性は GPL ヘッダから取れるので、クロックの来ていないブロックを読む危険も
事前に評価できる（`clkdev.h` / `clock.h` がある）。

**`0x1000950c` が書けないことと合わせて、「クロックか電源がまだ来ていない面が
ある」という筋が有力**。`clkdev.h` / `clock.h` は GPL なので、どのブロックに
どのクロックが要るかはソースから読める（`Origin: gpl-derived` で実装可能）。

## 41. ★ レジスタ窓の筋は尽きた＝TS の有効化は MIPS から見えるレジスタではない（2026-09-22）

§40 で「窓が足りない」としたので、**GPL のカーネルヘッダで窓の素性を確かめた**
（`include/asm-mips/emma/emma3sep.h`）。結論は逆で、**窓の筋はここで尽きる。**

### 41.1 ★ GPL が差分に名前を付けてくれた

§39 の差分のうち BHIF の 4 語は、**名前の付いたレジスタ**だった:

| アドレス | GPL の名前 | noblur | vendor |
|---|---|---|---|
| `0x10000024` | **`EMMA_BHIF_AVSWITCH0`** | `00000002` | `00000000` |
| `0x10000110` | `EMMA_BHIF_SUB_INT_EN_0` | `00000080` | `000964a5` |
| `0x10000114` | `EMMA_BHIF_SUB_INT_EN_1` | `00000000` | `00000040` |
| `0x10000118` | `EMMA_BHIF_SUB_INT_EN_2` | `00000eee` | `00003eee` |

**`AVSWITCH0`＝AV 経路の切り替え**で、我々はベンダと違う値を持っていた。
いかにも本命に見えたので、4 語ともベンダの値にしてから起こした。

**4 語とも書けて、最後まで保持された（`AVSWITCH0` は 0 のまま）。
それでも `nr=0x2f` は 0、リングも変わらない。**

### 41.2 ★ 残りの窓は AV とは無関係だった

GPL ヘッダが挙げる窓は**これで全部**:

| 窓 | GPL の名前 | 中身 |
|---|---|---|
| `0x10000000` | `BHIF` | ホストインターフェース・割り込み。**比較済み** |
| `0x10000400` | `BROI` | **NAND ROM インターフェース**。TS と無関係 |
| `0x10009000` | `XDMA` | DMA。**比較済み** |
| `0x10012000` | `ETHER` | Ethernet。無関係 |
| `0x10100000` | `PBRD` | 周辺ブリッジ（FUART・I2C の割り込みマスク）。無関係 |
| `0x10110000` | `TIMER` | **比較済み** |
| `0x10113000` | `SATA_PORT` | 無関係 |
| `0x10200100` | `FLIP` | ローカルバス橋（`flip.h`＝PCI 風のエンディアン制御と USB/IDE のリセット）。無関係 |

∴ **EMMA3SEP の MIPS 側に「AV / TS 入力」のレジスタブロックは存在しない。**

これは §7 の確定事実と一致する: `arch/mips/emma` に登録されるデバイスは
i2c / Ether / USB / NAND / wdt / GPIO / SATA / SPI だけで、**受信系は皆無**。
AV は裏 CPU（MIPS 4KE）の持ち物で、メイン CPU からは
**共有メモリのコマンドスタック越しにしか触れない**（§12）。

### 41.3 ∴ 振り出しではなく、壁が 1 点に確定した

- **`/dev/mem` 経由の隠れた有効化」という筋は消えた。** MIPS から見える窓に
  TS 入力ブロックが無いのだから、そこには書きようがない。
- **∴ TS 入力の有効化は、裏 CPU 側のコマンドとしてしか存在しえない。**
  そしてそれは `val_demux` の ioctl として現れるはずで、**我々が出せずにいるのは
  `nr=0x0a` / `0x47` / `0x67` の 3 本だけ**（§34・§37・§38 で他は全部通した）。
- ∴ **`-3` を解くことが、迂回路のない唯一の課題になった。**

### 41.4 次＝`-3` がどこで出ているかを見る

**`nr=0x0a` がそもそも裏 CPU に届いているのかを確かめる。** §13 の道具立てが使える:

- `CSD_DEBUG` はビット集合で、コマンドスタックの足跡を吐かせられる（§13.1）。
- エントリ表は物理 `0x00010400`・32 バイト刻みで、**呼び出しの前後で差分を取れば
  その 1 本が生んだコマンドが見える**（§12.2・§14）。

∴ **`nr=0x0a` を出した前後でコマンドスタックを見る。**

- **コマンドが載っていれば**、裏 CPU が拒否している＝引数か前提の問題。
- **載っていなければ**、`val_merge`（メイン CPU 側）が入口で弾いている＝
  ハンドルやオブジェクトの検査に落ちている。§20.6 の `ret=40`（`val_merge` 内で
  完結）との対比で、**どちらかが機械的に分かる。**

これは実機のリグでそのまま試せる（`openird-csddebug` を置いて起動するだけ）。

## 42. ★ コマンドは通るがイベントが一切来ない（2026-09-22・コマンドスタック差分）

「TS 入力の有効化は裏 CPU のコマンドとしてしか存在しえない」（§41.3）を受けて、
**ベンダ起動と noblur 起動で、裏 CPU とのやり取りそのものを差分した。**

### 42.1 採り方

`CSD_DEBUG=65535` で `csd.ko` を load し（§13.1）、**16KB のログ環を 20ms ごとに
吸い出す**フック（`openird-pre-csd.sh`）を両モードで使った。
ベンダ側は UPnP で 503 MHz に選局（`Cn=33 / Ber=0`）、noblur 側は `yb1-mxl` ＋
`stage-c-demux.script`。

### 42.2 ★ 結果

| | ベンダ | noblur |
|---|---|---|
| ログ行数 | 17,964 | 1,172 |
| `Request_ApiExecute` | 685 | 54 |
| `alloc_entry` / `wait_response` / `assert_int` | 有り | **有り**（各 18） |
| `read_sw_int_cmdstk` | 有り | **有り**（18） |
| **`read_sw_int_event`** | **4,989** | **0** |

**コマンドの発行と応答の受け取りは、我々でも動いている。**
`alloc_entry` → `assert_int` → `wait_response` → `free_entry` が 18 組そろっており、
§12 のコマンドスタックは正しく回っている。

**欠けているのはイベントの側だけ。** ベンダは `read_sw_int_event` を 4,989 回
受けている（ほぼ全部 `category = 00`、1 回だけ `0b`）。**我々は 1 回も受けていない。**

### 42.3 ★ 読み

§12.3 のとおり、裏 CPU → メイン CPU の通知は BHIF のソフト割り込みで、
**`CSD_API_RESPONSE`（SW int 0）と、それとは別のイベント経路**がある。

- **応答（API の戻り）は来ている** ＝ 裏 CPU は生きていて、我々のコマンドを
  受理し、返している。
- **イベントは来ない** ＝ **裏 CPU 側の AV パイプラインが走っていない。**
  ベンダの `category = 00` が 2 分で 4,989 回＝毎秒 40 回前後は、
  バッファやフレームの周期的な報告とみるのが自然で、**流れていれば必ず出る**もの。

∴ **「設定は全部受理されているのに何も動いていない」という §33 以降の症状が、
裏 CPU 側の言葉で裏づけられた。** 我々が出せずにいる `nr=0x0a` / `0x47` / `0x67`
が、まさに「走らせる」側の命令である可能性が高い。

### 42.4 §38.2 の「失敗」はほとんど道具の都合だった

あわせて §38.2 で非ゼロを返していた非 demux 呼び出しを個別に確かめた:

| 呼び出し | 実際 |
|---|---|
| `io_controller nr=0x01 {1}` | **失敗ではない。イベント待ち**（ベンダは 422 回呼ぶ＝イベントループ）。何も無ければタイムアウトで返る |
| `io_controller nr=0x03 {0x0c}` | スクリプト形式では `ret=0`。単発形式で `-22` だったのは**引数の詰め方の違い**（単発はバイト、スクリプトは 32bit 語） |
| `val_prcg nr=0x01` | **`ret=0`** |
| `val_disp nr=0x03` | `-65542`。表示系の初期化が要る。**TS とは無関係** |
| `val_videnc nr=0x0f` | 単発形式では引数を渡せなかった（道具の制限）。映像符号化＝**無関係** |

∴ **非 demux ノードにゲートは隠れていない。** 通るものは通り、通らないものは
表示・符号化系で、TS 入力とは関係が無い。**§38 の候補 15 個は、これで尽きた。**

### 42.5 次

**イベントが来ない理由を、割り込みの側から見る。**

- `/proc/interrupts` の **`CSD_API_RESPONSE`（IRQ 104）と `CSD_SPECIAL`（IRQ 105）**
  の回数を、ベンダと noblur で比べる（§12.3 に実測の足場がある）。
  応答が来ているなら 104 は増えているはずで、**105 側が 0 なら
  「イベント割り込みが上がっていない」が数字で確定する。**
- そのうえで、**イベント割り込みを許可しているのは誰か**を探す。
  §41.1 で `SUB_INT_EN_0/1/2` をベンダの値にしても変わらなかったので、
  許可はレジスタではなく**裏 CPU 側の設定**（＝また `nr=0x0a` 系）に戻る公算が高い。

## 43. ★★ ベンダアプリ抜きで放送データを取り出した（2026-09-22）

**OpenIRD 自身のコードが、電波から中身を取り出した最初の記録。**

### 43.1 見つけ方＝回数ではなく「出している呼び出しの集合」で差分した

§42 で「コマンドは通るがイベントが来ない」と分かったあと、
**`Request_ApiExecute` の 685 対 54 を回数ではなく、オペコードと引数の列で差分した。**
`val_demux` の `nr` について「ベンダは出すが我々は出さない」を機械的に洗うと:

```
nr=0x0a / 0x47 / 0x67   … 1 回ずつ（§37。27000 = 27MHz ＝ PCR/STC のクロック系）
nr=0x0e / 0x17          … 無効化・結び外し（後始末）
nr=0x21 / 0x22          … STC の読み書き
nr=0x29 / 0x2a          … ★ ペイロードフィルタのリセットと書き込みポインタ
```

**`nr=0x47` の `0x6978` は 27000＝27MHz** で、`nr=0x21`/`0x22` が STC の読み書き
であることと合わせると、**`0x0a`/`0x47`/`0x67` はクロック（PCR/STC）側**であって、
`category = 00` のデータイベントとは別系統だった。**あの 3 本は道ではなかった。**

### 43.2 ★ 欠けていたのはペイロードフィルタの起動だった

ハンドル `0x00080023`（デバイス 1 の**種別 6＝ペイロード**フィルタ）の一生を
ベンダのトレースから抜くと、たった 7 行だった:

```
t=12.5   nr=0x19 {filt, ptr}                    起動時
t=12.5   nr=0x26 {filt, ring, 0x20000, desc}    起動時（バッファ）
--- ここから配信が始まるとき ---
t=149.3  nr=0x29 {filt}                         ★ リセット
t=149.3  nr=0x2a {filt}  -> 0x0001f800          書き込みポインタ
t=149.3  nr=0x0c {filt, 0x0130}                 ★ PID（リセットの後）
t=149.3  nr=0x0d {filt, 1}                      ★ 有効化
t=187.8  nr=0x0e {filt}                         停止
```

**我々は確保（`0x0b` 種別 6）とバッファ設定（`0x19`/`0x26`）まではやっていたが、
リセット・PID・有効化を一度も出していなかった。** §19.2 が
「ペイロード側は reset が前提」と書いていたのは、まさにこの経路のことだった。

### 43.3 ★ 結果

`nr=0x29` → `nr=0x0c` → `nr=0x0d` を足し、**ベンダが成功していたのと同じ条件**
（557 MHz ＝ NHK 総合・PID `0x0130`）で流した。リングは phys `0x0d000000`。

```
000000: 00 00 01 bd 00 2e 80 81 17 2f ca bf be 39 8e 43
000010: 43 49 53 01 3f ff ff ff ...
000020: 80 ff f0 80 00 00 00 0a 3f 01 1a 6a 70 6e 80 00
```

- **`00 00 01 bd`** ＝ MPEG の **PES スタートコード ＋ `stream_id = 0xBD`
  （private_stream_1）**。続く `00 2e` が PES パケット長、`80 81 17` がフラグと
  ヘッダ長。**正しい PES パケットである。**
- **`43 49 53`** ＝ ASCII `"CIS"`、**`6a 70 6e`** ＝ ASCII `"jpn"`（ISO-639 の
  言語コード）。**PID 0x0130 の ARIB 字幕系データ**とみて整合する。
- **TS の同期バイト（0x47）は立たない。当然で、種別 6 は「ペイロード」フィルタ＝
  TS ヘッダを剥がして PES を渡す**（§11.4 の DEMUX が普通の MPEG-2
  デマルチプレクサであることとも合う）。

**裏 CPU のイベントも 0 → 2 に変わった**（`category = 01` と **`category = 00`**）。
`category = 00` はベンダが 2 分で 4,989 回受けているデータイベントと同じもので、
**§42 で「一切来ない」としていたものが来た。**

### 43.4 到達点

| 柱 | 状態 |
|---|---|
| フロントエンド（MXL683） | **完了**（§30。冷起動から選局・品質測定まで） |
| デマルチプレクサ | **データが出た**（本節） |
| **放送データの取り出し** | **成功**（PES・PID 指定） |

∴ **段階 C の「受信の中核」は通った。** 残るのは形を整える仕事:

1. **生 TS（188 バイト）で取り出す。** いま出ているのは PES。§9.2 の B2
   「素通し指定」＝ TS ヘッダを保ったまま渡す設定を見つける（種別 7 の PID
   フィルタ側か、`nr=0x26` の引数か）。**chardev 契約は生 TS なので、ここは要る**（§4.1）。
2. **PID を自前で知る。** いまは PID を外から与えている。PAT/PMT を読んで
   サービスの PID を引くのは、データが取れた以上ただのパーサ仕事。
3. **`nr=0x0a`/`0x47`/`0x67`（PCR/STC）** は TS の取り出しには要らなかった。
   タイムスタンプが要る段になったら戻る。

## 44. デマルチプレクサに「生 TS」の出口は無い（2026-09-22）

§43 で PES が取れたので、次は §4.1 の chardev 契約が要求する**生 TS（188 バイト）**
を探した。**このデマルチプレクサには無い。**

### 44.1 フィルタ種別の総当たり

`nr=0x0b {種別, ptr, dev, out}` の第 1 語を `0x03`〜`0x0f` まで振って確保を試した
（デバイス 1 に対して）:

| 種別 | ret | 返るハンドルの класс |
|---|---|---|
| `0x03` `0x04` `0x05` | `-3` | — |
| **`0x06`** | **0** | `0x0008xxxx` ＝ **ペイロード**（PES を渡す・§43） |
| **`0x07`** | **0** | `0x0010xxxx` ＝ **セクション/PID** |
| `0x08`〜`0x0f` | `-3` | — |

∴ **有効な種別は 6 と 7 の 2 つだけ。** 「TS をそのまま渡す」種別は無い。

### 44.2 セクション側のリングも空のまま

§43 の成功した走行で、種別 7（PID 0）に結びつけたバッファ `0x0100`
（リング phys `0x0c000000`）は**最後まで `0xff` のまま**だった。
一方でペイロード側（phys `0x0d000000`）には PES が入っていた。

∴ 種別 7 は**セクション**（PAT/PMT/EIT 等）用で、**我々の PID 0 設定では
何も落ちてこなかった**。ベンダは同じ構成で 32 バイト取れているので（§33.1）、
こちらは**まだ手順が足りない**（ペイロード側で `nr=0x29` が要ったのと同じ類い
の前提がある公算）。

### 44.3 ★ chardev 契約への含意

§4.1 は **px4_drv 互換の「生 TS が読める chardev」**を OpenIRD の対外契約と
定めている。**この基板のデマルチプレクサは、その形で TS を出さない。**

考えられる道は 3 つ:

1. **デマルチプレクサを通さない経路を探す。** ベンダの生 TS は
   デマルチプレクサのフィルタではなく、**DMA でトランスコーダ（h57fmb）へ**
   渡っている（§36.3・§38.3 の `nr=0x1d` が `0x0acea000` を渡す）。
   その DMA の宛先を自前のリングに向けられれば生 TS が得られる可能性がある。
   **XDMA の窓（§39.2 で差が出た `0x10009340/44` 等）はこの経路のものかもしれない**
   ＝ §40 で「結果であって原因ではない」とした語に、別の意味が出てくる。
2. **種別 7（セクション）と種別 6（PES）から TS を組み立て直す。**
   **契約としては不十分**。TS ヘッダ・スクランブル制御ビット・PCR が失われるので、
   上位（recisdb 系）が期待するものにならない。§4.1 の趣旨から外れる。
3. **契約を見直す。** この基板では「生 TS」ではなく別の ABI を出す。
   **最後の手段**。エコシステム接続（§4.1 の表）を捨てることになる。

**∴ 次に調べるのは 1。** 具体的には、ベンダが配信中に TS リング `0x0acea000` を
どう回しているか（`h57fmb0` の `nr=0x02`/`0x03` が 743/726 回・§36.3）と、
その宛先を差し替えられるか。

### 44.4 スコープ上の確認（デスクランブラ）

§43 で取れた PES は**そのまま読める**状態だった（`"CIS"` / `"jpn"` が見えた）。
念のため記しておく:

- **我々は `val_m2dsc` / `val_dscram` を一度も呼んでいない。** §43 の列にも
  `stage-c-payload.script` にも、デスクランブラに関する呼び出しは無い。
  `tools/vendor-rig` の replay でも `val_m2dsc` は意図的に外してある（§38.2）。
- ∴ 読めたのは**その PID がスクランブルされていなかった**ためで、
  **我々が解除したのではない。** 限定受信は永久にスコープ外（§0・CLAUDE.md §1-2）
  で、この結果はその境界を一切動かさない。

## 45. PID に「全部」は無い＝ペイロードフィルタは PID 単位でしかない（2026-09-22）

§44.3 の道 2 を潰すため、ペイロードフィルタに与える PID の受理範囲を掃いた。

| PID | `nr=0x0c` |
|---|---|
| `0x0130`（§43 で成功した値） | **0** |
| `0x0100` | **0** |
| `0x1fff`（ヌルパケット） | **0** |
| **`0x2000`** | **`-3`** |
| **`0xffff`** | **`-3`** |

∴ **受理されるのは 13 bit の正しい PID（`0x0000`〜`0x1fff`）だけで、
「全 PID」の約束事は無い。** ペイロードフィルタは設計として PID 単位であり、
**ここから TS ヘッダ付きの流れは出てこない。**

§44.1（種別は 6 と 7 だけ）と合わせて、フィルタの**種別と PID 値**からは生 TS が
出ないことが分かった。

> **⚠ 2026-09-22 訂正**: ここで「フィルタ API から生 TS を得る道は完全に閉じた」と
> 書いたのは**早計だった**。試したのは種別と PID 値だけで、§11.4 が挙げる
> **`ControlHardwarePidFiltering`（全 PID 素通し・`nr=0x48` のフラグ）**と
> **`/dev/val_record`（`MMAC_RECORD_*`）**を触っていない。**§46 を見ること。**

> **この走行ではデータ自体は落ちなかった**（`0x0130` でも書き込みポインタ 0、
> リングは `0x29` のリセットで全部ゼロになったまま）。PID を変えるたびに
> `0x0e`→`0x29`→`0x0c`→`0x0d` を 3 秒間隔で回したためで、**§43 の成功と
> 矛盾する結果ではない**（あちらは 1 回設定して長く待った）。
> 受理/拒否の判定はこの走行で足りるが、**「落ちるかどうか」を見るなら
> 1 PID につき 1 走行にすること。**

### 45.1 残る道は 1 つだけ

§44.3 の 3 つのうち、2（PES から組み立て直す）は契約を満たさず、3（契約の見直し）
は最後の手段。**∴ 道 1＝デマルチプレクサを通さない DMA 経路**しか残っていない。

§10.3 の読みに戻る:

```
MXL683 →（シリアル TS）→ SoC の TS 入力 →[MULTI2 デスクランブラ]→ DRAM リング
                                              ↑ 設定しなければ素通り
```

**この「DRAM リング」へ書く段を、我々の番地に向けられるか**が最後の問いになる。
ベンダのそれは `0x0acea000` で、`h57fmb0 nr=0x1d` が**トランスコーダに
「そこを読め」と教えている**（§38.3）。**書く側**を設定しているのは別の誰かで、
まだ同定できていない。候補は `nr=0x30` が返す種別 `0x2000` のオブジェクトと、
デバイス 3（入力源 4）の経路。

**スコープの再確認**: デスクランブラは経路上の 1 段で、有効化は設定（§10.3）。
OpenIRD はその段を設定しないので、この道で得られるのは
**スクランブルされたままの TS** になる。CAS には触れない。

## 46. §45.1 の「出口は無い」は早計だった＝未検証が 2 つあった（2026-09-22）

§44/§45 で「デマルチプレクサのフィルタ API から生 TS を得る道は閉じた」と書いたが、
**試したのはフィルタ種別（`nr=0x0b` の第 1 語）と PID 値（`nr=0x0c`）だけ**で、
§11.4 の一覧にある次の 2 つを触っていなかった。**§45.1 の結論は取り消す。**

### 46.1 (A) `ControlHardwarePidFiltering`＝全 PID 素通し

§11.4 の DEMUX 関数一覧に **`ControlHardwarePidFiltering`（全 PID 素通し）**が
ある。`nr=0x48` は「デバイスを開いた直後に呼ぶ」もので、**ベンダは必ず `{dev, 1}`
を渡す**（§22.1）。これが hardware PID filtering の有効化なら、**`0` が素通し**に
なる。PID 値ではないので §45 の掃きでは触れていない。

`{dev1, 0}` で流してみたが、**PID フィルタ側のリング（phys `0x0c000000`）は
`0xff` のままだった**。ただし**この走行では `nr=0x48` の戻り値を取り落とした**
（出力の切り出しを誤った）ので、**受理されたのかどうかが分かっていない**。
**やり直しが要る。**

> なおこの走行でもペイロード側（phys `0x0d000000`）には PES が入った
> （`00 00 01 bd … 21 0a 81 cd e1 …`＝前回と違うバイト列＝生きている）。
> §43 は安定して再現する。

### 46.2 ★ (B) `/dev/val_record` は生きている

**触っていなかったノード。** `MMAC_RECORD_*` は 10 本（§11.3）で、
§11.4 は「`SetPidFilterBuffer` ＋ 書き込みポインタと**同じ性質の第 2 の経路**」と
書いていた。**録画経路なら生 TS が出る筋**である。

**ioctl の型バイトは規則で出せた**: major 230 の `val_*` は
**型 = `0xC1` + minor**（`val_disp` 3→`0xc4`、`val_jpeg` 5→`0xc6`、
`val_osd` 6→`0xc7`、`val_videnc` 12→`0xcd`、`val_prcg` 15→`0xd0` が全部一致）。
**`val_record` は minor 9 ∴ 型 `0xCA`。**

ゼロ埋め 40 バイトで `nr=0x01`〜`0x0a` を叩いた結果:

```
nr=0x02  ret = 40     ← ★ val_merge 内で完結（§20.6）＝有効な呼び出し
それ以外 ret = -22    ← EINVAL＝nr は在るが引数が違う（-25/ENOTTY ではない）
```

∴ **`/dev/val_record` の ioctl 面は実在し、応答する。** `-22` が返るのは
「その `nr` は存在するが引数が違う」という意味で、**10 本すべてに入口がある。**

`val_demux` の `nr=0x02` が `OpenDevice`（`{1, ptr}`・dir=R）なので同じ形を試したが、
`0x4028ca02 {1, ptr}` は `-22`、`0x8028ca02 {1, ptr}` は `-1` で、**ゼロ埋めのときだけ
`40`** が返る。∴ **`nr=0x02` の引数の形が `val_demux` とは違う。** ここが次の課題。

### 46.3 次

1. **`nr=0x48 {dev, 0}` の戻り値をちゃんと採る**（今回の取り落とし）。受理されるなら
   「PID フィルタの出力をどこへ書くか」（`SetPidFilterBuffer`＝§17.4 で未同定）を
   併せて解く必要がある。
2. **`val_record` の `nr=0x02` の引数を当てる。** ゼロ埋めで `40`、`{1, ptr}` で
   `-22`。語数・語の並びを振って `ret=0`（＝裏 CPU まで行く）を探す。
   `ret=40` は「`val_merge` 内で完結」なので、**まだ裏 CPU に届いていない**。
3. DMA 迂回（§45.1 の道 1）は**この 2 つの後**。

## 47. 素通しのフラグは受理される。残るは「PID フィルタの出力先」（2026-09-22）

§46.3 の宿題を 2 つとも進めた。

### 47.1 ★ (A) `nr=0x48 {dev, 0}` は `ret=0`

§46.1 で取り落としていた戻り値を採った:

```
nr=0x48 {0x00010000, 0}   ret=0     ← デバイス 1・素通し側
nr=0x48 {0x02010002, 1}   ret=0     ← デバイス 2・ベンダと同じ
```

∴ **`nr=0x48` の第 2 引数は 0 でも 1 でも受理される。**
§11.4 の `ControlHardwarePidFiltering`（全 PID 素通し）だという読みと矛盾しない。

**ただし PID フィルタ側のリング（phys `0x0c000000`）は `0xff` のまま。**
素通しにしても**出力先が無い**からで、§17.4 で「未同定」としたまま残っている
**`SetPidFilterBuffer`** がここで効いてくる。

### 47.2 `SetPidFilterBuffer` の掃きは道具が落ちて未完

ペイロード側が `0x26`（SetPayloadBuffer）/ `0x29`（Reset）/ `0x2a`（WritePointer）
という並びなので、**未マップの `nr` に同じ形 `{filter, ring, size, descriptor}` を
投げて**探した（`0x06` は実機が落ちるので除外・§18.4）。

```
nr=0x03 0x04 0x07 0x08 0x09   →  -7（その nr は無い）
nr=0x0f                       →  ★ yb1-ioctl が Segmentation fault
```

**道具側の不具合で掃きが途中で止まっている。** 残り
（`0x10`〜`0x13` / `0x18` / `0x1b`〜`0x20` / `0x23`〜`0x25` / `0x27` / `0x28` /
`0x2c` / `0x2d` / `0x31` / `0x35` / `0x36`）は**未検証**。

### 47.3 (B) `val_record` の `nr=0x02` の形

単発で入出力の両方を見た:

| 引数 | ret | 読み返し |
|---|---|---|
| 全部ゼロ（W） | **40** | 変化なし |
| 第 1 バイト `0x01`〜`0x04`（W） | **-1** | 変化なし |
| 全部ゼロ（**R**） | -22 | — |

∴ **`nr=0x02` は「第 1 語が 0 の W」しか受け付けず、`ret=40` で
`val_merge` 内で完結し、何も書き返さない。** `val_demux` の `nr=0x02`
（`OpenDevice {1, ptr}`・dir=R）とは**別の形**である。
**引数の形はまだ当たっていない。**

### 47.4 次

1. **`yb1-ioctl` の `nr=0x0f` での segfault を直す。** 直さないと `SetPidFilterBuffer`
   の掃きが進まない。落ちたのは道具の側（ドライバは応答している）。
2. 掃きを最後まで通す。**`nr=0x48 {dev,0}` が通る以上、出力先さえ見つかれば
   「全 PID を DRAM リングへ」が成立する**＝ §4.1 の chardev 契約が満たせる。
   **これが今いちばん近い道。**
3. `val_record` は引数の形を当てる作業が残る。第 1 語が 0 固定で `ret=40` という
   のは「問い合わせ系」に見えるので、**後ろの語にポインタを置く**形を試す。

## 48. 未マップの `nr` を掃き切った＝`0x35` / `0x36` が出た（2026-09-22）

§47.2 で道具が落ちて止まっていた掃きを完走させた。

### 48.1 `nr=0x10` は道具を殺す（触らないこと）

`argbuf` が 256 バイトなのでドライバの書き過ぎで BSS を壊したのかと考え、
引数バッファを **8192 バイト**（`_IOC` が表せる最大）に広げたが、**症状は同じ**。
∴ **原因は我々のバッファではなく `nr=0x10` そのもの**で、**毎回 SIGSEGV で
プロセスが死ぬ**（2 回再現）。

**`val_demux` の触ってはいけない `nr`**:

| `nr` | 症状 |
|---|---|
| **`0x06`** | **実機が落ちる**（§18.4） |
| **`0x10`** | **呼んだプロセスが SIGSEGV で死ぬ**（本節） |

バッファを広げた変更自体は残す（他のノードでの書き過ぎに対する保険になる）。

### 48.2 ★ 掃きの結果

`{PID フィルタ, リング, 大きさ, ディスクリプタ}`（＝`nr=0x26` と同じ形）を
未マップの `nr` に投げた:

| `nr` | ret | 読み |
|---|---|---|
| `0x03` `0x04` `0x07` `0x08` `0x09` `0x0f` | `-7` | **その `nr` は無い** |
| `0x1e` `0x1f` `0x27` `0x28` `0x2c` `0x2d` | `-3` | **在るが、このハンドルの種類ではない** |
| **`0x35`** **`0x36`** | **0** | ★ **この形を受理する** |
| `0x10` | — | 触れない（§48.1） |
| `0x11`〜`0x13` `0x18` `0x1b`〜`0x1d` `0x20` `0x23`〜`0x25` `0x31` | `-7` | 無い |

∴ **`nr=0x35` / `nr=0x36` が `SetPidFilterBuffer`（§11.4・§17.4 で未同定）の
最有力候補。** どちらも **W 専用**（`0x36` を dir=R で呼ぶと `-7`）。

### 48.3 それでもリングは埋まらない

順番を正して試した:

```
nr=0x48 {dev1, 0}                       素通し           ret=0
nr=0x0b {7, …}                          PID フィルタ確保  ret=0
nr=0x35 {filt, ring, size, desc}        バッファを与える  ret=0
nr=0x36 {filt, ring, size, desc}                          ret=0
nr=0x0c {filt, 0x0130}                  PID              ret=0
nr=0x0d {filt, 1}                       有効化           ret=0
nr=0x6a {0} ×2                          蹴り             ret=0
```

**リング（phys `0x0e000000`）は `0xff` のまま。** `nr=0x2f` で書き込みポインタを
読もうとすると `-3`（このハンドルの種類ではない）。

∴ **`0x35`/`0x36` は形こそ受理するが、これだけでは流れない。**
ペイロード側が `0x26`（バッファ）→ `0x29`（リセット）→ `0x0c` → `0x0d` だったように、
**この経路にも「リセット」か「開始」に当たる 1 本がまだ要る**とみるのが自然。
バッファオブジェクト側の `nr=0x2e`（開始）に当たるものが、PID フィルタ経路にも
あるはず。

### 48.4 次

1. **`0x35`/`0x36` の直後に効く「開始」を探す。** `-3` を返した
   `0x1e` `0x1f` `0x27` `0x28` `0x2c` `0x2d` は**在るが引数の種類が違う**ので、
   **PID フィルタのハンドルではなく別の引数**（バッファ id、あるいは `0x35` が
   返す何か）で呼べば通る可能性がある。まず `{filt}` 1 語だけの形で掃き直す。
2. `0x35` と `0x36` のどちらが set でどちらが別物かを切り分ける（片方だけ呼ぶ）。
3. `val_record`（§47.3）は並行して残っている。

---

**運用の手順は [../poe-br100-yb1-rig.md](../poe-br100-yb1-rig.md) に分けた**
（リグの切り替え・電源の入れ直し・ファイルの授受・トレースの採り方・実機に置いて
ある道具）。段階 C の定型列は
[tools/yb1-ioctl/stage-c-demux.script](../../tools/yb1-ioctl/stage-c-demux.script)。

## 49. ★ ioctl の名前が機械的に決まった＝`val_pvr_trick` が録画エンジンだった（2026-09-23）

§48.4 の宿題を §14 の手法（**ioctl を 1 本ずつ出して、エントリ表の差分を見る**）で
やり直した。結果、**呼び出しの意味を推測ではなく機械的に決める道**が open した。

### 49.1 掃き方に穴があった＝**方向（dir）を全部見ていなかった**

§46/§48 の掃きは **`_IOW`（dir=4）と `_IOR`（dir=2）だけ**で、`_IOWR`（dir=6）と
引数なしの `_IO`（dir=1・size=0）を見ていなかった。**これが致命的だった**:
後述のとおり、**値を返す呼び出しはほぼ全部 `_IOWR`** で、**初期化は `_IO`** である。

`_IOC_SIZE` も掃いた（0〜128）。**`val_*` は 40 以外を全部 `-22` で弾く**＝
§11.8 の「引数は例外なく 40 バイト固定」はドライバ側の検査で強制されている。

なお **`-7` は「その nr は無い」**（`dmesg` に `val_demux ioctl : unknown request`
が出る）、**`-22` は「その方向・大きさでは受け付けない」**で、意味が違う。

### 49.2 ★ `nr=0x35` / `nr=0x36` は 1 語しか送らない＝バッファ設定ではありえない

エントリ表の差分（§12.2 の語 1 ＝ `(コマンド ID << 16) | 連番`、語 2/3 ＝
リクエスト/レスポンスの番地、語 6 ＝ 合計長）:

| ioctl | コマンド | リクエスト長 | レスポンス |
|---|---|---|---|
| `nr=0x26`（既知・対照） | `0x1929` | 12 B | 4 B |
| `nr=0x29`（既知・対照） | `0x192c` | 8 B | 無し |
| `nr=0x2f`（既知・対照） | `0x1932` ＋ `0x191b` | 8 B / 4 B | 無し / 4 B |
| **`nr=0x35`** | **`0x1937`** | **4 B（1 語）** | **無し** |
| **`nr=0x36`** | **`0x1938`** | **4 B（1 語）** | **無し** |

対照 3 本は §14.1 の値を**バイト単位で再現**したので、手法は効いている。

∴ **`0x35`/`0x36` は「ハンドル 1 個だけ」を裏 CPU に渡す呼び出し**である。
`SetPidFilterBuffer` は番地と大きさを運ばなければならない（対照の `nr=0x26` が
12 バイト運んでいる）から、**`0x35`/`0x36` はバッファ設定ではありえない。**
§48.2 の「`{フィルタ, リング, 大きさ, ディスクリプタ}` の形を受理する」は、
**先頭 1 語しか読まれていなかった**だけだった。

さらに実機で確かめた:

```
nr=0x35 {PID フィルタ}      ret=0      nr=0x36 {PID フィルタ}      ret=0
nr=0x35 {バッファオブジェクト} ret=-3   nr=0x36 {バッファオブジェクト} ret=-3
```

∴ **引数は PID フィルタのハンドル 1 語**。DEMUX の関数表で「PID フィルタを 1 語
取る」ものは `EnablePidNearlyFullPauseState` / `DisablePidNearlyFullPauseState` /
`ClearPidNearlyFullPauseState` / `GetPidNearlyFullPauseState` の 4 本しかない
（§49.5 の名前表）。**満杯手前の流量制御**という読みと矛盾しない。

### 49.3 PID 値は原因ではなかった

「PID フィルタ側が空なのは PID `0x0000` を与えているからでは」を潰した。
ペイロードフィルタを**一切確保せず**（§19.4 の排他を避けて）、PID フィルタに
**§43 でデータが取れたのと同じ `0x0130`** を与えた:

```
書き込みポインタ（nr=0x2f）  0 のまま（4 秒・8 秒・15 秒後とも）
リング phys 0x0c000000        0xff のまま
裏 CPU のイベント             0 件
```

∴ **PID 値は関係ない。** PID フィルタ経路はまだどこかで止まっている。

### 49.4 ★ `/dev/val_record` は裏 CPU の**ブロック `0x1c`**

開いた fd を保ったまま（`_IO` の効果が per-open のため）エントリ表を差分した:

| ioctl | dir | ret | コマンド |
|---|---|---|---|
| `nr=0x01` | `_IO`（引数なし） | 0 | **`0x1c00`** |
| `nr=0x02` | `_IOW` | 0 | 無し（`val_merge` 内で完結） |
| `nr=0x03` | `_IOWR` | 40 | 無し |
| `nr=0x04`〜`0x08` | `_IOR` | −1 → −3 | **`0x1c03`〜`0x1c07`** |
| `nr=0x09` | `_IO`（引数なし） | 0 / 1 | **`0x1c08`** |
| `nr=0x0a` | `_IOWR` | 40 | 無し |

- **カテゴリは `0x1c`**。`0x18`(dcnv)・`0x19`(demux)・`0x2b`(m2dsc) に続く 4 つ目。
- **コマンド ＝ ioctl nr − 1**（例外なし）。
- **`nr=0x01` は `/dev/val_demux` の `nr=0x01`（Initialise）を先に呼んでいないと
  `-10` を返す**＝ブロック間に初期化の順序がある。

### 49.5 ★★ **ioctl nr ＝ そのブロックの関数表の 1 始まりの位置**（カーネルが名前を教えた）

§15.1 で「番号による同定は打ち切る」としたが、**打ち切る必要はなかった**。

`val_merge.ko` の節名（`readelf -SW`、`objdump -h` と同じ観察）を並べると、
**ファイル順はソース定義順の逆**（§15.1）。そこから起こした関数表の位置と、
実機の ioctl nr が一致することを、**カーネル自身の Oops メッセージ**で 2 ブロック
独立に確認した。引数の語にポインタでない値を置くとドライバがそこを参照して落ち、
**`dmesg` が関数名を解決して印字する**:

```
epc : MMAC_RECORD_GetCapabilities+0x90  <- record_ioctl+0x1fc      nr=0x02
epc : MMAC_PVR_RecordSetConfiguration+0xb8 <- pvr_trick_ioctl+0x464 nr=0x18
```

- `MMAC_RECORD_GetCapabilities` は RECORD の **2 番目**＝`nr=0x02` ✓
- `MMAC_PVR_RecordSetConfiguration` は PVR の **24 番目**＝`nr=0x18` ✓

**成立条件**: そのノードが応答する nr の本数が、ブロックの関数の本数とぴったり
同じであること。RECORD は `0x01`〜`0x0a` の 10 本＝関数 10 本、PVR は `0x01`〜`0x1b`
の 27 本＝関数 27 本で、**どちらも一致する**。

**DEMUX には当てはまらない。** 応答する nr は `0x64` `0x65` `0x6a` `0xc9` まで飛ぶ
＝ 71 本より多く、`nr=0x0b` は第 1 語の種別で払い出す物を変える（6 でペイロード、
7 で PID）＝ **`val_merge` が Allocate 系をまとめている**。∴ DEMUX の名前は
**引数の形と挙動から決める**しかない（§15.3／§49.2 がその方法）。

### 49.6 ★★ `/dev/val_pvr_trick` ＝ PVR ブロック＝**録画エンジン**

ノードの一覧を取り直した（major 230・minor 0〜21 の 22 本）。型バイト ＝ `0xC1`+minor
（§46.2）なので、**minor 14 の `/dev/val_pvr_trick` は型 `0xCF`**。ここが
**27 本の PVR ブロック**である（`pvr_trick_ioctl` という名前も Oops が出した）。

PVR の関数表（ソース順）と、実機が返した方向・戻り値:

| nr | 関数 | dir | ret（引数ゼロ） |
|---|---|---|---|
| `0x01` | `Initialise` | **`_IO`** | 0（2 回目は −0x10007） |
| `0x02` | `PlaybackOpenDevice` | `_IOR` | 0 |
| `0x03` | `PlaybackCloseDevice` | `_IOR` | 0 |
| `0x04` | `PlaybackSetConfiguration` | `_IOR` | 40 |
| `0x05`〜`0x0f` | `Playback*`（再生・早送り・巻戻し・一時停止…） | `_IOR` | −0x10004 / −0x10002 |
| `0x10` | `PlaybackGetNextStreamDataAddress` | **`_IOWR`** | 40 |
| `0x11` | `PlaybackDataSegmentFilled` | `_IOR` | 40 |
| `0x12` `0x13` | `Playback(Reverse)Skip` | `_IOR` | −0x10004 |
| `0x14` `0x15` | `PlaybackGetDisplayingTime` / `GetTotalTime` | **`_IOWR`** | 40 |
| **`0x16`** | **`RecordOpenDevice`** | **`_IOWR`** | 40 |
| **`0x17`** | **`RecordCloseDevice`** | `_IOR` | −0x10004 |
| **`0x18`** | **`RecordSetConfiguration`** | `_IOR` | 40（ゼロ）／−0x10002（ポインタ） |
| **`0x19`** | **`RecordStart`** | `_IOR` | −0x10004 |
| **`0x1a`** | **`RecordStop`** | `_IOR` | −0x10004 |
| **`0x1b`** | **`RecordSetAdditionalPidFilter`** | **`_IO`** | 0 |

**方向は意味とぴったり対応している**＝**値を返す呼び出し（`Get*` / `*OpenDevice`）が
`_IOWR`、引数を渡すだけの呼び出しが `_IOR`、何も取らない 2 本が `_IO`**。
27 本の並びと 27 個の nr が 1 対 1 で、方向まで意味と合う。**名前の同定は堅い。**

戻り値は errno ではなく **AV 層の番号**（`-0x10001` `-0x10002` `-0x10004`
`-0x10007`）で、**`40` は `val_merge` 内で完結**（§14.2）。
**PVR のどの呼び出しもエントリ表を動かさない**＝まだ裏 CPU に届いておらず、
`val_merge` の状態機械で止められている。

### 49.7 `RecordSetConfiguration` の引数は**構造体へのポインタ**

`nr=0x18` に `{0, 0x00020000}` を渡したときの Oops が `BadVA = 0x00020000`
＝ **第 2 語をそのまま参照した**。∴ 形は `RecordSetConfiguration(x, *config)`。

第 2 語に**有効な KSEG1 番地**（ゼロ済みの領域、あるいは `{リング, 大きさ}` を
置いた領域）を与えると、戻り値が **`40`（短絡）から `-0x10002` に変わる**
＝ **構造体を実際に読みに行っている**。**中身のレイアウトが次の関門。**

`RecordOpenDevice`（`nr=0x16`・`_IOWR`）は `{0, ptr}` `{1, ptr}` `{2, ptr}`
`{3, ptr}` `{デバイス, ptr}` `{ptr}` のどれでも `40` を返し、**ゼロ済みの
ポインタ先に何も書かない**。ハンドルは `val_merge` 内に持たれているとみられる。

### 49.8 MMAC ブロックの数え直し（§11.3 の更新）

節名を数え直したら §11.3 に**載っていないブロックが 5 つ**あった。全 28 ブロック:

| ブロック | 本数 | | ブロック | 本数 | | ブロック | 本数 |
|---|---|---|---|---|---|---|---|
| VIDEO | 85 | | OSD | 31 | | DMA | 12 |
| DEMUX | 71 | | **PVR** | **27** | | DCNV | 11 |
| AUDIO | 63 | | DEMOD | 22 | | RECORD / SCRAM / SCARD / ELU / DSCRAM | 各 10 |
| SCALER | 47 | | PCMCIA | 18 | | M2DSC | 8 |
| VENC | 44 | | DISP | 16 | | **CCAPTION** | **7** |
| PQC | 32 | | BITBLT / CAPTURE | 各 15 | | **COPYPROTECT** | **6** |
| | | | PLAYBACK | 12 | | **DEEPC** / **TELETEXT** / **PRCG** | 各 5 |

### 49.9 触ってはいけないものの追加

| ノード | 呼び方 | 症状 |
|---|---|---|
| `val_record` | `nr=0x02` の第 1 語にポインタ以外 | カーネル Oops → 呼んだプロセスが死ぬ |
| `val_pvr_trick` | `nr=0x18` の第 2 語にポインタ以外 | 同上 |

どちらも**ボードは生きたまま**で、電源を入れ直せば元に戻る（`val_demux` の
`nr=0x06` とは違う）。**なお `dmesg` は CSD の吸い出しループが持っていくので、
Oops は `/tmp/csd.log` を見ること。**

### 49.10 次

1. **`RecordSetConfiguration` の構造体レイアウト。** 第 2 語のポインタ先を掃いて
   `-0x10002` が消える形を探す。**リングの番地と大きさが入るのはここ**とみてよい。
2. `RecordOpenDevice` が何を返しているのか（`40` を返すが何も書かない）。
   `RecordStart` は第 1 語が `0` で `-0x10004`、`1` で `-0x10001` と**区別する**ので、
   第 1 語はハンドルである。
3. **PID フィルタ経路（§48）と録画経路（本節）は別々に追える。** 前者は
   `0x35`/`0x36` が流量制御だと分かった以上、**`SetPidFilterBuffer` に当たる
   ioctl がまだ見つかっていない**（`nr=0x2b` がバッファオブジェクトに対する
   同じ形なので、PID フィルタ側にも対応する 1 本があるはず）。
4. 他のノードにも §49.5 の規則を当てる（`val_playback` = PLAYBACK 12 本、
   `val_dscram` = DSCRAM 10 本など、**本数が一致するブロックは全部名前が付く**）。

## 50. `nr=0x35`〜`0x38` は「満杯手前の一時停止」4 本組だった（2026-09-23）

§49.1 で見つけた「方向を全部見ていなかった」を `val_demux` にも当て、ついでに
**nr の全域（`0x00`〜`0xff`）**を掃き切った。

### 50.1 `val_demux` に `_IOWR` も `_IO` も無い

`_IOWR`（dir=6・size 40）と引数なしの `_IO`（dir=1・size 0）を `0x00`〜`0xff`
（`0x06`/`0x10` を除く）で掃いた結果、**508 本すべて `-7`**。

∴ **`val_demux` の ABI は `_IOW` と `_IOR` の 40 バイトだけ**。
`val_record`/`val_pvr_trick` と違い、この節点は値を返す呼び出しも `_IOR` で書く。

### 50.2 ★ `0x37`〜`0x3b` は在った（§48.2 は `0x36` で止まっていた）

§48.2 の掃きは `0x36` までで、**その上は `0x47` `0x48` `0x64` `0x65` `0x67`
`0x6a` `0xc9`（ベンダのトレースに出たもの）しか叩いていなかった**。掃き直すと:

| `nr` | ret | コマンド | リクエスト |
|---|---|---|---|
| **`0x37`** | −2 | **`0x1939`** | **4 B（1 語）** |
| **`0x38`** | 40 | 無し（`val_merge` 内で完結） | — |
| **`0x39`** | −2 | **`0x193b`** | 8 B |
| **`0x3a`** | −2 | **`0x193e`** | 8 B |
| **`0x3b`** | −2 | **`0x193d`** | 8 B |

`0x3c`〜`0xff` は（`0x47` `0x48` `0x64` `0x65` `0x67` `0x6a` `0xc9` を除いて）
全部 `-7`。**これで `val_demux` の nr 空間は全部見た。**

### 50.3 ★ 4 本組が揃った

PID フィルタのハンドルを渡すと:

```
nr=0x35 {PID フィルタ}  ret=0    コマンド 0x1937  1 語・応答なし
nr=0x36 {PID フィルタ}  ret=0    コマンド 0x1938  1 語・応答なし
nr=0x37 {PID フィルタ}  ret=0    コマンド 0x1939  1 語・応答なし
nr=0x38 {PID フィルタ}  ret=40   コマンドを生まない（＝問い合わせ）
```

**連番のコマンド 3 本＋問い合わせ 1 本**で、**どれも引数は PID フィルタのハンドル
1 語だけ**（バッファオブジェクトを渡すと `-3`）。DEMUX の関数表でこの形に当たる
族は **`EnablePidNearlyFullPauseState` / `DisablePidNearlyFullPauseState` /
`ClearPidNearlyFullPauseState` / `GetPidNearlyFullPauseState`** しかない。
**問い合わせだけが `val_merge` 内で完結する**のは、§14.2 の「状態の読み出しは
共有メモリから答える」（STC の `nr=0x21`/`0x22` と同じ）とも噛み合う。

∴ **`0x35`〜`0x38` ＝ 満杯手前の一時停止の制御 4 本**と読むのが自然で、
**`SetPidFilterBuffer` ではない**（§49.2）。

### 50.4 一時停止を解除しても流れない

「PID フィルタが最初から一時停止側に居るなら、`Clear` が効くはず」を試した。
§43 と同じ 557 MHz・PID `0x0130`、有効化の前と後の両方で `0x37` を呼び、
`0x36`→`0x37` の順も試した:

```
書き込みポインタ（nr=0x2f）  0 のまま（3 秒・6 秒・10 秒後とも）
リング phys 0x0c000000        0xff のまま
```

∴ **一時停止は原因ではない。** §49.3（PID 値も原因ではない）と合わせて、
**PID フィルタ経路に足りないのは「出力先の結び付け」1 点**に戻る。
`nr=0x2b` がバッファオブジェクトに番地と大きさを与える 12 バイトの呼び出しで、
`nr=0x16` が PID フィルタをバッファオブジェクトに結ぶ。**この 2 本が通っていて
なお流れない**以上、まだ見えていない前提がある。

`0x39`/`0x3a`/`0x3b` は PID フィルタでもバッファオブジェクトでも `-3`
＝**第 3 のハンドル種別**（関数表の並びからはストリーム ID フィルタか
スクランブルフィルタ）を取る。

### 50.5 運用メモ

`nr=0x6a`（蹴り）を出したあとに未知の nr を続けて掃くと、一度だけ道具が
`Segmentation fault` で落ち、以後その節点への ioctl が全部無応答になった。
**冷起動からなら `0x6b`〜`0xc7` は全部 `-7` で無害**。掃きは冷起動から始め、
`0x6a` を出したあとは掃かないこと。

### 50.6 次

1. **PID フィルタの出力先。** `SetPidFilterBuffer` に当たる ioctl は
   `0x35`〜`0x38` ではなかった。`nr=0x24`（`_IOR`・コマンド `0x1926`・8 バイト
   ＝2 語）は **PID フィルタでもバッファでも `-3`** なので、**2 語の組み合わせが
   違う**（デバイス＋何か、など）。ここを詰める。
2. `RecordSetConfiguration`（§49.7）の構造体。`RecordOpenDevice` が
   ハンドルを返さないので、**PVR の録画経路はこの製品では塞がれている**可能性も
   ある。`GetCapabilities`（`val_record` `nr=0x02`）が返す語
   （`0x802cad74`・冷起動を跨いで同じ）の指す先を読むのが先。
3. §49.5 の規則を他の節点に当てる（本数が一致するブロックは全部名前が付く）。

## 51. 録画エンジンは 2 つとも「デバイスハンドル」で塞がれている（2026-09-23）

§49.7 の Oops/BadVA 法を**引数の語ごと**に回して、RECORD と PVR の語レイアウトを
決めに行った。**`ret=40` は経路封鎖ではなく `val_merge` の短絡**という読みは
正しかったが、**リングが OpenDevice の語に埋め込まれている、は否定**された。

### 51.1 採り方＝語を 1 つずつ「参照したら落ちる値」にする

40 バイトの引数の**ある 1 語だけ**を `0x00001000`（4 バイト境界に乗った未マップの
番地）にし、残りをゼロにして 1 本ずつ発行する。ドライバがその語を参照すれば

```
Unable to handle kernel paging request at virtual address 00001000
epc : MMAC_<BLOCK>_<関数名>+0x..  BadVA : 00001000
```

が出て**関数名と、構造体内で読んだ位置（BadVA の下位）まで分かる**。
Oops は呼んだプロセスを殺すだけでボードは生きるので、**1 プローブ 1 プロセス**で
回せる。RECORD の `_IO(0xca,0x01)`（Initialise）は**1 冷起動に 1 回だけ成功し、
以後 `-2`＝状態は per-open ではなく全体**なので、後続のプロセスはその状態を継ぐ。

対照として `val_record` `nr=0x02` の語 0 は毎回 Oops し、`MMAC_RECORD_GetCapabilities`
を印字した＝手法が効いていることの確認。

### 51.2 ★ どの語もポインタではなく、**検査される語はちょうど 1 つ**

| 呼び出し | 参照される語 | 検査される語 | ゼロのとき | 別の値のとき |
|---|---|---|---|---|
| `val_record` `nr=0x03` OpenDevice | **無し** | **語 4** | **40**（短絡） | **−3** |
| `val_record` `nr=0x0a` GetPacketCount | 無し | **語 1** | 40 | −3 |
| `val_record` `nr=0x04`〜`0x08` | 無し | **語 0** | −3 | −3 |
| `val_pvr_trick` `nr=0x16` RecordOpenDevice | 無し | **1 つも無い** | 40 | **40** |
| `val_pvr_trick` `nr=0x19` RecordStart | 無し | **語 0** | −0x10004 | −0x10001 |

**∴ `ret=40` は「検査する語がゼロだったので何もせず返した」＝短絡**である
（§49 で「封鎖」と書きかけたのは誤り）。**同時に、ポインタの語が 1 つも無い**以上、
**RECORD はリングを OpenDevice の語では受け取らない。** RECORD に `SetBuffer` が
無いのは確かだが、**代わりに引数へ埋め込む形にもなっていない。**

### 51.3 ★ 塞がっているのは「デバイスハンドル」

`nr=0x03` の**語 4** に、作れるハンドルを全部与えたが**全部 `-3`**:

```
0x00010000 / 0x02010002 / 0x01010001   デバイス 1 / 2 / 3
0x00100023 / 0x0210006b               PID フィルタ（デバイス 1 / 2）
0x00080022 / 0x0208006a               ペイロードフィルタ（デバイス 1 / 2）
0x00000100                            バッファオブジェクト
0x00002002                            nr=0x30 が返すもの
0x00010000〜0x00800000                種別バイトの掃き出し
1 / 2 / 3 / 4 / 8 / 0x10 / 0x20 / 0x28 / 0x40 / 0x80 / 0x100 …
```

`nr=0x07`（Start）も同じ 21 通りで**一様に `-3`**、PVR の `RecordStart` も
**一様に `-0x10001`**。**どこにも有効なハンドルが無い。**

そして **PVR の `RecordOpenDevice` は引数を 1 語も見ない**＝**何を開くか伝える
手段が無く、ハンドルも返さない**。**録画系はビルドには入っているが、この製品では
配線されていない**と読むのが素直である（本機は地デジ単チューナで HDD 給電も
B-CAS スロットも無い・§7）。

> 唯一 `0` を返す PVR の呼び出しが **`PlaybackOpenDevice`（`nr=0x02`）** と
> **`PlaybackCloseDevice`（`nr=0x03`）**である。ここだけ生きている可能性があり、
> 次に当たる価値がある。

### 51.4 ∴ 生 TS は PID フィルタ経路に戻る

§50.4 の結論（足りないのは「出力先の結び付け」1 点）に戻る。録画側から回り込む
道は、**ハンドルを作る呼び出しが見つかるまで進めない。**

### 51.5 次

1. **PVR `PlaybackOpenDevice`（`nr=0x02`・`_IOR`・`ret=0`）**が何を返すか。
   §51.2 の語ごとの検査法をそのまま当てる。ここがハンドルの出所なら、
   `RecordStart` の語 0 に入れるものが手に入る。
2. PID フィルタの出力先（§50.6 の 1）。`nr=0x24` の 2 語の組み合わせ。
3. §49.5 の名前規則を他ノードへ。

## 52. 生 TS はデマルチプレクサの外＝トランスコーダ経路にある（2026-09-24）

「デマルチプレクサの配信口はセクション / PES / PCR / PTS だけ」「生 188 バイトが
実在するのは `0x0acea000` の 1 本だけ」という読みを、ベンダのトレースと実機の
両方で当たった。**前半は裏が取れ、後半は所有者が判明した。**

### 52.1 デマルチプレクサの DRAM 出口は 16 バイト単位＝TS ではない

ベンダの `val_demux` `nr=0x2f`（書き込みポインタのポーリング）**3,871 回**を
バッファオブジェクトごとに分解した:

| バッファ | ポーリング | オフセットの進み方 |
|---|---|---|
| `0x00000100` | 820 | `0x20` `0x30`（リング `0x3000`） |
| `0x00000102` | 784 | `0x d0` `0xe0`（リング `0xa000`） |
| `0x00000107` | 790 | `0xb0` `0xc0` |
| `0x0000010f` | 1,115 | `0x10`〜`0xe0`（リング `0x62000`） |
| `0x00000101` `0x103` `0x105` | 118 | `0x10` `0x20` `0x90` `0xa0` |

**進み幅は例外なく `0x10` の倍数で、`0xbc`（188）の倍数は 1 つも無い。**
∴ このリングは **16 バイト単位のレコード**を並べる口であって、TS パケットの
口ではない。ベンダがリングを置く番地（`0x094a4000` / `0x0a0c6000` / `0x0a1de000` /
`0x0a408000`〜`0x0a420000`）は §10.2 が**配信中に走査して「TS 無し」**とした
領域に入る。

> **残る穴 1 つ**: 種別 7 のリングの**生バイトは未ダンプ**である。我々のリングは
> 埋まらず（§50.4）、ベンダのリングは配信セッション中しか埋まらないため。
> 幅の証拠は強いが、バイトで確認したわけではない。

### 52.2 StreamOut はベンダが**一度も呼ばない**

起動〜選局〜配信 1 回の `val_demux` / `val_demux1` の ioctl を nr ごとに数えた
（33 種）。`OpenStreamOutDevice` / `SelectStreamOutGroup` / `StartStreamOut` に
当たる呼び出しは**1 本も無い**。∴ **`0x0acea000` を埋めているのは StreamOut では
ない。** ベンダが使う nr は `01 02 05 0a 0b 0c 0d 0e 14 15 16 17 19 1a 21 22 26 29
2a 2b 2e 2f 30 32 33 34 47 48 64 65 67 6a c9` の 33 種だけ。

### 52.3 ★ `0x0acea000` は 188 バイト × 1024 パケットだと、別経路からも裏が取れた

高位 KSEG1 番地を引数に含む ioctl を全部拾うと、**3 つしか無い**:

| ノード | nr | 番地 |
|---|---|---|
| `h57fmb0` | `0x1d` | `aacea000` `ab096000` `ab442000` |
| **`val_dcnv`** | **`0x07`** | `aac88000` `aac89000` `aac8a000` `aacea000` ほか 99 種 |
| `val_demux` | `0x2b` | `aa0c6000` `aa1de000` `aa408000`〜`aa420000` |

`val_dcnv` `nr=0x07` の応答:

```
{ 00000002, aacea000, aac8a007, 000000bc, 0002f000 }
                                   ↑188      ↑192512 = 188 × 1024
```

**パケット長 188、1024 パケット**が引数に書いてある＝§10.2 の走査とは独立に、
**あのリングが素の TS であることが数字で裏づいた。**

> **スコープの明記**: `DCNV` は Data Converter で、関数表（11 本）に
> `SetConvertKey` / `SelectConvertKey` / `SetKeyArea` / `SetDesKeyType` がある
> ＝**鍵を持つ DES 変換器**（録画の保護用とみられる）。**本節が使ったのは
> 番地と幾何（188 / 1024）だけ**で、鍵に関する呼び出しは叩かないし、
> 引数の形も書かない。限定受信（§0）とは別のものだが、境界に近いので明記する。

### 52.4 ★ 段階 C では AV 領域のどこにも TS が無い

冷起動 → 選局（557 MHz・ロック）→ 我々の列（§43 のペイロード経路）→ 10 秒待ち、
のあとで **AV 予約領域 112 MB 全部**（`0x09000000`〜`0x0fffffff`）を 16 MB ずつ
走査した:

```
0x09000000 / 0x0a000000 / 0x0b000000 / 0x0c000000 / 0x0d000000 / 0x0e000000 / 0x0f000000
  すべて TS sync 0 hits    （選局前も、列を流した後も）
0x0acea000 を名指しで 4 MB 走査しても 0 hits
```

∴ **あのリングはベンダのトランスコーダ経路が動いているときだけ埋まる。**

### 52.5 ★★ `/dev/h57fmb0` は開けるようになった

§38.3 は「Dione は USB デバイスで、`dione_transcode -w` が終わると切れる。
**欠けているゲートではなく再生の時機が違うだけ**」とした。**そのとおりだった。**

```
/app/bin/dione_transcode -t -P 1        （-w は付けない＝SPI-ROM は書かない）
  → lsusb に  Bus 001 Device 004: ID 06b8:102d
  → yb1-ioctl /dev/h57fmb0 0x400cae08  →  ret=0   out: {06b8, 102d, 3}
```

**段階 C のリグで、ベンダアプリ抜きのままトランスコーダのノードが開く。**
（`dione_transcode` はベンダの実行ファイルなので**観察の足場**であって段階 C の
部品ではない。USB デバイスなので、観察からクリーン実装できる余地がある——
SB-TV02 側の `h57fmb.ko` についての判断と同じ。）

ベンダの `h57fmb0` の使い方（`ioctl` 1,691 本の内訳）:

| nr | dir | 大きさ | 回数 | 最初の引数 |
|---|---|---|---|---|
| `0x1d` | W | 16 | 5 | `{index, 0x3ac000, 0xaacea000, 0x2abd6000}` ＝ DRAM ↔ Dione の番地対応 |
| `0x08` | R | 12 | 1 | `{0x06b8, 0x102d, 3}` ＝ USB の素性 |
| `0x04` | R | 46 | 1 | `"0.0.4.1"` ほかの版情報 |
| `0x01` | WR | 12 | 7 | `{0x00010000, 0, 0x00bf0000}` |
| `0x11`/`0x12` | R/W | 20 | 19/8 | `{0x80000004, 4, 0, 0, 0x2ae328cc}` ＝ レジスタ読み書き対 |
| `0x02` | R | 12 | **743** | `{1, 0x00080010, <ユーザ番地>}` ＝ Dione から読む |
| `0x03` | W | 12 | **726** | `{1, 0x00090070, <ユーザ番地>}` ＝ Dione へ書く |
| `0x1a` | WR | 12 | 181 | `{0x2ee07000, **0x0002f000**, 0}` ＝ **188 × 1024** と同じ大きさ |

### 52.6 ∴ ハード改造の前に、まだ道がある

- **デマルチプレクサから生 TS は出ない**（§52.1・§52.2）。§44.3 の道 2（PES から
  組み立て直す）も契約を満たさない。**ここは閉じたと見てよい。**
- **生 TS は存在し、所在も幾何も分かっている**（§52.3）。埋めているのは
  トランスコーダ経路で、**その入口 `/dev/h57fmb0` は我々の手で開けた**（§52.5）。
- ∴ 次は **`nr=0x1d` に我々の DRAM を渡して、TS がそこに落ちるか**を見る。
  落ちれば §4.1 の chardev 契約はトランスコーダ経路で満たせる。
  **デスクランブラ（`val_m2dsc` / `val_dscram`）は一度も設定しない**ので、
  得られるのはスクランブルされたままの TS になる（§10.3 の筋のまま）。

### 52.7 次

1. `/dev/h57fmb0` を開いたまま、`nr=0x08` → `0x04` → `0x01` → `0x1d`(我々の番地)
   の順に置き、`nr=0x02`/`0x03`/`0x1a` を回して、我々の番地を走査する。
2. `0x0acea000` を**誰が書いているか**。ベンダの `nr=0x1d` が Dione に教える
   `0x2a......` 側の番地空間と対で見る。
3. 種別 7 のリングの生バイト（§52.1 の残る穴）。ベンダの配信セッション中に
   `0x094a4000` を読めば取れる＝**PC クライアントでの視聴が要る**。

## 53. トランスコーダは我々の手で起動する。足りないのは「SoC の TS 出し」（2026-09-24）

§52.7 の 1（`/dev/h57fmb0` に我々の DRAM を渡す）を実施した。**ノードは完全に
動かせる**ところまで行き、**止まっている場所が 1 点に絞れた。**

### 53.1 ★ `dione_transcode` はベンダアプリ抜きで起動まで通る

段階 C のリグ（`blur` 無し）で、選局してから:

```
/app/bin/dione_transcode -t -i /dev/null -1 /tmp/out1.ts -2 /tmp/out2.ts -P 1
  [SUCCESS] start_device(),  status = 00000000, elapsed time = 1.99(s)
  [SUCCESS] StartStream(),   status = 00000000, elapsed time = 0.014(s)
  can't open /dev/tsout_demux
  [DONE]
```

**トランスコーダのファーム転送とストリーム開始が、ベンダアプリ無しで成功する。**
止まるのは **`/dev/tsout_demux`** で、このノードは

- **どのモジュールも作らない**（`/proc/devices` にも `/dev` にも無い）
- **`dione_transcode` の中にしか文字列が無い**（`blur` にも他の `.ko` にも無い）
- `/dev` は squashfs の読み取り専用なので**こちらで作ることもできない**

∴ **開発キットの口**で、この製品には最初から無い。`-t`（TS 入力ピンで入力を送る）
はそちら向けの経路である。

### 53.2 転送の内訳（ioctl トレース 2,828 本）

| nr | dir | 回数 | 中身 |
|---|---|---|---|
| `0x03` / `0x02` | W / R | 1,382 / 1,404 | **USB バルクでのファーム転送**（`start_device` の 2 秒） |
| `0x12` / `0x11` | W / R | 10 / 11 | レジスタ書き/読み `{0x80000004, 4, …}` |
| `0x01` | WR | 8 | コマンド/状態 |
| `0x1d` | W | 6 | **DRAM ↔ Dione の番地対応** |
| `0x04` | R | 2 | 版情報 |

ベンダの配信トレースの `0x02`×743 / `0x03`×726 も、**同じ形のファーム転送**と
みてよい（我々の 1,404/1,382 と同種で数が違うだけ）。**TS を運んでいるのは
`nr=0x1a`（181 回・毎回 `0x0002f000` ＝ 188 × 1024）**の方である。

### 53.3 ★ `0x0acea000` は `dione_transcode` 自身が選んでいる

我々の走行の `nr=0x1d` の引数は、**ベンダのトレースとバイトまで同じ**だった:

```
{0, 0x3ac000, aacea000, 2abd6000}
{1, 0x3ac000, ab096000, 2af82000}
{2, 0x3ac000, ab442000, 2b32e000}
```

∴ **§10.2 で TS を見つけたあの番地は、このツールが決めているトランスコーダの
バッファ**である（§10.7.4 の訂正が、独立に裏づいた）。**デマルチプレクサの
出口ではない。**

そして **`nr=0x1d` は我々の番地（`0xab000000`）も `ret=0` で受理する。**

### 53.4 ★ 止まっているのは「Dione に TS が届いていない」1 点

`StartStream()` が成功した**直後**に転送を頼んでも:

```
nr=0x1a {0x2abd6000, 0x0002f000, 0}   ret = -145   (ETIMEDOUT)
nr=0x1a {0x2af82000, 0x0002f000, 0}   ret = -145
nr=0x01 {0x00010000, 0, 0x00bf0000}   ret = -11    (EAGAIN)
        → 応答は {0x000100**80**, …}  ＝ 状態語に 0x80 が立つ
```

Dione は列挙されたまま（`lsusb` に `06b8:102d`）で、識別も版も読める。
∴ **チップは起きていて待っているが、TS 入力ピンに何も来ていない。**
DRAM 側（`0x0acea000` / `0x0b096000` / `0x0b442000` / 我々の `0x0b000000`）は
いずれも TS ゼロ、AV 予約領域 112 MB も 0 hits。

### 53.5 デマルチプレクサ側は、これで完全に否定された

ベンダがこの 1 セッションで確保するバッファオブジェクトは **33 個**:

```
0x100..0x10f  0x3000 / 0xa000 / 0x5000 / 0x50000 / 0x14000 / 0x28000 / 0x64000
0x110..0x11b  0x118000 （1,146,880 B）が 12 本
0x01000120..24  0x8000 が 5 本
```

**どれ 1 つとして 188 の倍数ではない。** 書き込みポインタも `0x10` の倍数でしか
進まない（§52.1）。∴ **デマルチプレクサに TS リングは存在しない。**

### 53.6 `io_controller` は GPIO であって、レジスタ窓ではない

一番回数の多い口（7,862 本）を解くと、**引数は 4 バイト 1 語**で、値は
`0x0c` `0x13`〜`0x17` `0xff` `0x01` といった**小さい整数**だけだった
（`nr=0x03` は `0x13`〜`0x16` を 920 回ずつ＝4 本のポーリング）。
∴ **ポート番号を渡す GPIO の口**で、TS 出しの有効化ビットはここには無い。

### 53.7 ∴ 残っているのは 1 つ＝**SoC の TS 出し**

- デマルチプレクサ … TS リング無し（§53.5）
- StreamOut … ベンダが一度も呼ばない（§52.2）
- `io_controller` … GPIO（§53.6）
- トランスコーダ … **我々の手で起動でき、DRAM も受け取る。入力待ちで止まる**（§53.4）

∴ **MXL683 の TS を Dione の入力ピンへ出す設定が、まだどこかにある。**
ベンダはそれを **`/dev/mem` のレジスタ書き込み**で行っているとみるのが自然で、
それは**トレーサの盲点そのもの**（§38.5）。§10.5 の
`BHIF ALTERNATE0 = 0x00084008`（TS 入力ピン設定）が最有力の窓である。

### 53.8 次

1. **盲点を埋める。** ベンダ起動（段階 B）と段階 C で、`BHIF`（`0x10000000`）と
   §10.7.2 で見つけた**名前だけの 16 MB `EMMA_GIO_BASE`（`0x11000000`）**を
   突き合わせる。§39〜§41 の窓差分は**デマルチプレクサ向け**に見ていたので、
   **今回は「Dione の入力ピンに TS が出るか」を判定基準にやり直す**
   （判定は `nr=0x1a` が `-145` から変わるかどうか＝一発で分かる）。
2. 種別 7 のリングの生バイトと、`0x0acea000` のスクランブル状態
   （§10.3 は「平文＝デスクランブラ下流」と読んだが、§53.3 でそこが
   トランスコーダのバッファと確定した以上、**トランスコーダ出力だった可能性**が
   ある）。どちらも**ベンダの配信セッション中の観察**が要る＝PC クライアント。

## 54. レジスタ説は正しい判定法で否定。そして `0x0acea000` の読みに疑いが出た（2026-09-24）

§53.8 の 1（レジスタ窓の盲点を、`nr=0x1a` を判定基準に潰す）を実施した。
**レジスタ説は否定**され、代わりに**もっと根の深い訂正**が必要になった。

### 54.1 ⚠ `0x11000000` の窓は**読むと実機が止まる**

§10.7.2 で「名前だけの 16 MB」とした `EMMA_GIO_BASE` を読みに行ったところ、
**`0x11110000` を 1 語読んだ時点でボードが応答しなくなった**（ログを 1 語ごとに
`sync` していたので、止まった位置が残った）。電源を入れ直せば戻る。

**∴ この窓は走査してはいけない。** レジスタ窓を総当たりで探る道は、ここで閉じる。

### 54.2 ★ TS 入力ピンの設定は、もう合っている

BHIF の窓を「何もしない／選局後／Dione 起動後」で 3 回撮って差分した:

```
選局後 → Dione 起動後 :
  0x10000028   00084000 -> 00084008      ← ★ EMMA_BHIF_ALTERNATE0
  0x10000414   782c0000 -> 785d0000
  0x10110d00 / 0x10110d18                 （タイマの計数）
```

**`0x10000028` は §10.5 が「TS 入力ピン設定」とし、§10.7.3 がベンダで
`0x00084008` と実測した語**である。**Dione を起こすだけで、我々の鑑でも
ベンダと同じ値になる。** ∴ **ピンの設定は欠けていない。**

### 54.3 レジスタ説は、正しい判定法で否定された

§39.2 の差分のうち BHIF 5 語をベンダの値に固定し（起動前に書き、最後まで保持
されることを確認）、XDMA の基底と長さも我々の AV メモリに向けて置いた:

```
0x10000024=00000000  0x10000110=000964a5  0x10000114=00000040
0x10000118=00003eee  0x100001e4=ffffffff
0x10009340=0b000000  0x10009344=0b000800  0x1000953c=00040000
（0x1000950c は書いても 0 のまま＝読み取り専用か、条件付き）
```

その状態で Dione を起こし、`ALTERNATE0 = 00084008` を確認してから:

```
nr=0x1a {0x2abd6000, 0x0002f000, 0}   ret = -145   （変化なし）
```

∴ **§40 の結論は判定法を変えても同じ**＝**SoC 側のレジスタは関門ではない。**

### 54.4 ★★ 時間の内訳が、`0x0acea000` の正体を疑わせる

ベンダの `h57fmb0` 1,691 本を**プロセスと時刻**で割ると、性格がはっきり分かれた:

| nr | pid | 回数 | 最初 | 最後 | 幅 |
|---|---|---|---|---|---|
| `0x1d` | 967 | 3 | 5.274 | 5.274 | 0 |
| `0x03`（書き） | 1537 | 726 | 143.969 | **147.995** | **4.0 s** |
| `0x02`（読み） | 1537 | 743 | 143.674 | 187.973 | 44.3 s |
| **`0x1a`** | **7860** | **181** | **147.186** | 187.957 | **40.8 s** |

- **`nr=0x03` は 4 秒で終わる**＝我々の走行の `start_device()`（1,382 本・2 秒）と
  同じ性格＝**ファーム転送**。配信中ずっと TS を押し込んでいるのではない。
- **`nr=0x1a` はファーム転送が終わった直後（147.2 s）に始まり、セッション終了まで
  40.8 秒間続く**。**別プロセス（pid 7860）**が回している。
- 量は **181 × 0x2f000 ＝ 34.8 MB / 40.8 s ＝ 約 6.8 Mbps**。

**地上デジタル 1 波の素の TS は約 17 Mbps** である。6.8 Mbps は**再エンコード後の
配信ストリーム**の大きさに近い。∴ **`nr=0x1a` が運んでいるのは
トランスコーダの「出力」**で、**`0x0acea000` に見えていた 188 バイトの TS は
再多重された配信用ストリームだった可能性が高い。**

§10.3 が「全パケット平文＝デスクランブラの下流」と読んだのも、
**そもそもスクランブルされる前の放送 TS ではなかった**なら素直に説明がつく。

### 54.5 ∴ §45.1 の「最後の道」は前提から見直しが要る

§45.1 は「生 TS は `0x0acea000` に実在する。デスクランブラを設定しなければ
同じ経路にスクランブルされたままの TS が入る」という筋だった。
**§53.3（あの番地は `dione_transcode` が選ぶトランスコーダのバッファ）と
本節（運ばれているのは 6.8 Mbps ＝ 出力側）を合わせると、この前提は怪しい。**

**確定させるには、ベンダの配信セッション中の観察が要る**（PC クライアント）:

| 見るもの | 判定 |
|---|---|
| `0x0acea000` の `transport_scrambling_control` | 全部 0 なら「素の放送 TS ではない」側の証拠 |
| 同じリングの**ビットレート**（書き込みポインタの進み方） | 17 Mbps なら素の TS、7 Mbps 前後なら出力 |
| PID の集合と、選局中のサービスの PMT との一致 | 映像 PID が違えば再多重 |
| 種別 7 のリング（`0x094a4000`）の生バイト | §52.1 の残る穴 |

### 54.6 いま言えること

| 経路 | 状態 |
|---|---|
| デマルチプレクサのフィルタ | **生 TS 無し**（§52.1・§53.5・33 個のバッファが全部 188 の倍数でない） |
| StreamOut | **ベンダが呼ばない**（§52.2） |
| `io_controller` | **GPIO**（§53.6） |
| SoC のレジスタ | **関門ではない**（§54.3。`0x11000000` は読むと止まる・§54.1） |
| RECORD / PVR | **ハンドルを作れない**（§51） |
| トランスコーダ | **起動でき、DRAM も受け取る。入力待ち**（§53.4） |

**∴ この製品の DRAM には、素の放送 TS が現れる場所が今のところ 1 つも無い。**
§4.1 の chardev 契約をこの基板で満たせるかは、**§54.5 の観察の結果次第**である。

## 55. ★★ 決着：`0x0acea000` はトランスコーダの再多重出力だった（2026-09-24）

§54.5 の観察を、**ベンダの配信セッション中**に実施した（段階 B・`blur` 稼働・
PC クライアントで視聴）。採取は **`/dev/mem` の読み取りだけ**で、ioctl は 1 本も
出していない（出すと `blur` のセッションが壊れる・§14.3）。

> 採取は「リングに TS が現れたら走る」見張りを仕掛けて行った。§36.1 の
> 「採取ループが先に終わっていた」失敗を繰り返さないため。

### 55.1 ★ 種別 7 のリングの中身＝**PSI/SI セクション**（§52.1 の穴が埋まった）

ベンダのバッファオブジェクトのリングを生バイトで読んだ。**どれも TS ではなく、
テーブルがそのまま並んでいた**:

| 物理 | 先頭バイト | 正体 |
|---|---|---|
| `0x094a4000` | `00 b0 1d 7f e0 cb 00 00 …` | **PAT**（`table_id=0x00`） |
| `0x094a7000` | `01 b0 10 ff ff c5 00 00 …` | **CAT**（`0x01`） |
| `0x094aa000` | `02 b0 d7 04 00 e5 00 00 …` | **PMT**（`0x02`） |
| `0x094b4000` | `40 f0 8c 7f e0 c5 00 00 …` | **NIT**（`0x40`） |

**`TS sync : 0 hits` がすべてのリングで出る。** ∴ §52.1 の
「進み幅が `0x10` の倍数だけ＝16 バイト単位のレコード」は、**セクションの
詰め方**だった。**デマルチプレクサの DRAM 出口はセクションと PES であって、
TS ではない**が、これで**バイトで確認**できた。

### 55.2 ★★ 選局中サービスの PMT と、リングの PID を突き合わせた

`0x094aa000` の PMT を解くと（program `0x0400`・PCR `0x01ff`）:

```
type=0x02 PID=0x0100   MPEG-2 映像
type=0x0f PID=0x0110   AAC 音声（主）
type=0x0f PID=0x0111   AAC 音声（副）
type=0x06 PID=0x0130   字幕     type=0x06 PID=0x0138   文字スーパー
type=0x0d PID=0x0140 / 0x0160 / 0x0161 / 0x0162 / 0x0170 / 0x0171 / 0x0172   データ
```

同じ瞬間の `0x0acea000`（3.85 MB・20,178 パケット・`desync 302`）の PID:

```
0x0000(PAT)  0x0002  0x0003  0x0012(EIT)  0x0111  0x0130  0x0138
0x01f0(PMT)  0x01ff(PCR)  0x1fff(null)
scrambling_control: none=20178  reserved=0  even=0  odd=0
```

**照合の結果:**

| PMT の PID | リングに | 読み |
|---|---|---|
| **`0x0100` 映像** | **無い** | **差し替えられた** |
| **`0x0110` 主音声** | **無い** | **差し替えられた** |
| — | **`0x0002` `0x0003` が在る**（PMT に無い） | **再エンコード後の映像と音声** |
| `0x0111` 副音声 / `0x0130` / `0x0138` / `0x01ff` | 在る | 素通し |
| `0x0140` `0x016x` `0x017x` データ | **無い** | 落とされた |

∴ **`0x0acea000` は、放送 TS ではなく「トランスコーダが作った配信用の TS」**である。
映像と主音声だけが新しい PID に差し替わり、データ放送は落ち、PSI・字幕・PCR・
副音声は素通し——**まさに再多重の形**。§54.4 のビットレートの見立て（6.8 Mbps）と
完全に整合する。

### 55.3 ∴ §10.2 / §10.3 / §45.1 の前提は**取り消す**

- §10.2「TS リングは物理 `0x0acea000`」…… **位置は正しいが「放送 TS」ではない**。
- §10.3「ここはデスクランブラの下流。**デスクランブラを設定しなければ
  同じリングにスクランブルされたままの TS が入る**」…… **成り立たない。**
  平文だったのは**デスクランブル済みだから**ではなく、**再エンコードされた
  別のストリームだから**である。設定を省いても、この経路に放送 TS は来ない。
- §45.1「残る道は 1 つ＝この DRAM リングへ書く段を我々の番地に向ける」……
  **その段は存在しない。**

**スコープ上の確認**: この出力はベンダが CAS で解除したあとの内容である。
**OpenIRD はこれを TS 源にしない**（§10.3 で既に宣言したとおりで、今回さらに
下流だと分かった）。今回読んだのは**リングの PID とヘッダ、および PSI/SI
（PAT/CAT/PMT/NIT ＝ 放送の公開シグナリング）だけ**で、
**我々は何も解除していない**（§44.4 と同じ立場）。

### 55.4 この基板の全体像（確定）

```
MXL683 →（シリアル TS）→ SoC TS 入力 →[デスクランブラ]→ デマルチプレクサ
                                                          ├→ セクション（PSI/SI）→ DRAM  §55.1
                                                          └→ ペイロード（PES）  → DRAM  §43
                                        └→（TS ピン）→ Dione トランスコーダ
                                                          └→ 再多重 TS → DRAM  §55.2
```

**DRAM に「素の放送 TS」が現れる場所は、この製品には無い。**

| 経路 | 出るもの |
|---|---|
| ペイロードフィルタ（種別 6） | **PES**（§43・実証済み） |
| PID フィルタ＋バッファオブジェクト（種別 7） | **PSI/SI セクション**（§55.1） |
| StreamOut | ベンダが呼ばない（§52.2） |
| RECORD / PVR | ハンドルを作れない（§51） |
| トランスコーダ | **再多重・CAS 解除後**（§55.2）＝**スコープ外** |

### 55.5 残っている可能性

1. **`ControlHardwarePidFiltering(dev, 0)`＝全 PID 素通しは受理される**（§47.1）。
   足りないのは**その出力を DRAM に結びつける 1 本**で、`MMAC_DEMUX` の関数表には
   `SetPidFilterBuffer` / `GetPidFilterBufferWritePointerOffset` が**確かに在る**
   （§11.4）。**ハードウェアにその機能はある。** ioctl として露出していない
   だけかもしれない（ベンダが使わないので）。
2. **ベンダの列でまだ写していない 1 本**: `nr=0x17` は `nr=0x16` と**同じ引数で
   対**で呼ばれている（各 39 回）。我々の列は `0x16` しか出していない。
3. それも駄目なら、**この基板では §4.1 の chardev 契約を満たせない**。
   §44.3 の道 3（契約の見直し）か、復調器の TS 出力を直接取る物理的手段になる。

## 56. `SetPidFilterBuffer` は ioctl として露出していない（2026-09-24）

§55.5 の 1 と 2 を潰した。

### 56.1 `nr=0x17` は**解除**だった（§55.5 の 2 は否定）

ベンダのトレースから、1 本の PID フィルタ（`0x0010001c`）とそのバッファ
（`0x105`）に触る呼び出しだけを時刻順に抜くと:

```
12.598  0x2b {buf, a94bc000, 3000, ptr}   リングを与える
12.599  0x15 {ptr, buf}
17.192  0x15 {ptr, buf}        ← もう一度
17.192  0x2e {buf}             開始
17.193  0x2f {buf} -> 2f00
17.193  0x16 {filt, buf}       ★ 結合
17.194  0x0c {filt, 0x0014}    PID（TDT/TOT）
17.194  0x0d {filt, 1}         有効化
21.942  0x2f {buf} -> 0
21.942  0x17 {filt, buf}       ★ 解除
21.943  0x0e {filt, 0}         無効化
21.943  0x2f {buf} -> 10       セクションが 1 つ入っていた
```

∴ **`nr=0x17` は `nr=0x16` の対で、`nr=0x0e`（DisablePidFilter）と一緒に呼ばれる
「解除」**である。**立ち上げに足りない 1 本ではなかった。**

### 56.2 ベンダの列をそのまま写しても、セクションは来ない

上の順番を**そのまま**、余計なもの（デバイス 2・3、ペイロードフィルタ、
`nr=0x30`/`0x33`/`0x34`）を一切作らずに実行した。PID は**ベンダと同じ `0x0014`**
（時刻表・約 5 秒に 1 回）。

```
0x01 0x02 0x05 0x65 0x65 0x64 0x64 0x0b{7} 0x14 0x1a 0x2b 0x15 0xc9 0x2e 0x2f
0x16 0x0c{0x0014} 0x0d          ← 全部 ret=0、ハンドルの種別もベンダと同じ
0x2f を 6 秒おきに 4 回          ← 24 秒間、書き込みポインタは 0 のまま
```

### 56.3 ★ `nr=0x26` は種別 7 のハンドルを受け付けない

ペイロード側は**フィルタが自分でリングを持つ**（`nr=0x26`）のに対し、セクション側は
**バッファオブジェクト経由**（`nr=0x2b` ＋ `nr=0x16`）である。この違いが効いて
いるなら、**種別 7 のフィルタに `nr=0x26` で直接リングを与えられれば
それが `SetPidFilterBuffer`**になる。同じ走行に**対照として §43 の
ペイロード経路**を置いて試した:

| 呼び出し | ハンドル | ret |
|---|---|---|
| `nr=0x19 {filt, ptr}` | `0x00100022`（種別 7） | **0** |
| **`nr=0x26 {filt, ring, size, desc}`** | `0x00100022` | **−3** |
| `nr=0x29 {filt}` | `0x00100022` | −3 |
| `nr=0x2a {filt}` | `0x00100022` | −3 |
| **対照: 同じ列のペイロードフィルタ** | `0x00080023` | **PES が落ちた** |

対照の `0x0d000000` には `00 00 01 bd 00 2e 80 81 … 43 49 53 … 6a 70 6e` が入った
＝**この走行でリグは生きていた**。∴ **`nr=0x26` の族はペイロード専用**である。

### 56.4 ∴ この driver の ioctl 面には、その呼び出しが無い

- `val_demux` の nr 空間は `0x00`〜`0xff` を**全部**見た（§50.1・§50.2）。
  `_IOWR` も引数なしの `_IO` も**存在しない**（508 本すべて `-7`）。
- `{種別 7 のハンドル, 番地, 大きさ, ディスクリプタ}` の形を**全 nr に投げた**
  結果、受理したのは `0x35`/`0x36` だけで、それは**先頭 1 語しか読まない**
  満杯手前の一時停止だった（§49.2・§50.3）。
- `nr=0x26` も種別 7 を拒む（本節）。

∴ **`MMAC_DEMUX_SetPidFilterBuffer` はハードウェアの関数表には在る（§11.4）が、
`val_merge` はそれを ioctl として出していない。** ベンダが使わないからである。

### 56.5 副産物：ペイロード経路の最小列は、思っていたより短い

この走行で、**デバイス 2・デバイス 3・`nr=0x30`/`0x33`/`0x34` を一切作らずに
PES が落ちた**。∴ `stage-c-payload.script` の前半はペイロードには不要で、
段階 C の実装が写すべき最小列は:

```
0x01{0} → 0x02{1,ptr} → 0x05{dev,3} → 0x65{0,1} 0x65{1,1} 0x64{0,1} 0x64{1,1}
→ 0x0b{6,ptr,dev,ptr} → 0x19{filt,ptr} → 0x26{filt,ring,size,desc}
→ 0xc9{7,1} 0xc9{8,1} → 0x29{filt} → 0x0c{filt,PID} → 0x0d{filt,1}
```

### 56.6 残っている道は 1 本

ioctl の面は尽きた。**残るのは、`val_merge` を通さずに裏 CPU へ直接コマンドを
積む**道である。材料は揃っている:

- エントリ表の位置と語の意味（§12.2・§13.3）
- データリング（物理 `0x00010800`・32 KB）と `data_wp`（§12.1）
- ドアベル＝BHIF のソフト割り込み（§12.3）
- ioctl → コマンド ID の対応表（§14.1・§49.4・§50.2）

`SetPidFilterBuffer` のコマンド ID は分かっていないが、**`0x19xx` の空きを
`{種別 7 のハンドル, 番地, 大きさ}` で掃けば当たる可能性がある**。
ただし**これは裏 CPU の API を総当たりする行為**で、§18.4 の「総当たりは実機を
落とす」がそのまま当てはまる。**着手するかどうかは判断が要る。**

## 57. トランスコーダのパラメータ空間を手に入れた（2026-09-24）

「同系統のトランスコーダには MPEG-2 スルーのモードがあり、そこに入れると生 TS が
出る。`blur` に `StartThroughTsOut` が、`SetTranscodeMode` に `VideoTSOut(2)Mode`
がある」という指摘を追った。**モード欄の在り処までは届いたが、掃いた結果を
判定できない**ところで止まっている。

### 57.1 文字列から見える構造（`strings` のみ）

```
blur:            HAVPF_PD_Player_StartTsOut / StartThroughTsOut /
                 StartBlackScreenTsOut / SetPidTsOut / SetAesKeyTsOut /
                 SendTTSData / GetTTSDataFreeBuf / GetNotificationTsOut
                 HAVPF_PD_Dione_SetTranscodeSetting / StartStreaming / …
dione_transcode: SetTranscodeMode / GetTranscodeMode / SetTsOutPacketSize /
                 SetAccumulationMode / SetClosedGOPMode / SetTranscodePIDFilter
                 usVideoTSOut2Mode = %u
                 FMB_API_{SET,GET}_LSI_REG / LOAD_FIRM / GET_USB_INFO / …
                 /app/bin/mb86h57_h58_aac_idle.bin   ← 2 つのファーム
                 /app/bin/mb86h57_h58_aac_trc.bin
```

**`StartThroughTsOut` は Dione ではなく Player（＝SoC 側の TS 出し）の API** で
あることに注意。`SendTTSData` / `GetTTSDataFreeBuf` があるので、**ベンダは
ユーザランドから TS（TTS）を AV へ流し込んでいる**とみられる。

### 57.2 ★ `/dev/h57fmb0` の転送の意味が分かった

`nr=0x02`/`nr=0x03` の引数は `{1, アドレス, ユーザ番地}` で、**16 ビット 1 語**を
読み書きする。値は**ポインタの先**にあるのでトレースには映らない。
そこで `yb1-ioctl` に**自前の I/O バッファ**を足した（`$B` がその番地、
`fill` で詰め、`show` で見る）。これで**我々の側から読み書きできる**:

```
nr=0x02 {1, 0x1000, $B} → 00 00      nr=0x02 {1, 0x1a52, $B} → 00 08
nr=0x02 {1, 0x1a6e, $B} → 80 00
```

`dione_transcode` が起動時に `0x1000`〜`0x1a6e` を **2 バイト刻みで全部書き、
1 語ごとに読み返している**のは、この空間の設定だった。

### 57.3 ★ パラメータ空間の地図（`trc` ファーム・設定直後）

`0x1000`〜`0x1a70`（1,337 語）を読むと、**非ゼロは 76 語**で、塊がはっきり分かれる:

| 範囲 | 中身 |
|---|---|
| `0x1002`〜`0x101c` | 系の設定（`0x1004=0x8903` `0x100c=0x4500` `0x101c=0x020f` ほか） |
| **`0x1028`〜`0x1038`** | **`0x8010 0x8011 0x8012 0x8014 0x8023 0x8024 0x8025 0x8028 0x8029`** ＝ 上位ビット `0x8000` 付きの **PID 表**（NIT/SDT/EIT/TDT と ARIB の表）＝**素通しさせる PID の一覧** |
| **`0x1100` と `0x1240`** | **同じ形の 2 つの塊**（間隔 `0x140`）。先頭が **`0x1102 = 2` / `0x1242 = 2`**、続いて `0xffff`、`0x0e10`(3600)、`0x00e0`(224)、`0x00c0`(192) … |
| `0x1500` と `0x1600` | **映像エンコーダ 2 系統**。`0x162c=0x0500`(1280) `0x162e=0x02d0`(720)＝**変換後の解像度**、`0x1c20`(7200)/`0x1770`(6000)/`0x0e10`(3600)＝ビットレート |
| `0x1a04`〜`0x1a6e` | 末尾（`0x1a52=8` `0x1a56=0x4000` `0x1a64`〜`0x1a6e` が `0x8000` ×6） |

∴ **`usVideoTSOutMode` / `usVideoTSOut2Mode` は `0x1102` / `0x1242`** と見るのが
自然である（**2 系統の同じ位置に同じ値**、`%u` で出る小さい数）。

### 57.4 掃いた。しかし**判定できない**

`0x1102` と `0x1242` に `0`〜`7` を書き（読み返しで一致を確認）、そのたびに
`nr=0x01` の状態コマンドを打って `nr=0x1a` を試した:

```
mode = 0 … 7    どの値でも   nr=0x1a → -145 (ETIMEDOUT)
                             DRAM の 3 バッファとも TS 0 hits
```

**既定値（`2`）でも `-145` である。** ∴ これは「モードの値が違う」のではなく、
**§53.4 のまま＝Dione の TS 入力に何も来ていない**ためで、**判定器が死んでいる。**

**モード欄を掃く作業自体は、入力が通るようになるまで意味を持たない。**

### 57.5 ∴ 壁は動いていない＝「SoC の TS 出し」

- `dione_transcode -t` は `/dev/tsout_demux` を開こうとして死ぬ。この
  ノードは**どのモジュールも作らず、この製品には無い**（§52.5・§53.1）。
- ベンダはそこを使わず、**`blur` が `HAVPF_PD_Player_StartTsOut` /
  `SendTTSData` で AV へ TS を流し込んでいる**とみられる（§57.1）。
- ∴ **`StartThroughTsOut` は Dione のモードではなく SoC 側の経路**で、
  そこが開かない限り Dione は入力待ちのままである。

### 57.6 次

1. **`dione_transcode -e <秒>`** は TS 入力エラーを `/tmp/dione_ts_err.dat` に
   出す。**入力が本当に来ていないことの独立な確認**になる。
2. `blur` の `Player TsOut` 族が **どの `val_demux` の nr に落ちるか**。
   ベンダが使う 33 個のうち意味が未確定なのは `0x0a` `0x19` `0x32` `0x33`
   `0x34` `0x47` `0x64` `0x65` `0x67` `0x6a` `0xc9`。**`SendTTSData` は
   `MMAC_DEMUX_CpuInputPlay` に当たるはずで、`nr=0x06`（実機が落ちる）が
   その位置にある**（§49.5 の番号規則から）。
3. パラメータ空間はもう我々のものなので、**入力が通った瞬間に §57.4 の掃きを
   やり直せば答えが出る**。

## 58. ★ トランスコーダのデータ経路が、ベンダアプリ抜きで動いた（2026-09-24）

§57.4 で「判定器が死んでいる」と書いた状態を抜けた。**`-d`（USB バルクで入力を
送る）なら `/dev/tsout_demux` は要らない。**

### 58.1 合成 TS を入力に与えた

判定用に**中身が一目で分かる TS** を作った（2,112 パケット・397 KB）:

| PID | 中身 |
|---|---|
| `0x0000` | PAT（program 1 → PMT PID `0x1000`） |
| `0x1000` | PMT（PCR `0x0100`、MPEG-2 映像 `0x0100` 1 本） |
| `0x0100` | ペイロードが **`5a <連番> a5` で始まり `00 01 02 03 …` と続く** |
| `0x1fff` | ヌル |

### 58.2 ★ `-d` は通る

```
/app/bin/dione_transcode -d -i /app/setting/in.ts -1 /tmp/out1.ts -2 /tmp/out2.ts -P 1
  [SUCCESS] start_device(),  1.63(s)
  [SUCCESS] StartStream(),   0.014(s)
  StartStreamThread(eIndex_ReceiveTSOut1) returns 0
  StartStreamThread(eIndex_ReceiveTSOut2) returns 0
  StartStreamThread(eIndex_SendTSIn)      returns 0
  waiting for stream end.  /  done stream end.
  [SUCCESS] NullOutStream(), 0.0046(s)
```

**エラー 0 件で最後まで通る。** ∴ **トランスコーダのデータ経路は、ベンダアプリ
抜きで、我々の手で端から端まで動く。**

> `-l`（入力をループ）を付けると `buffer overflowed, so drop data` が延々出て
> 起動もしない。**`-l` は使わないこと。**

### 58.3 ★★ `nr=0x1d` の 3 バッファの役割が確定した

走行後に DRAM を走査すると、**`0x0b442000` に 765 個の同期**が立ち、中身は

```
47 41 00 18 | 5a e8 02 a5 | 00 01 02 03 04 05 06 07 …
PID: 0x0100  0x1fff  0x0000  0x1000     scrambling: none=768
```

**我々が入れた合成 TS そのもの**だった。∴

| `nr=0x1d` の index | 物理 | 役割 |
|---|---|---|
| 0 | `0x0acea000` | **TSOUT1**（出力） |
| 1 | `0x0b096000` | **TSOUT2**（出力） |
| **2** | **`0x0b442000`** | **TS 入力**（SoC → Dione の受け渡し） |

**§10.2 が TS を見つけた `0x0acea000` は TSOUT1 ＝ 出力**で、§55 の結論
（再多重された配信用ストリーム）とぴったり合う。

### 58.4 モード欄の掃きは、まだ判定に届かない

合成 TS は**復号できる映像ではない**ので、**変換モードでは TSOUT1/2 が空**に
なるのは当然である（実測 0 hits）。**素通しモードなら TSOUT1 に入力がそのまま
出るはず**で、これが判定になる。

`-d` を走らせたまま `0x1102` / `0x1242` に `0`〜`7` を書いた（読み返しで一致）:

```
mode = 0,1,2,3,4,5,6,7   TSOUT1 / TSOUT2 とも 0 hits（入力側は 765 のまま）
```

∴ **モードは走行中に書いても効かない**（`StartStream` で確定されるとみられる）。
`dione_transcode` は `start_device` の中でパラメータ空間を丸ごと書くので、
**その後・`StartStream` の前に割り込む隙が無い。**

### 58.5 idle ファームは、この経路では起動しない

`/app/bin/mb86h57_h58_aac_idle.bin`（232 KB）を `_trc.bin`（1.45 MB）の上に
bind mount して読ませると:

```
send cmd [NG] cmd = 0x04 sub_cmd = 0x20, errno = 145
FMB_API_GET_FACTOR err=-1 → state_change( -> STOP)=-1
[FAIL] start_device(), status = e0000121
```

∴ **差し替えるだけでは動かない。** 別の使い方のイメージである。

### 58.6 次（判定を成立させる 2 つの道）

1. **復号できる本物の TS を入力にする。** そうすれば既定（変換）モードでも
   TSOUT1 が埋まり、**「入力のバイトがそのまま出てくるか」で素通しが判定できる**。
   本物の TS は**ベンダの配信中に `0x0acea000`（TSOUT1）から 2 MB 取れば手に入る**
   （§55 の採取と同じやり方。`dd` が /dev/mem で効かなかったので `yb1-mem` で採る）。
2. **モードを `StartStream` の前に書く。** `dione_transcode` に隙が無いので、
   パラメータ空間を**我々が直接全部書いてから** `nr=0x01` の状態遷移を自分で
   叩く形になる。空間の地図（§57.3）と状態コマンドの実例
   （`{0x00040000,0,0xffffffc0}` `{0x00040020,0,1}` `{0x00050000,0x20000,0x410000}`）は
   揃っている。

## 59. モード欄の掃きは「入力が受理されていない」ため無効だった（2026-09-24）

§58.6 の道 2（配信の観測を要しない方）を実施した。**書き込みの仕掛けは効いたが、
掃きの土台が無かった**ことが分かった。

### 59.1 ★ パラメータを「押さえ込む」やり方は成立する

`dione_transcode` は `start_device` の中でパラメータ空間を丸ごと書くので割り込む
隙が無い。そこで**隙を探すのをやめ、値を押さえ続けた**——別プロセスから
`0x1102`/`0x1242` を連続で書き続けながら `dione_transcode` を走らせる。

**走行中に読み返すと我々の値が入っていた**:

```
readback DURING the run: buf+0x000: 00 05      start_device ok:1 fail:0
```

∴ **ベンダの書き込みに勝てる。この手は使える。**

### 59.2 掃いた範囲（すべて無反応）

| レジスタ | 値 | 結果 |
|---|---|---|
| `0x1102` / `0x1242` | `0`〜`7` | TSOUT1/2 とも 0 hits |
| `0x1002` | `0`〜`3` | 同 |
| `0x100a` | `0`〜`2` | 同 |
| `0x1684` | `0`〜`2` | 同 |
| `0x1a52` | `0`〜`2` | 同 |
| `0x1518` / `0x1618` | `0x0f02` `0x0f03` `0x0000` | 同 |
| `0x1510` / `0x1610` | `0x0000` `0x0001` `0x0002` | 同 |

**28 通りとも `start_device` は成功したまま**で、**何一つ挙動が変わらない**。
これは「値が違う」にしては不自然で、**そもそも効く状態になっていない**ことを疑わせた。

### 59.3 ★★ 対照＝チップは我々の入力を受け取っていなかった

`-e <秒>`（TS 入力の検査）を付けると、はっきり出た:

```
[ERROR] TS stream is none.  [GetStreamErr_DBG@WirelessDtvStreaming.c:366]
[wrg] TS Check err ---> ReCheck. (2)
 ----> sync byte check = F
/tmp/dione_ts_err.dat : 32 00 00 00 …
```

**`sync byte check = F`** ＝ **同期バイトの検査で弾かれている。**
∴ **`0x0b442000` に我々の TS が載るのは「SoC が DRAM に置いた」ところまで**で、
**チップは一度も取り込んでいない。** ∴ **§59.2 の掃きは、そもそも判定に
なっていなかった**（TSOUT1 が空なのはモードのせいではない）。

### 59.4 192 バイト TTS にすると、文句が具体的になる

`blur` の `SendTTSData` と、TSOUT 記述子の `0x1132 = 0x1272 = 0x00c0`（192）から、
**入力は 4 バイトのタイムスタンプ付き 192 バイト**と読み、そう作り直して与えた:

| 入力 | チップの反応 |
|---|---|
| 188 バイト素の TS | `TS stream is none.` だけ |
| **192 バイト TTS** | **`Continious counter of TS stream is bad.` ＋ `Sync byte of TS stream is bad.`** |

**192 の方が確実に深く見ている**（連続性カウンタまで検査している）が、
`sync byte check = F` は変わらない。∴ **枠は近いが、まだ受理されていない。**

### 59.5 ∴ 必要なのは「本物の TS」1 個だけ

合成ストリームでは、枠を合わせてもチップの検査を通らない。**逆に言えば、
チップが受理する TS が 1 本あれば、§59.1 の押さえ込みで
モード欄を掃ける**（素通しなら TSOUT1 に入力がそのまま出る＝判定できる）。

**その TS は本機で録る必要が無い。** 手持ちの MPEG-2 TS（他の機器で録ったもの、
サンプルでよい）を 1 本置けばよく、**配信セッションの観測は要らない**。

### 59.6 次

1. **本物の TS を 1 本入れて `-d -e 1` を通す**。`sync byte check` が変われば
   判定器が立ち上がる。枠は 188 と 192 の両方を試す。
2. 通ったら §59.1 の押さえ込みで `0x1102`/`0x1242` ほかを掃き直す。
3. `Detect PID(%04x)` が出れば、チップがどの PID を見ているかも分かる。

## 60. 入力は届いている。足りないのは「どの PID を変換するか」（2026-09-24）

§59.5 の「本物の TS を 1 本」を用意して試した。**枠でも中身でもなかった。**

### 60.1 転送路を直した（運用）

`/app/setting` は残り 276 KB しか無く、telnet 経由の転送は 1 MB で 10 分かかる。
デバイスの busybox には **`tftp` があり `wget` は無い**ので、録画サーバ側に
読み取り専用の TFTP サーバ（`tools/host/tftpd.py`）を立てて引かせた:

```
tftp -g -r mp2.ts -l /tmp/mp2.ts 192.168.11.33 6969     →  2 MB を 7.4 秒
```

**10 分が 7 秒になった。** 置き場は `/tmp`（tmpfs・51 MB 空き）。

### 60.2 3 種類の入力を与えた

| 入力 | 中身 |
|---|---|
| 合成 TS | PAT/PMT ＋ `5a <連番> a5` の PID `0x0100` ＋ ヌル |
| 実 TS（H.264） | 録画サーバの Amatsukaze 出力を 188 バイト境界で切り出したもの |
| **実 TS（MPEG-2）** | **放送をその場で録ったもの**（`videoType: mpeg2` / 1080i / drop 0 / scrambling 0） |

**3 つとも、188 枠でも 192 枠でも、チップの反応は文字通り同一だった**:

```
[ERROR] TS stream is none.  /  [wrg] TS Check err ---> ReCheck. (2)
 ----> sync byte check = F
```

∴ **この検査は我々が送った USB の入力を見ていない。** 入力の種類をどう変えても
一字一句同じなので、**`-e` が読んでいるのはチップの「TS 入力ピン」の状態**
（この鑑では常に無信号）である。§59.3 で「同期バイト検査で弾かれている」と
書いたのは**誤読**で、取り消す。

### 60.3 ★ 入力は DRAM に届いている

最後の走行のあと、入力バッファを生バイトで見た:

```
0x0b442000:  02 82 94 00 | 47 01 40 1b | 1a 64 e6 12 …
             ↑タイムスタンプ  ↑我々が与えた TTS パケット（PID 0x0140）
```

先頭 4 バイトは**我々が付けたタイムスタンプの続き**（`0x02829400` ＝
10,638 パケット × 4000 の付近）で、後ろは**放送 TS の実パケット**。
∴ **SoC → DRAM → USB の入力経路は生きている。**（188 枠の走査が
「no TS here」と言うのは、中身が 192 枠だからである。）

### 60.4 ∴ 止まっているのは変換の入口

入力は届き、`StartStreamThread` は 3 本とも `0` を返し、`NullOutStream` まで
通るのに **TSOUT1/TSOUT2 が空**。**チップは「何を変換するのか」を知らない**
とみるのが自然である。`dione_transcode` には

```
SetTranscodePIDFilter / GetTranscodePIDFilter
V_PID_IN   A_PID_IN   PCR_PID_IN   PMT_PID_IN
Detect PID(%04x), waitNullOut = %d
```

があり、**変換対象の PID を明示的に与える設計**になっている。我々の入力の PID
（放送の実 PID）を伝えていないので、何も拾われていない。

### 60.5 次

1. **パラメータ空間から `V_PID_IN` / `A_PID_IN` / `PCR_PID_IN` / `PMT_PID_IN`
   に当たるレジスタを見つける。** §57.3 の地図には `0x1028`〜`0x1038` に
   `0x8010 0x8011 0x8012 0x8014 0x8023 …`＝**上位ビット付きの PID 表**が既に
   見えている。**入力 TS の PID（`0x0100` `0x0110` など）を書いた語を探し、
   §59.1 の押さえ込みで差し替える。**
2. それで TSOUT1 が埋まれば、**§57.4 のモード欄の掃きが初めて判定になる**。

## 61. ★ トランスコーダが我々の手で出力した。モード欄も効く（2026-09-24）

放送を録った**生 MPEG-2 TS** を十分な量（12 MB）与えたところ、**`-d` で
トランスコーダが実際に出力を出した**。§57.4／§58.4 で「判定できない」と
していた状態を抜けた。

### 61.1 ★ 出力が出た

```
入力  /tmp/in12.ts   11,999,852 B（放送の生 MPEG-2 TS・188 枠）
出力  /tmp/out1.ts    5,342,208 B      ← ベンダアプリ抜き・我々の手で
```

短い入力や合成ストリームで空だったのは、**量と中身が足りなかっただけ**だった。
**§60.4 の「チップは何を変換するのか知らない」という見立ては外れ**で、
`dione_transcode` は入力から自分で拾っている。

### 61.2 ★ モード欄（`0x1102` / `0x1242`）は効く

§59.1 の押さえ込みで値を保ちながら走らせると、**出力の大きさが値で変わる**:

| 値 | `out1.ts` | |
|---|---|---|
| `0`/`4`/`6` | 3.0〜3.3 MB | 変換（より強い圧縮） |
| **`2`（既定）** | **5.3 MB** | **変換** |
| `1` / `5` | 11.5〜12.7 MB | **入力とほぼ同じ大きさ** |
| `3` / `7` | 起動失敗 | |

∴ **`0x1102`/`0x1242` は確かに出力の作り方を決める欄**である（§57.3 の読みが
実測で裏づいた）。

### 61.3 しかし `1`/`5` は素通しではなかった

出力の先頭 2 MB を TFTP で持ち帰って PID を数えた:

| | 枠 | PID の内訳 |
|---|---|---|
| **入力** | 188 | `0100:9143` `0160:682` `0170:191` `0140:148` `0111:138` `0110:135` `0012:47` `01f0:20` |
| **既定（変換）** | **192** | **`0100:7437` `0160:950` `1fff:910` `0170:271` `0111:195` `0110:192` `0140:188` `0012:68`** |
| `1` / `5` | 192 | **`1fff:9442`** `0100:973` `0000:1` |

- **`1`/`5` の中身はほぼヌルパケット**。大きさが入力並みなのは**一定レートに
  するための詰め物**で、**素通しではない**。
- **★ 既定の変換出力は、入力の PID 構成をそのまま保っている**（`0x0100` 映像・
  `0x0110`/`0x0111` 音声・`0x0140`/`0x0160`/`0x0170` データ・`0x0012` EIT）。
  **PID を振り直していない。** §55.2 でベンダの配信出力が `0x0002`/`0x0003` に
  なっていたのは、**ベンダがそう設定している**からで、チップの性質ではない。
- **出力は例外なく 192 バイト枠**（4 バイトのタイムスタンプ付き）。

∴ **「映像だけ差し替えて他はそのまま」**が既定の振る舞いで、**映像の再エンコードを
やめさせる値**がまだ見つかっていない（`1`/`5` は「やめた結果ヌルで埋める」）。

### 61.4 運用：TFTP を読み書き両対応にした

`tools/host/tftpd.py` に WRQ を足し、**実機から母艦へも送れる**ようにした
（`tftp -p -l /tmp/x -r x <host> 6969`）。30 MB の取り込みも 2 MB の書き戻しも
数秒で済む。

### 61.5 次

1. **映像の再エンコードだけを止める欄**を探す。`0x1500`/`0x1600` の映像
   エンコーダ 2 系統（`0x162c=1280` `0x162e=720` の解像度、`0x1c20`/`0x1770`/
   `0x0e10` のビットレート）に、入力をそのまま通す設定があるはず。
2. **スクランブルされたままの TS を入力にする**（CS の MULTI2 未復号）。
   復号できない以上、**変換モードでは何も出ないはず**で、**素通しなら
   スクランブルされたまま出てくる**——**素通しの有無を一発で判定できる対照**になる。
   我々は復号しない（§0）ので、スコープには触れない。

## 62. ★★★ MPEG-2 スルーを当てた＝`0x100a` の上位ニブル ＝ `2`（2026-09-24）

**モードは「1 レジスタの上位ニブル（bit4-7）で、下位ニブルに 2 本目が同居する」
——この読みが当たった。** §61.2 で掃いた `0x1102`/`0x1242` は**下位ニブルしか
動かしておらず、映像モードを検証していなかった。**

### 62.1 当たりのレジスタ

**`0x100a`**（系の設定の塊・§57.3）は既定で **`0x0010`**:

```
0x0010  =  上位ニブル 1（TSOUT1 の映像モード）＋ 下位ニブル 0（TSOUT2 ＝ 停止）
```

**TSOUT2 が全走行で空（`out2.ts` が 0 バイト）だった**のと辻褄が合う。
§59.2 でこのレジスタを `0`/`1`/`2` にしたのは**上位ニブルを 0 にした＝TSOUT1 を
止めた**だけで、モードを試していなかった。

### 62.2 ★ 実測（平文の放送 TS 12 MB を入力）

上位ニブルだけを動かし、出力の**各パケットが入力とバイト単位で同一か**を数えた:

| `0x100a` | `out1` | 入力と同一のパケット |
|---|---|---|
| `0x0000` | 起動せず | — |
| **`0x0010`（既定）** | 3.03 MB | **19.8%** |
| **`0x0020`** | **7.22 MB** | **★ 85.1%** |
| `0x0030` | 10.68 MB | 59.4% |
| `0x0040` | 0.87 MB | 40.1% |
| `0x0050` | 3.18 MB | 19.8%（`0x0010` と同じ） |
| `0x0060` | 8.52 MB | 85.1%（`0x0020` と同じ） |
| `0x0070` | 7.07 MB | 59.4%（`0x0030` と同じ） |

**PID ごとに分けると決着がつく**:

```
0x0020（上位ニブル 2）
   PID 0100 :  7601 パケット,  7601 が入力とバイト同一  (100.0%)   ← 映像
   PID 0160 :   580 パケット,   580 が同一              (100.0%)
   PID 0170 / 0111 / 0110 / 0140 / 0012                 (100.0%)
   PID 1fff :  1556 パケット,     0 が同一              （レート調整の詰め物）

0x0010（既定・上位ニブル 1）
   PID 0100 :  7437 パケット,     0 が入力とバイト同一  (0.0%)     ← 再エンコード
   PID 0160 / 0170 / 0111 / 0110 / 0140                 (100.0%)
```

∴ **既定（`1`）は「映像だけ作り直し、他はそのまま通す」。
`2` は「映像もバイト単位でそのまま通す」＝MPEG-2 スルー。**
**ヌルパケット以外、出力は入力と同一である。**

> 値は **`1`≡`5`、`2`≡`6`、`3`≡`7`** と重なるので、効いているのは
> **bit4-5** で、`bit6`（＝`4`）は別の意味を持つ。

### 62.3 出力の枠

出力は**例外なく 192 バイト**（4 バイトのタイムスタンプ ＋ 188 バイト）。
188 バイトの素の TS が要るなら**先頭 4 バイトを捨てるだけ**でよい。

### 62.4 ⚠ スクランブルされた入力では、映像が落ちる

MULTI2 のかかったままの CS 録画（10,638 パケット中 10,159 が
`scrambling_control` 2 か 3）を同じ手順で入れると:

| `0x100a` | `out1` | 中身 |
|---|---|---|
| `0x0010`（既定） | **0 バイト** | 復号できないので当然 |
| `0x0020` / `0x0040` / `0x0060` | 144,384 B | **平文の PSI/SI と EIT だけ**（`1fff:680` `0012:30` `1e01:20` `0000:5` `0011:2` `0010:1`）、`scrambling_control` は全部 0 |

∴ **素通しモードでも、スクランブルされたパケットは出てこない。**
（我々は復号していない。復号できない素材を入れて、出てこないことを確かめただけ
である。§0 のスコープに触れない。）

**これは段階 C にとって重い。** 放送波は全部スクランブルされているので、
**この経路で得られるのは平文の PSI/SI だけ**になる。原因が
「暗号化パケットを捨てる仕様」なのか「PCR や多重化のタイミングを掴めずに
止まる」のかは、まだ分けられていない。

### 62.5 次

1. **暗号化パケットが落ちる理由を分ける。** `0x1028`〜`0x1038` の PID 表
   （`0x8010 0x8011 …` ＝ 素通しさせる PID の一覧）に**映像の PID を足す**と
   どうなるか。ここが「何を通すか」の指定なら、暗号化でも通る可能性がある。
2. `bit6`（`0x0040`）の意味。
3. 下位ニブル（TSOUT2）にも `2` を入れて、2 本目からも素通しが出るか。

## 63. ★★★ 落としていたのは中身ではなくフラグだった（2026-09-24）

§62.4 の「素通しでも暗号化パケットが出てこない」の原因を分けた。

### 63.1 PID 表を足しても変わらない

`0x1028`〜`0x1038` の PID 表（`0x8000 | PID`）に、入力の映像 PID を足して試した
（スクランブルされたままの CS 録画・素通しモード）:

| 置き方 | `out1` |
|---|---|
| 何もしない（対照） | 144,384 B |
| `0x103a`=`0x900e` / `0x103c`=`0x904e`（追加） | 144,384 B |
| `0x1038` を `0x900e` で上書き | 144,384 B |
| `0x103a`=`0x100e`（`0x8000` 無し） | 433,152 B（**増えたのはヌルだけ**） |

∴ **あの表は「素通しさせる PID の一覧」ではない**（PSI/SI の表を並べたもの）。

### 63.2 ★★ フラグだけを 0 にしたら、暗号化された中身がそのまま出た

同じ CS 録画の**ヘッダの `transport_scrambling_control`（バイト 3 の bit6-7）
だけを 0 にした**写しを作った。**ペイロードは一切触っていない**（暗号化されたまま）。

| `0x100a` | `out1` |
|---|---|
| `0x0010`（変換） | **0 バイト**（復号できないので当然） |
| **`0x0020`（素通し）** | **11,117,568 B**（入力 11,999,852 B） |

PID ごとにバイト比較すると:

```
PID 100e :  6369 パケット,  6369 が入力とバイト同一  (100.0%)   ← CS の映像・暗号化のまま
PID 104e :   218 パケット,   218 が同一              (100.0%)
PID 0012 :   135 /  135 (100.0%)    PID 1e01 : 105 / 105 (100.0%)
PID 0000 :    18 /   18 (100.0%)    PID 1fff : レート調整の詰め物
```

∴ **トランスコーダが落としていたのは「中身が読めないから」ではなく、
「ヘッダにスクランブルの印が立っているから」だった。**
**素通しは中身を一切見ない。** 印さえ立っていなければ、
**暗号化されたペイロードもバイト単位でそのまま出てくる。**

> **スコープ**: 我々は何も復号していない。**自分の写しのヘッダ 2 ビットを 0 に
> しただけ**で、ペイロードは最初から最後まで暗号化されたままであり、
> **出てきたものも暗号化されたまま**である。むしろこれは §4.1 が求める
> 「スクランブルされたままの TS」がこの経路から出ることの実証になる。

### 63.3 段階 C にとっての意味

```
素通しモード（0x100a 上位ニブル = 2）
  ├ 中身は問わない（H.264 でも MPEG-2 でも暗号化済みでも同じ）
  ├ 出力は 192 バイト枠（4 バイトのタイムスタンプ ＋ 188）
  └ transport_scrambling_control が 0 でないパケットだけ落とす
```

∴ **生 TS をこの経路から取り出すには、あと 2 つ**:

1. **印を無視させる**（そういうレジスタがあるか）か、
   **経路の上流で印を落とす**か。
2. **生の放送 TS をトランスコーダの入力に届ける**（§53.4 の壁。
   `-d` の USB 経由なら我々の手で入れられるが、それには SoC 側が
   TS を持っていなければならない）。

### 63.4 次

1. **印を無視させるレジスタ**を探す。判定は今や一発で、
   **スクランブルされたままの CS 録画をそのまま入れて `out1` が 11 MB 出れば当たり**。
2. `bit6`（`0x0040`）と下位ニブル（TSOUT2）。

## 64. スクランブルの印を無視させるレジスタは見つからない（2026-09-24）

§63.4 の 1。判定は一発（素のスクランブル CS 録画を素通しモードに入れて
`out1` が 11 MB 出れば当たり／144,384 B なら外れ）なので、機械的に掃いた。

### 64.1 掃いた 40 本

各レジスタを **`0xffff`** にして 1 回ずつ（1 本でその語の全ビットを覆える）:

| 群 | レジスタ | 結果 |
|---|---|---|
| 非ゼロ・系 | `0x1006` `0x101a` `0x101c` | 144,384 B（外れ） |
| 非ゼロ・系 | `0x1002` `0x1004` `0x100c` `0x100e` | **`start_device` が失敗**＝必須の設定 |
| 非ゼロ・末尾 | `0x1a04` `0x1a1c` `0x1a1e` `0x1a52` `0x1a54` `0x1a56` `0x1a58` `0x1a64`〜`0x1a6e` | 全部 144,384 B |
| ゼロ・系 | `0x1000` `0x1008` `0x1010`〜`0x1018` `0x101e`〜`0x1026` | 全部 144,384 B |
| ゼロ・表の先 | `0x103a` `0x103c` `0x103e` `0x1040` | 全部 144,384 B |
| ゼロ・出力記述子 | `0x1100` `0x1108` `0x110a` `0x1240` | 全部 144,384 B |

**40 本すべて外れ。** ∴ **「印を無視する」スイッチは、少なくともこの範囲には無い。**
ベンダの製品はトランスコーダへ**デスクランブル済みの TS しか送らない**ので、
そもそも要らない機能だった、と読むのが自然である。

### 64.2 ★ この経路の構造的な限界

§63 で分かったことを、段階 C の目的に当てはめると閉じてしまう:

```
-d（USB で入力を送る）
    SoC が既に TS を持っていることが前提
    → 持っているなら、そもそもトランスコーダは要らない

-t（TS 入力ピン）
    ピンに TS が来ていない（§53.4・我々の鑑では nr=0x1a が -145）
    → 仮に来ても、放送 TS には印が立っているので素通しが落とす（§62.4・§64.1）
    → 印が落ちているのは「デスクランブラを通した後」＝ CAS 処理後で、
      §0 により OpenIRD はそれを TS 源にしない
```

∴ **この基板のトランスコーダ経路から OpenIRD が使える生 TS は出てこない。**
出てくるのは (a) 我々が既に持っている TS の写し、か
(b) CAS を通した後の TS のどちらかである。

### 64.3 それでも残るもの（この調べで確定した事実）

トランスコーダについては、**使い方が全部割れた**:

| | |
|---|---|
| ノードを開く | `dione_transcode -t -P 1` で Dione が再列挙され `/dev/h57fmb0` が開く（§52.5） |
| パラメータ空間 | `nr=0x02`/`0x03` の `{1, アドレス, ポインタ}` で 16 bit 単位に読み書き（§57.2）。地図は §57.3 |
| DRAM の 3 本 | `nr=0x1d` の index 0/1/2 ＝ TSOUT1 / TSOUT2 / TS 入力（§58.3） |
| **素通し** | **`0x100a` の上位ニブル ＝ `2`**（下位ニブルは TSOUT2）。中身を問わずバイト単位で通す（§62） |
| 出力の枠 | 192 バイト（4 バイトのタイムスタンプ ＋ 188） |
| 落とす条件 | `transport_scrambling_control` が 0 でないパケットだけ（§63.2） |
| 転送 | `tools/host/tftpd.py` ＋ busybox `tftp`（§60.1） |

**別機種で同じトランスコーダを使う場合、ここはそのまま効く。**

### 64.4 次

**壁は §53.4 に戻る**＝「生の放送 TS が SoC のどこにも現れない」。
残っている道は §56.6 の 1 本だけ——**`val_merge` を通さず裏 CPU へ直接コマンドを
積み、`MMAC_DEMUX_SetPidFilterBuffer`（ハードには在るが ioctl に出ていない）を
自分で呼ぶ**。材料（エントリ表・データリング・ドアベル・コマンド ID 表）は
揃っているが、裏 CPU の API を総当たりする行為になる（§18.4）。

## 65. 訂正：復調器は Dione のピンを直接食わせていない（2026-09-24）

§53.7 で「**MXL683 の TS を Dione の入力ピンへ出す設定が、まだどこかにある**」と
書いた。これは「**復調器の TS がそのままトランスコーダに届いている**」という
**検証していない前提**の上に立っていた。**取り消して、測り直した。**

### 65.1 実測＝選局しても、ピンには何も来ない

地上デジタルでも **PSI/SI（PAT/PMT/EIT/NIT）はスクランブルされていない**。
そして素通し（`0x100a` の上位ニブル `2`）は印の立っていないパケットをそのまま
通す——暗号化された CS のファイルからでも**平文の表だけが 144 KB 出た**（§62.4）。

∴ **復調器がピンを直接食わせているなら、選局しただけで平文の PSI が出るはず**。
デスクランブラ（`val_m2dsc` / `val_dscram`）には一切触れずに測った:

```
yb1-mxl 1 tune 557   →  0x112d = 0x03c7（ロック）
dione_transcode -t   →  start_device / StartStream とも SUCCESS、Dione は列挙済み
0x100a = 0x0020 を押さえたまま nr=0x1a を 3 本のアドレスへ、15 秒かけて 5 回

  nr=0x1a → -145（ETIMEDOUT）× 5     DRAM 3 本とも 0xff のまま＝一度も書かれない
```

**平文の PSI すら来ていない。** ∴ **復調器は Dione のピンを直接食わせていない。**

### 65.2 ベンダのトレースからの裏づけ

同じ結論が、ベンダの配信 1 回ぶんの数字からも出る:

- **`nr=0x03`（USB で Dione へ押し込む）は 4.0 秒で終わる**＝我々の
  `start_device`（1,382 本・2 秒）と同じ性格＝**ファーム転送**。
  配信中ずっと TS を押し込んではいない（§54.4）。
- TS を運んでいるのは **`nr=0x1a`（DRAM ↔ Dione）が 40.8 秒間 181 回**。
- ∴ **ベンダの生 TS はピンから入っている。**
- しかしベンダのトランスコーダは**地上波の映像を再エンコードしていた**（§55.2）
  ＝**印が 0 の TS が届いていた**＝**デスクランブラより下流から来ている。**

∴ **ピンを食わせているのは SoC**（`dione_transcode -t` が要求する
`/dev/tsout_demux` ＝ **demux の TS 出力**という名前とも一致する）。

### 65.3 ∴ 壁は 1 つに収束した

```
demux が TS を丸ごと出す口が、どこにも開いていない
  ├ DRAM へ … SetPidFilterBuffer はハードに在るが ioctl に無い（§56.4）
  └ ピンへ  … StreamOut をベンダは呼ばず、tsout_demux も存在しない（§52.2・§53.1）
```

**トランスコーダ経路も、結局この 1 点に戻る。** §64.2 で「構造的に閉じている」と
書いた理由は、**印の問題ではなく、そもそも TS が出てこないこと**だった。

### 65.4 次

残るのは §56.6 の 1 本だけ——**`val_merge` を通さず裏 CPU へ直接コマンドを積む**。
材料は揃っている（エントリ表 §12.2、語の意味 §13.3、ドアベル §12.3、
ioctl → コマンド ID の対応 §14.1／§49.4／§50.2）。ただし**裏 CPU の API を
総当たりする行為**で、§18.4 の「総当たりは実機を落とす」が当てはまる。

## 66. 裏 CPU のコマンド形式を全部採った。残るはドアベル 1 点（2026-09-24）

§56.6 の道（`val_merge` を通さず裏 CPU へ直接コマンドを積む）に着手した。
**観察 → 仕様化の段はここで終わる。** 実行の合図だけが残っている。

### 66.1 ★ エントリの形（既知の ioctl を 1 本ずつ出して確定）

`nr=0x26`（コマンド `0x1929`・要求 12 B ＋ 応答 4 B）と `nr=0x2b`（`0x192e`）を
出して、エントリ表（物理 `0x00010400`・32 バイト刻み）の差分を採った:

| 語 | 値 | 意味 |
|---|---|---|
| 0 | **`0`（変わらない）** | 実行中だけ立つとみられる。完了後は 0 |
| **1** | `19290100` / `192e0101` | **`(コマンド ID << 16) | 連番`**。連番は 1 ずつ増える |
| **2** | `a0010800` → `a0010810` | **要求の番地**（KSEG1・データリング内） |
| **3** | `a001080c` → `a001081c` | **応答の番地 ＝ 要求 ＋ 要求長** |
| 4 | `7fffffff` | タイムアウト |
| **5** | **`00000002`** | **定数 2**（§12.2 では 0 としていた。訂正） |
| 6 | `00000010` | **要求長 ＋ 応答長** |

データリングは **`0xa0010800` から 16 バイト刻み**で切り出される。

### 66.2 ★ 要求の中身は「ioctl 引数の先頭 N 語」そのもの

区別のつく引数を渡して確かめた:

```
nr=0x26 {11111111, 22222222, 33333333, 44444444}
  → リング `0xa0010800`: 11111111 22222222 33333333   （12 バイト＝要求長ちょうど）
nr=0x2b {55555555, 66666666, 77777777, 88888888}
  → リング `0xa0010810`: 55555555 66666666 77777777
```

∴ **`val_merge` は引数を加工していない。** 4 語目（制御ページへのポインタ）は
ドライバ側で処理され、裏 CPU には渡らない。

### 66.3 受け渡しは共有メモリには残らない

`csd_info` が指す残りのブロック（`0x00018800` / `0x00019000` / `0x0001d000` /
`0x0001f000`）と、ブロック 0（`0x00010000`・1 KB）を ioctl の前後で撮ったが、
**1 語も変わらない**。∴ 「実行せよ」の合図は**一瞬で消える**種類のものである。

### 66.4 BHIF のソフト割り込み窓（実測値）

```
0x100001a0 SW_INT      = 00000000
0x100001a4 SW_INT_EN   = ffffff07   ← bit 0,1,2 と 8..31 ＝ メイン CPU が受ける側
0x100001a8 SW_INT_CLR  = 00000000
0x100001ac             = a0010000   ★ 共有メモリの先頭。GPL に名前が無い
0x100001b4             = 00070000   （3 ビットぶんのマスクに見える）
0x100001bc / 0x1cc     = 03dfdc35 / 030ff435（静的）
```

**`0x1ac` に共有領域の先頭が入っている**のが収穫で、**裏 CPU はここからコマンド
スタックを見つけている**とみられる。

### 66.5 ドアベルは bit 3〜7 ではない

読み取り専用のコマンド **`0x1906`**（要求長 0・応答 4 バイト。`val_demux`
`nr=0x07` の読み方向が `ret=0` で返すもの・§50.2）を、**未使用のエントリ 20**
に自分で積み、応答領域を 0 にしてから `SW_INT` の bit 3〜7 を 1 本ずつ立てた
（word0 は 0 と 1 の両方で）:

```
bit 3,4,5,6,7 を立てる  →  SW_INT = 08, 18, 38, 78, f8 と溜まっていく
                           応答は 0 のまま / エントリの word0・word1 も不変
そのあと普通の ioctl（nr=0x01）は ret=0 ＝ AV は無傷
```

∴ **立てたビットを誰も消さない**＝**裏 CPU はここを見ていない。**
そして**この操作は安全**（実機も AV も壊れない）。

### 66.6 次の候補

1. **`0x100001b0` / `0x100001b4`**。`0x1b4 = 0x00070000` は 3 ビットのマスクに
   見え、`0x1ac` に共有先頭が入っていることからも、**この窓が裏 CPU への
   通り道**の可能性がある。
2. **エントリ番号をどこかで伝えている**かもしれない（§12.1 の
   `csd_info+0x24` ＝ エントリ番号 0〜31）。共有メモリには現れないので、
   レジスタ経由か、ドアベルの値に埋めているか。
3. word0 の「実行中」の値（完了後に 0 に戻るので、**走っている最中に採る**
   必要がある。`nr=0x2f` のポーリングを回しながら撮るのが早い）。

## 67. ドアベルは当てられなかった。ただし窓の性格は分かった（2026-09-24）

§66.6 の 3 候補を潰した。**ドアベルは出ていない**が、**周辺のレジスタの性格**が
分かり、**次に採るべき手段**がはっきりした。

### 67.1 word0 は「実行中」でも立たない

裏 CPU を忙しくさせた状態（`nr=0x2f` を連射。エントリ表の語 1 が
`19320101` `19320103` … `191b0124` と動くのを確認）で、**先頭 8 エントリを
60 回サンプル**した。**word0 が 0 以外になったサンプルは 1 つも無い。**

∴ csd の `set_apictrl_flag` / `get_apictrl_flag` が触る「フラグ」は
**共有メモリではなく csd 自身の帳簿**である。**裏 CPU はそれを読んでいない。**
∴ 裏 CPU に要るのは**エントリの中身と合図だけ**。

### 67.2 21 通りの合図——すべて空振り

エントリ 20 に読み取り専用の `0x1906`（要求長 0・応答 4 バイト）を積み、
応答領域を 0 にしてから 1 つずつ試した:

| 試した先 | 値 | 応答 |
|---|---|---|
| `0x100001a0` SW_INT | bit 0〜7 を 1 本ずつ | すべて 0 のまま |
| `0x100001a8` SW_INT_CLR | bit 3,4,5 | 同 |
| `0x100001b0` | `1` `8` `0x14` `0x10000` `0x114` | 同 |
| `0x100001b4` | `0x70000` `0x10000` `0x20000` `0x40000` `1` | 同 |

### 67.3 ★ 副産物＝窓の性格

- **SW_INT の bit 0〜5 は消費される。bit 6,7 は残る。**
  §66.5（bit 3〜7 だけを立てた）では**誰も消さなかった**のに、今回 bit 0〜2 も
  立てたら **0〜5 がまとめて消えた**。∴ **bit 0〜2 がメイン CPU の割り込みを
  起こし、その処理が 0〜5 をまとめてクリアしている。**
- **`0x100001b0` は生きている。** `0x114` を書いたのに、あとで読むと `0x11d`。
  **誰かが書き換えている。**
- **`0x100001b4`（静止値 `0x00070000`）は機能している。**
  `1` を書いたあと、**普通の ioctl が応答を返さなくなった**（AV が固まった）。
  **電源を入れ直せば完全に戻る**（確認済み・`ret=0`、窓も静止値に復帰）。
- `0x100001bc` は**カウンタ**（起動ごとに `03dfdc35` / `02ffdc35` / `06dfdc35`）。

### 67.4 ⚠ 触ってはいけないものの追加

| 番地 | 症状 |
|---|---|
| **`0x100001b4`** | **AV が固まる**（電源で戻る） |

### 67.5 ∴ 当て推量での探索は打ち切る

ホストインターフェースの窓を当てずっぽうで叩くのは、**当たる確率が低く、
しかも実際に AV を固めた**。**この方法は割に合わない。**

**代わりに採るべきは、合図を「観測する」こと。** ユーザランドからでは
1 サンプル 50 ms で、マイクロ秒のパルスは捕まらない。**自前のカーネル
モジュールで BHIF の窓を高頻度に見張れば、csd が何を書いているかが見える。**
逆アセンブルは要らない——**走っているものを観測するだけ**なので
clean-room の枠内に収まる（`i2c-algo-emma` にトレースを足したのと同じ方法）。

### 67.6 次

1. **BHIF 窓のウォッチャ（自前 `.ko`）**。`0x100001a0`〜`0x100001bf` を
   タイトループで読み、値が変わった瞬間だけ `printk` する。ioctl を 1 本出せば、
   **`assert_int` が何番地に何を書いたかが出る。**
2. それが出れば §66 のエントリ形式と合わせて、**`SetPidFilterBuffer` を
   自分で呼べる。**

## 68. 裏 CPU への合図は当て推量では出ない（2026-09-24）

§67.6 の前に、安い当て方を出し切った。**出ない。**

### 68.1 `SW_INT` は 8 ビットしか無い

`0x100001a0` に `0xffffffff` を書くと、読み返しは **`0x000000f8`**。
∴ **書けるのは bit 0〜7 だけ**で、**bit 0〜2 は即座に消費される**（メイン CPU 側の
処理）。**この窓はもう全部見た。**

### 68.2 形を正してもう一度

§67.2 の試みはエントリ 20・連番を勝手に決めた形だった。**val_merge が使うのと
同じ作法**に揃えて置き直した——エントリ表を読んで**最初の空きエントリ**と
**連番の次の値**を求め、そこに `0x1906` を積む:

```
entry 0..10 が使用中（seq 0x0100..0x010a）  →  空き = entry 11, 次の連番 = 0x010b
entry 11 word1 = 1906010b, word2/3 = a0010c00, word4 = 7fffffff,
          word5 = 2, word6 = 4       ← val_merge が書くのと同じ形

A: 合図を出さずに 3 秒待つ        → 応答 0（裏 CPU はポーリングしていない）
B: SW_INT の bit 3〜7 を 1 本ずつ  → 応答 0（5 通りとも）
```

**AV はどの試行でも無傷。**

### 68.3 ∴ この 1 点は、当て推量では埋まらない

コマンドの形は完全に分かっている（§66）。足りないのは**「実行せよ」の伝え方**
だけで、そこだけが:

- 共有メモリに痕跡を残さない（§66.3）
- `SW_INT` の全ビットを試しても動かない（§68.1・§68.2）
- 近傍のレジスタ（`0x1b0` `0x1b4`）も外れ、`0x1b4` は書くと AV が固まる（§67.3）
- ユーザランドからの観測では捕まらない（CPU が 1 個なので、書いている最中は
  こちらが走れない）

**残る観測手段は、自前のカーネルモジュールで MIPS のハードウェア
ウォッチポイント（CP0 `WatchLo`/`WatchHi`）を張り、範囲を二分して
「どの番地が書かれたか」だけを絞ること。** 命令列は読まない（読めば
逆アセンブルになりスコープ外）ので、**当たった番地が分かるところまで**である。
相応の工数がかかり、当たる保証も無い。

### 68.4 この基板の現在地（まとめ）

| 経路 | 状態 |
|---|---|
| ペイロードフィルタ（種別 6） | **PES が出る**（§43・実証済み・最小列は §56.5） |
| PID フィルタ（種別 7） | **PSI/SI セクションが出る**（§55.1。ベンダのリングで確認） |
| PID フィルタ → DRAM の生 TS | **`SetPidFilterBuffer` が ioctl に無い**（§56.4） |
| StreamOut | ベンダが呼ばない・`tsout_demux` も無い（§52.2・§53.1） |
| RECORD / PVR | ハンドルを作れない（§51） |
| トランスコーダ | **全部割れた**が、入力は SoC 依存で、素通しは印で落とす（§62〜§65） |
| 裏 CPU 直叩き | **コマンドの形は完全。合図だけ不明**（§66・§68） |

∴ **生 TS を出す道は、合図 1 つの向こうにある。**

## 69. ウォッチポイントは動くが、狙う番地が割り込み経路にある（2026-09-24）

§68.3 の手段——MIPS のハードウェアデータウォッチポイントで**番地だけ**を絞る
——を実装した。**道具は動く。しかし狙いどころが悪い。**

### 69.1 道具

`tools/yb1-bhifwatch`（GPL-2.0・我々のコード）。CP0 `WatchLo`/`WatchHi` に
**書き込みだけ**（W ビット）のウォッチを張り、`WatchHi` のマスク
（`VAddr[11:3]`）で 8 バイト〜4 KB の範囲を選ぶ。カーネルの `do_watch()` は
`TIF_LOAD_WATCH` の立っていないスレッドでウォッチレジスタを**クリアする**ので、
**`WatchLo` が 0 になったこと**をもって「その範囲に書き込みがあった」と判定する。

**命令列は一切読まない。** 読めば逆アセンブルになりスコープ外（`docs/clean-room.md`）。

ビルドは `scripts/build-yb1-bhifwatch.sh`（`i2c-algo-emma` を建て直したのと
同じツリー・同じ手順）。`vermagic=2.6.27.39 preempt mod_unload MIPS32_R2 32BIT`
＝**実機のベンダカーネルと一致**。

### 69.2 実機での結果

| 張った範囲 | 結果 |
|---|---|
| BHIF の 4 KB ページ全体（`0xb0000000` / mask `0x1ff`）、**無負荷** | **即座にボードが固まる** |
| `0xb00001a0` の 8 バイトだけ（mask 0）、負荷あり | `armed` のログを残して**固まる** |

ログは insmod の**前に** `sync` してあるので、**張った直後に落ちている**ことが
確定している。どちらも**電源を入れ直せば完全に戻る**（`ret=0` を確認）。

### 69.3 なぜ固まるか

§12.3 の実測では **`CSD_INTEVENT00` が待機中でも 80 Hz で鳴っている**。
その処理は `SW_INT_EN`（`0x100001a4`）のマスク/アンマスクを伴う——
つまり**我々が張りたい番地は、割り込みハンドラが触る番地**である。

書き込みが割り込み文脈で起きると、ウォッチ例外もそこで発生する。2.6.27 の
`do_watch()` は `force_sig(SIGTRAP, current)` を呼ぶ作りで、**割り込み文脈から
呼ばれることを想定していない**。∴ ハングする。

### 69.4 ∴ 続けるなら、例外ハンドラごと差し替えることになる

「記録して何もせず戻る」だけのウォッチ例外ハンドラを自前で置けば通せるが、
`set_except_vector()` は export されていないので**例外ベクタを直接書き換える**
ことになる。さらに侵襲的で、**失敗するたびに電源の入れ直し**（約 2 分）である。

**道具はリポジトリに残す**（`tools/yb1-bhifwatch`）。**別の番地**——割り込み
経路が触らないところ——を見たいときには、そのまま使える。

### 69.5 現在地

**生 TS を出す道は、合図 1 つの向こうにある**（§68.4）。その合図は

- 当て推量では出ない（§68）
- ユーザランドからは観測できない（CPU が 1 個）
- ウォッチポイントでは狙う番地が割り込み経路にあって固まる（本節）
- 逆アセンブルは**恒久的にスコープ外**（`docs/clean-room.md`）

∴ **この 1 点は、いまの道具立てでは埋まらない。**

## 70. 合図は要らなかった。関数は名前で呼べる（2026-09-24）

§69.5 の「この 1 点は埋まらない」は**前提が狭すぎた**。合図（ドアベル）を
再現しようとしていたが、**合図を鳴らす側のコードは実機にあり、その関数は
`/proc/kallsyms` が名前で教えてくれる**。再現するのではなく、**持ち主のコードに
仕事を返せばよい**。

### 70.1 まず、合図が出ない理由を潰しきった

先に §68 の続きを機械的に終わらせた（r118〜r122）。

| 試したこと | 結果 |
|---|---|
| `nr=0x24` を 2 語の形で 56 通り（§56.4 が掃いていない空間） | **全部 −3**。リングは空 |
| 共有 64KB（物理 `0x00010000`〜`0x0001ffff`）を本物のコマンド中に差分 | **動くのはエントリの語と要求の語だけ**。プロデューサ添字は**どこにも無い** |
| csd が次に書くものと**バイト単位で同じ**エントリを手で置き、合図を鳴らす | **取られない**。無合図 3 秒 → `SW_INT` bit 0/1/2 個別 → 3 つ同時 → `0x1b0` に添字、全部だめ |
| ホストインターフェース `0x1a0`〜`0x1b8` をタイトループで監視（168 万パス） | **変化ゼロ**。単一 CPU なので、我々のループは csd が寝てからしか走らない |

- **`0x100001b0` は添字レジスタではない**。3 を書いてから 4 を書くと **7** が読める
  ＝**write-1-to-set のラッチ**である。
- **キャッシュは原因ではなかった**。MIPS の `__uncached_access()` は
  `file->f_flags & O_SYNC` で即 1 を返し、`yb1-poke` は `O_SYNC` で開いている
  ＝**`/dev/mem` 経由の書き込みは非キャッシュ**。なお **`/dev/kmem` では
  KSEG1 に届かない**（`high_memory` より上なので `vread`/`vwrite` 経路に落ち、
  読めば 0、書けば「書けた」と言って何もしない）。

∴ **合図はユーザランドからは観測も再現もできない**。ここは変わらない。

### 70.2 ★ コマンドエントリの語の意味が全部割れた（§66 の訂正）

`tools/yb1-mem` に `spin`（n 語をタイトループで読み、**変化を全部時系列で**記録）を
足して、本物のコマンド 1 本の最中を見た。churn（最初と最後の比較）では
`0 → 1 → 0` を見落とすが、spin なら見える。

| 語 | 意味 |
|---|---|
| 0 | ほぼ常に 0。成功した一部のコマンドでだけ非ゼロ（`SetPidFilterBuffer` で `0x00040002`、`0x1915` で機器ハンドル）。**「有効」フラグではない**（spin でも 0→1→0 は出なかった） |
| 1 | `(コマンド番号 << 16) | 通し番号`。通し番号は 16bit の通し |
| 2 | 要求の番地（KSEG1、データリング内） |
| 3 | 応答の番地 ＝ 要求の番地 ＋ 要求長 |
| 4 | `0x7fffffff` |
| **5** | **★ 状態。ioctl は `−語5` を返す**（`0x1900`→1 と `ret=-1`、`0x1901`→6 と `ret=-6`、`0x1926`→3 と `ret=-3`、成功は 0） |
| 6 | 要求長 ＋ 応答長（バイト） |
| 7 | 0 |

**§66 の「語5 は定数 2」は誤り**——失敗したコマンドの状態 2 を定数と読み違えていた。
**§12 の「データリングは 16 バイト刻み」も誤り**で、実際は**要求長＋応答長ぶんだけ
進む 4 バイト刻み**である。エントリ表は 32 枠だが、**添字 0 は使われない**
（31 の次は 1）。

### 70.3 ★★ `/proc/kallsyms` が AV の関数表を名前で持っている

観察として許される範囲（`nm` 相当・§clean-room）で、`val_merge` の
**`MMAC_DEMUX_*` が 76 本、名前と番地つきで読める**。自前の GPL モジュール
`tools/yb1-mmac`（`mmaccall.ko`）に番地と引数を渡して 1 回だけ呼び、外すだけの
道具を置いた。

```
FN=$(awk '$3 == "MMAC_DEMUX_SetPidFilterBuffer" { print $1 }' /proc/kallsyms)
insmod mmaccall.ko fn=0x$FN a0=0x00080023 a1=0xaf000000 a2=0x00020000 nargs=3
```

呼ぶと**ドライバ自身のデバッグ出力**（`set_apictrl_flag: entry = 19` →
`free_entry` → `Free_CmdStackArea`）が出て、**コマンドスタックにエントリが
1 本積まれる**。∴ **合図は val_merge が鳴らしてくれる。** 引数の並びが分から
なくても、**エントリの要求長が本当の語数を教える**ので機械的に詰められる。

これは逆アセではない。命令列は一切読まず、名前で呼んで**答え**（ハードが
リングに何を書くか）を得ているだけで、ベンダ資材は 1 バイトも出て行かない。

### 70.4 ★★★ `nr=0x26` が `SetPidFilterBuffer` だった（§56.4 の撤回）

`MMAC_DEMUX_SetPidFilterBuffer` を名前で呼ぶと、**コマンド `0x1929`・要求 3 語・
応答 1 語**が積まれる。同じ走行のリングを見ると、**`nr=0x26` が積むのも
`0x1929` で、要求の中身は `{ハンドル, リングの番地, 大きさ}`・応答は
ディスクリプタ**——バイト単位で同じである。

```
要求リング +0x08:  00080023 ad000000 00020000 | b00336a0
                   ハンドル  リング    128KB     ← 応答
```

- 受け付けるのは **種別 6 のハンドル**（`0x0008xxxx`）。種別 7（`0x0010xxxx`）は
  名前で呼んでも**状態 3**＝§56.3 の ioctl での拒否と同じ。
- `nr=0x2b`（バッファオブジェクトにリングを与える）は**別のコマンド `0x192e`**。

∴ **`MMAC_DEMUX_SetPidFilterBuffer` は ioctl として露出している。`nr=0x26` が
それである。** §56.4 の「val_merge はそれを ioctl として出していない」は**撤回**。
我々は §19 以来それを使っていた。**ハードウェアの言う "PID フィルタ" が、
我々が §16.2 で「種別 6 ＝ペイロードフィルタ」と名付けたものである。**

### 70.5 ★★ 分水嶺の答え：全 PID 素通しでも、出るのは PES

`ControlHardwarePidFiltering` を**全 PID 素通し**（`nr=0x48 {機器, 0}`・`ret=0`）に
したうえで、動く出口——種別 6 フィルタ＋自前リング——のバイトを読んだ。

| リング | PID | 中身 |
|---|---|---|
| `0x0d000000` | `0x0130` | **`00 00 01 bd 00 2e 80 81 … 43 49 53 … 6a 70 6e`** ＝ PES（§43 と同じ） |
| `0x0e000000` | `0x1fff`（ヌル） | **未書き込み**（`ff` のまま） |
| `0x0c000000` | 種別 7＋バッファオブジェクト | **未書き込み**（§56.2 と同じ） |

**`0x47` が 188 バイト周期で立つ場所は、どのリングにも無い。**

∴ **PID フィルタの出口は、TS ヘッダを剥いだ中身を書く。** 素通し設定は
「前段の PID 選別を止める」ものであって、**出口の形式を変えない**。

### 70.6 ★★ 関数表そのものが、DRAM への生 TS が無いことを言っている

`MMAC_DEMUX_*` 76 本の行き先は**ちょうど 3 つ**しかない。

| 行き先 | 関数族 | 出るもの |
|---|---|---|
| ペイロードバッファ | `SetPayloadBuffer` / `Get…WritePointerOffset` / `Reset…` / `Set…WritePointerOffset` / `Set…NearlyFullPointerOffset` | 解析済み |
| **PID フィルタバッファ** | `SetPidFilterBuffer` ＋ 同じ 5 本組 | **PES**（§70.5） |
| **StreamOut** | `OpenStreamOutDevice` / `CloseStreamOutDevice` / `SelectStreamOutGroup` / `StartStreamOut` / `StopStreamOut` | **ピン**（`Set…Buffer` が無い＝DRAM ではない） |

**`Transport` / `Packet` / `Raw` / `Through` の名を持つ関数は 1 本も無い。**
∴ **復調器から来た 188 バイトをそのまま DRAM に落とす出口は、ハードウェアの
API に存在しない。** §52 の見立ては正しかった。

### 70.7 ★★★ StreamOut は生きていて、`nr=0x6a` がそれだった（§52.2 の撤回）

名前で呼んで、エントリのコマンド番号を読むだけで対応が取れる。

| 関数 | コマンド | 要求 |
|---|---|---|
| `MMAC_DEMUX_OpenStreamOutDevice` | `0x194a` | **4 語** ＋ 応答 1 語（＝値 4 つ＋ハンドル用ポインタの 5 引数） |
| `MMAC_DEMUX_CloseStreamOutDevice` | `0x194b` | 1 語 |
| `MMAC_DEMUX_SelectStreamOutGroup` | `0x194c` | 2 語 `{ハンドル, グループ}` |
| **`MMAC_DEMUX_StartStreamOut`** | **`0x194d`** | 1 語 `{ハンドル}` |
| `MMAC_DEMUX_StopStreamOut` | `0x194e` | 1 語 |

そして **ioctl `nr=0x6a` が積むのも `0x194d`**＝**`nr=0x6a` は `StartStreamOut`**
である。`stage-c-payload.script` は**ベンダのトレース由来で `nr=0x6a` を 2 回
呼んでおり、`ret=0` が返っている**。

∴ **§52.2 の「ベンダは StreamOut を一度も呼ばない」は撤回。** 呼んでいる。
そして**我々の段階 C の列も、すでに StreamOut を開始している**。

まだ埋まっていないのは `OpenStreamOutDevice` の 4 語の中身（ハンドルが取れて
いないので `SelectStreamOutGroup` / `StartStreamOut` は状態 3 で跳ねる）。
**要求長 4 語は確定している**ので、あとは値を詰めるだけである。

### 70.8 掃き残しと、触ってはいけないものの追加

- 種別 6 のハンドル＋1 語の形で `nr=0x29`〜`0xc7` を掃いた（r125）。**全部 −7**
  ＝新しい ioctl は無い。§50 の結論（nr 空間は尽きている）と一致。
- **`nr=0x28` と `nr=0xc8` は `yb1-ioctl` を落とす**（`nr=0x10` と同じ扱い）。
  触ってはいけない一覧に追加する。
- `/app/setting`（4MB・yaffs2）は**満杯になると "Cannot allocate memory" で
  書けなくなる**。走行前に古いログを消すこと。

### 70.9 現在地

| 問い | 答え |
|---|---|
| PID フィルタの出口に生 TS は出るか | **出ない。PES である**（§70.5・実測） |
| `SetPidFilterBuffer` は使えるか | **使っていた。`nr=0x26` がそれ**（§70.4） |
| DRAM へ生 TS を落とす関数はあるか | **無い**（§70.6・関数表 76 本） |
| 残る出口 | **StreamOut（ピン）**。`StartStreamOut` は `nr=0x6a` で通る。`OpenStreamOutDevice` の 4 語が残件（§70.7） |
| 裏 CPU への合図 | **要らなくなった**。関数は名前で呼べる（§70.3） |

∴ **次は StreamOut のピンである。** `OpenStreamOutDevice` の 4 語を詰め、
`SelectStreamOutGroup` でグループを選び、`StartStreamOut` して、**その TS が
どこへ出るか**（トランスコーダの TS 入力ピンか、どこにも繋がっていないか）を
見る。トランスコーダ側は §62 で **MPEG-2 素通し（`0x100a` 上位ニブル 2）**が
バイト一致で通ることが分かっているので、**繋がっていれば `0x0acea000` に出る**。

ただし §63 のとおり、トランスコーダは
**`transport_scrambling_control != 0` のパケットを落とす**。∴ この経路で
スクランブルされたままの TS が通るかどうかが、段階 C の最後の分岐になる。
**通らないなら、そこで止める**——限定受信の解除は §0 のとおり恒久的に対象外で、
その結果自体が「ハードウェアタップが本線」の確定になる。

## 71. StreamOut は動く。ただしこの基板では誰も受け取らない（2026-09-24）

§70.7 の残件（`OpenStreamOutDevice` の 4 語）は、**探す必要がなかった**。
起動直後のコマンドスタックに、**状態 0 で通った StreamOut のコマンドが
そのまま残っていた**。

### 71.1 ★ 要求の中身から、4 本とも ioctl の対応が確定した

```
エントリ 194a0113  要求 a00108b8 (4 語)  応答 a00108c8   状態 0
エントリ 194c0114  要求 a00108cc (2 語)                  状態 0
エントリ 194d0116  要求 a00108dc (1 語)                  状態 0
エントリ 194e0117  要求 a00108e0 (1 語)                  状態 0

要求リング +0xb8: 02010002 00000001 00000002 00000005 | 00002002  ← 応答
           +0xcc: 0210006b 00000001
           +0xdc: 00002002
           +0xe0: 00002002
```

これは `stage-c-payload.script` の次の 4 行と**バイト単位で一致**する:

| ioctl | 関数 | 引数 |
|---|---|---|
| `nr=0x30 {機器2, 1, 2, 5, ptr}` | **`OpenStreamOutDevice`** | 応答＝**ハンドル `0x00002002`**（§21 の「第 5 のハンドル空間」の正体） |
| `nr=0x32 {PIDフィルタ, グループ}` | **`SelectStreamOutGroup`** | どのフィルタを StreamOut のグループに入れるか |
| `nr=0x33 {0x00002002}` | **`StartStreamOut`** | |
| `nr=0x34 {0x00002002}` | **`StopStreamOut`** | |

∴ **StreamOut は ioctl 面に全部出ている。** そして **`stage-c-payload.script` は
ベンダのトレース由来なので、`nr=0x33` で開始した直後に `nr=0x34` で停止して
いた**。§70.7 の「`nr=0x6a` が StartStreamOut」は**位置合わせの誤り**で、正しくは
`nr=0x33`/`nr=0x34` である（`nr=0x6a` は未同定のまま）。

### 71.2 停止を外して、機器 2 も全 PID 素通しにして、動かした

`nr=0x34` の行を消し、`nr=0x48 {機器2, 0}` にして走らせた（`r132`）。

```
nr=0x48 ret=0  {00010000, 0}      機器 1 全 PID 素通し
nr=0x48 ret=0  {02010002, 0}      機器 2 全 PID 素通し
nr=0x30 ret=0  {02010002, 1, 2, 5, ptr}   -> 0x00002002
nr=0x32 ret=0  {0210006b, 1}
nr=0x33 ret=0  {00002002}         ★ StreamOut 開始、そのまま
```

**通る。** そのうえでトランスコーダを MPEG-2 素通し（`0x100a` 上位ニブル 2）で
押さえながら起動した。

### 71.3 ★★ それでも Dione には何も来ない。理由は `dione_transcode` が言う

```
[SUCCESS] start_device()     status = 00000000
[SUCCESS] StartStream()      status = 00000000
can't open /dev/tsout_demux            ← ★
Bus 001 Device 004: ID 06b8:102d       ← Dione は生きている
nr=0x1a ret=-145  (×5、3 バッファ全部)  ← ETIMEDOUT＝取り出す物が無い
  0x0acea000 : 0 hits      0x0b096000 : 0 hits      0x0b442000 : 0 hits
```

**`/dev/tsout_demux` はこの基板に存在しない**（§53.1）。つまりベンダの
`dione_transcode` は「demux の TS 出力を受け取る**キャラクタデバイス**」を
期待していて、それが無い。**Dione の TS 入力は USB** であり（`blur` の
`SendTTSData`／DRAM の TS-in バッファ `0x0b442000`）、**復調器のピンでも
demux の StreamOut ピンでもない**（§65 と整合）。

∴ **demux の StreamOut は動くが、この基板では受け手がいない。**

### 71.4 ★★★ 段階 C の分水嶺：答えは「セクション/PES 側」

| 出口 | 届くもの | 根拠 |
|---|---|---|
| ペイロードバッファ | 解析済み | 関数名と §43 |
| PID フィルタバッファ（`nr=0x26`） | **PES**。全 PID 素通しでも変わらない | §70.5（実測・`00 00 01 bd`） |
| バッファオブジェクト（種別 7） | **PSI/SI セクション** | §55.1 |
| StreamOut（ピン） | **受け手がいない** | §71.3 |
| ハードウェア関数表に他の出口 | **無い**（76 本・`Transport`/`Packet`/`Raw` は 1 本も無い） | §70.6 |

∴ **この機器のソフトウェア経路で 188 バイトの生 TS は取り出せない。**
分岐の前提だった「`SetPidFilterBuffer` なら生 TS かもしれない」は、その関数が
`nr=0x26` そのものだと分かった時点で消えた（§70.4）——我々は最初からその出口を
読んでいて、出ていたのは PES だった。

**限定受信の解除には一切触れていない。** スクランブルされた TS を
「そのまま」運ぶ経路が**ハードウェアに無い**という話であって、鍵の問題ではない。

### 71.5 ∴ 段階 C は複合になる

ソフトウェア側で**できることは揃っている**:

| 役割 | 状態 |
|---|---|
| 選局・ロック・C/N | **完成**（§30・`tools/yb1-mxl`、ベンダアプリ抜きで冷起動から） |
| PSI/SI セクション | **取れる**（§55.1） |
| PES（PID 単位） | **取れる**（§43・最小列は §56.5） |
| 平文 TS のバイト完全な通過 | **実証済み**（§62・トランスコーダ素通し） |
| **生 TS（188B・スクランブルのまま）** | **ソフトウェアでは出ない**（本節） |

∴ **生 TS はハードウェアタップが本線**である。復調器と SoC の間の TS 線
（クロック・データ・同期・有効）を物理的に引き出し、そこを px4_drv 互換の
chardev の入力にする。ソフトウェア側は**選局と制御とメタデータ**を受け持つ。

次にやること:

1. **復調器 MXL683 と SoC の間の TS 線を基板上で同定する**（§7.5 の立ち上げ
   手順で復調器は我々の制御下にあるので、ロック中に信号が出ている線を探せる）。
2. タップ先の受け手を決める（別の SoC / FPGA / USB ブリッジ）。
3. ソフトウェア側は `ird-tuner-*` として**選局・C/N・PSI/SI**までを実装し、
   chardev の TS はタップから流す。

## 72. ★★★ 裏 CPU への合図が割れた（2026-09-24）

§68〜§70.1 の「合図は当て推量では出ない・ユーザランドからは観測できない」は
**結論が早すぎた**。観測できなかったのは**見ていた窓が狭かった**からで、
GPL カーネルのヘッダが、我々が一度も読んでいなかったレジスタ組を名前つきで
持っていた。

### 72.1 GPL が名前を持っていた

`include/asm-mips/emma/emma3sep.h`（GPL・読んでよい）に、**2 組の並行した
割り込みレジスタ**がある。

| | ステータス | 許可 |
|---|---|---|
| このCPU | `EMMA_BHIF_INT_ST_0/1/2` = `+0x0030/34/38` | `INT_EN_0/1/2` = `+0x0040/44/48` |
| **裏CPU** | **`EMMA_BHIF_SUB_INT_ST_0/1/2` = `+0x0100/104/108`** | **`SUB_INT_EN_0/1/2` = `+0x0110/114/118`** |

我々が §12.3 以来見ていたのは `SW_INT` 一族（`+0x01a0`）だけで、**`+0x0000`〜
`+0x00ff` と `+0x0100`〜`+0x019f` は一度も読んでいなかった**。

### 72.2 ★ 裏 CPU が受ける割り込みは 1 本だけ

実測（`r133`）:

```
SUB_INT_EN_0 = 0x00000080   SUB_INT_EN_1 = 0        SUB_INT_EN_2 = 0x00000eee
```

**裏 CPU が有効にしている「源 0」の割り込みは bit 7 の 1 本だけ**である。
そして `SUB_INT_EN` を全部 0 にして（＝裏 CPU に割り込みが届かないようにして）
本物のコマンドを 1 本撃つと:

```
SUB_INT_ST_0 : 00000000 -> 00000080     消費されずに立ったまま残る
```

さらに**マスク中は ioctl がそのまま返らない**（ドライバは応答を待っている）。
∴ **往復を駆動しているのはこの割り込みである。**

### 72.3 ★★ 合図は `0x100001b0` に `0x00020000` を書くこと

マスクしたまま本物のコマンド 1 本を撃ち、**ホストインターフェースの窓
512 バイト全部**を前後で差分すると、**動くのは 3 語だけ**だった（`r140`）:

```
+0x0030  INT_ST_0      00000000 -> 00000080
+0x0100  SUB_INT_ST_0  00000000 -> 00000080     （同じ源の 2 つの見え方）
+0x01b0                00000000 -> 00020000     ★
```

エントリ表の手前 1KB（物理 `0x00010000`〜`0x000103ff`）は**全部ゼロ**で、
**添字を伝える書き込みはどこにも無い**＝裏 CPU は自分で消費位置を持っている。

`0x100001b4` は `0x00070000`＝**bit 16/17/18 の許可マスク**で、`0x1b0` がその
要求レジスタである。実測で性格も割れた:

- `0x1b0` に `1 << 6` のような**下位ビットを書くと latch したまま残る**
- **bit 16/17/18 を書くと即座に 0 に戻る**＝裏 CPU が消費している
- マスクしたまま我々が `0x00020000` を書くと **`SUB_INT_ST_0` と `INT_ST_0` の
  bit 7 が立つ**（`r141`）＝**我々の書き込みで割り込みが上がる**

∴ **合図＝`0x100001b0` に `0x00020000` を書く。**

### 72.4 ★★★ 欠けていたのは `word0 = 0x00020000` だった

それでも手組みのエントリは走らなかった。理由は**エントリの中身**で、
しかも**我々はそれを見たことがなかった**——これまでダンプしたエントリは
**全部「完了後」**だったからである。

裏 CPU をマスクすれば、csd が書いた**未実行のエントリ**がそのまま残る:

```
未実行 : 00020000 19010103 a0010810 a0010814 | 7fffffff 00000000 00000008 00000000
完了後 : 00000000 19010103 a0010810 a0010814 | 7fffffff 00000000 00000008 00000000
         ^^^^^^^^ 裏 CPU が完了時に書き換える
```

∴ **`word0` は「保留」の印で、値は `0x00020000`（合図と同じ値）**。裏 CPU は
完了時にこれを潰す。§66 と §70.2 の「`word0` は常に 0／有効フラグではない」は
**この観測の当然の帰結を取り違えていた。撤回する。**

### 72.5 ∴ 手順（ベンダのコードを 1 行も通さない）

```
1. データリングの次の空き番地に、要求の語を書く
2. 次のエントリ（添字 0 は飛ばす。31 の次は 1）に 8 語を書く
     word0 = 0x00020000              ← 保留の印
     word1 = (コマンド << 16) | 通し番号
     word2 = 要求の番地（KSEG1）
     word3 = word2 + 要求長
     word4 = 0x7fffffff
     word5 = 0
     word6 = 要求長 + 応答長（バイト）
     word7 = 0
3. 0x100001b0 に 0x00020000 を書く
4. word5 を待つ。完了すると word0 が 0x00040002 になり、
   word5 が状態（0 が成功、以外は errno。ioctl の戻りは −word5）、
   応答は word3 の指す語に入る
```

**実測（`r142`・マスク無しの通常状態）**:

```
OpenDevice  コマンド 0x1901 要求 {1} -> word5 = 0、応答 = 0x00010000
もう一度                            -> 応答 = 0x01010001
コマンド 0x1904 に誤引数            -> word5 = 3
```

**`0x00010000` はベンダの ioctl `nr=0x02` が返すのと同じデバイスハンドル**で
ある。∴ **自前のコードだけで AV サブシステムのデバイスを開けた。**

（この走行はブロックの起動 `Initialise` に ioctl を 1 本だけ使っている。
`Initialise` はコマンド `0x1900`・要求長 0・応答長 0 なので同じ手順で積める
はずだが、未実測。）

### 72.6 これが変えること／変えないこと

**変えること**——段階 C の実装範囲:

| 層 | これまで | いま |
|---|---|---|
| フロントエンド（MXL683・I2C） | クリーン（§30） | 変わらず |
| 裏CPU ファームのロード | クリーン（mtd5 を mount して `dd`） | 変わらず |
| **デマルチプレクサ制御** | **ベンダ `val_merge.ko` が必要＝段階 B** | **クリーンに書ける＝段階 C** |

∴ **PSI/SI セクション・PES・PCR/PTS を、ベンダの `.ko` を 1 つも積まずに
取り出せる。** §56.6 の「残る道は 1 本」は、その 1 本が開いた。

**変えないこと**——§71 の判定:

ハードウェアの関数表 76 本に **DRAM へ 188 バイトの生 TS を落とす出口が無い**
のは、呼び方の問題ではなく構造である（§70.6）。StreamOut はピンで、この基板
には受け手がいない（§71.3）。∴ **生 TS はハードウェアタップが本線**のまま。

**限定受信には一切触れていない。**

## 73. AV の SRAM 窓が読めた。リング番地を持つのはそこだけで、MMIO には無い（2026-09-27）

**問い**: 裏 CPU が叩く DMA／TS 入力のレジスタのうち、(A) DMA の宛先（リング番地）を
持つもの、(B) TS 入力の制御・状態を持つものを、**読むだけの実機観測**で当てる。
MMIO には一切書いていない（ioctl は §43 の列と `nr=0x0e`・`nr=0x48` だけ）。

### 73.1 道具

`tools/yb1-mem` に 3 つ足した。

| | |
|---|---|
| `range [stride] [us]` | 1 行 1 語。**番地を書き出してから読む**ので、固まっても止まった番地が手元に残る。`us` は読む前に待つ時間（telnetd に行を送らせるため） |
| `find <lo> <hi>` | 値の KSEG ビットを畳んで `[lo, hi]` に入る語を出す |
| `raw` | 32bit 読みでそのまま吐く（スナップショット用。ホストで差分する） |

### 73.2 ★ `0x10030000`〜`0x1003ffff` は裏 CPU のデータ SRAM で、メイン CPU から読める

§17.3 以来「意味未確定」としていた `SetPidFilterBuffer` の応答
`0xb0033420`／`0xb00336a0` は、この窓を指していた。

- 64KB 全域を 1 語ずつ読んでも**固まらない**。起動直後は電源投入時の乱数状のゴミと
  ゼロが混在し、**`nr=0x01`（Initialise）の瞬間に約 1 万語が書かれる**
  ＝メモリであってレジスタではない。
- 外側も読んだ（4KB ごとに 1 語、20ms 待ち）: **`0x10000000`〜`0x100fffff` は全部読めて
  固まらない**（Ether のページは telnet を守るため全語読みから外した）。ほぼ 0 で、
  中身があるのは `0x10020000`〜`0x1002ffff`（別の窓）と数ページだけ。

### 73.3 ★ 実験 1: リング X/Y の差分＝リング番地を持つ語は SRAM に 12 語だけ

§43 の列（`stage-c-payload.script`）を、ペイロードフィルタのリングだけ変えて 2 回
（電源入れ直しを挟む）。X＝`0x0d000000`、Y＝`0x0e000000`、どちらも 128KB。
スナップショットは BHIF・XDMA・TIMER（各 4KB）、SRAM 64KB、制御ページ 8KB、
コマンドスタック 64KB。**走行内で動かず、X と Y で違う語**だけを採った。

| 番地 | X | Y | 読み |
|---|---|---|---|
| `0x100336a0` | `0d000000` | `0e000000` | フィルタごとのバッファ記述子 `[base][end][wp][wp][rp]`（20 バイト刻み・`0x100333e4 + 20×フィルタ番号`。応答で返るのはこれ） |
| `0x100336a4` | `0d01ffff` | `0e01ffff` | 〃 end |
| `0x100336a8`/`ac` | `0d000034` | `0e000034` | 〃 書き込み位置（PES 1 個＝52 バイト入った位置） |
| `0x100336b0` | `0d000000` | `0e000000` | 〃 |
| `0x10030b1c` | `0d000000` | `0e000000` | 別の表の base |
| `0x10030b20` | `0d01ffff` | `0e01ffff` | 〃 end |
| `0x10030bb0` | `0d000000` | `0e000000` | 〃 base |
| **`0x1003fd54`** | `0d000000` | `0e000000` | **転送の組**（下記）の base |
| **`0x1003fd58`** | **`0d01fff0`** | **`0e01fff0`** | 〃 end。**16 バイト境界に丸めてある**（§52.1 の「DRAM 出口は 16 バイト単位」と合う） |
| `0x1003fd5c` | `0d000034` | `0e000034` | 〃 書き込み位置 |
| `0x1003fd60` | `0d000000` | `0e000000` | 〃 読み位置 |
| `0x090000b8`〜`c8` | `0d…` | `0e…` | 既知（§17.3・DRAM の制御ページ） |
| `0x000108f4` | `ad000000` | `ae000000` | 既知（コマンドの要求語） |

- **BHIF・XDMA・TIMER に 0 語。** Y の状態で `0x10000000`〜`0x100fffff`（Ether 除く）を
  **全語** `find` しても、当たりは SRAM の中だけだった。
- `0x1003fd40` からの組は
  `f0000007 | 000081ac | 000036a0 | 50000014 | 00406ab9 | base | end16 | wp | rp` で、
  **SRAM 上のパケット位置・記述子（`0x36a0`＝上の表）・宛先**が 1 行に並ぶ。
  SRAM→DRAM の転送の指示書に見えるが、**推測**（ファームの変数かハードが読む記述子かは、
  読むだけでは区別できない）。
- `0x10030060 = 0x0e000000` は X でも Y でも同じ値の定数で、リングとは無関係。
- `0x10037c60` から `0x54` 刻みの表に `af580000` / `b0340000` / `b1100000` /
  `b1ec0000` / `b2c80000`（`0xdc0000` 間隔）がある。I/O レジスタに見えるが**等間隔の
  バッファ群**で、レジスタの手がかりではない。

DRAM 全域（`0x00000000`〜`0x0cffffff`）の完全一致検索で出たのは、制御ページと、
tmpfs に置いた自分のスナップショットと、道具のプロセスメモリだけだった。

### 73.4 実験 2: 取込の ON/OFF

停止は **`nr=0x0e {filt, 0}`**（`ret=0`）。`nr=0x0d {filt, 0}` は `-3` で、無効化ではない
（§20.6 の表どおり。最初の走行で取り違えた）。

| 番地 | ON | OFF | 読み |
|---|---|---|---|
| **`0x100306b0`** | **`80008130`** | **`80000130`** | **PID 表**。16 ビット単位で `bit15`＝有効・下位 13 ビット＝PID・未使用は `0x1fff`。起点 `0x1003066c`、**添字＝フィルタ番号**（`0x22`→PID `0x0000`、`0x23`→PID `0x0130`。機器 2 の `0x6b`（PID 0）も `+2×0x6b` で `8000`、PID 未設定の `0x6a` は `1fff` で裏が取れた）。**項は `0x00`〜`0x6b` の 108 個** |
| `0x10032050` | `00000000` | `00000008` | 状態の語（意味未確定） |

バッファ記述子（§73.3）は OFF にしてもリング番地を持ったまま。**MMIO は ON/OFF で
1 語も変わらない。** なお字幕 PES は約 100 秒で 1 個しか来ず、書き込み位置は走行中
動かなかった（動きで wp/rp を分ける判定には流量のある PID が要る）。

### 73.5 実験 3: ロック有（557 MHz）／無（707 MHz、`0x112d = 0`）

- **MMIO は差なし。** 動くのは TIMER `+0xd00`（ZPIO のデータ＝GPIO）だけで、状態と
  無関係に揺れている。BHIF が動いたのは Initialise の瞬間の 10 語だけ
  （`+0x060` `+0x118` `+0x130` `+0x230` `+0x314`〜`+0x320` `+0x414` `+0x428`）。
- SRAM に「ロックで値が切り替わる状態の語」は**無い**（X・Y で再現する安定差 0）。
  代わりに**動き方**で分かれる（X・Y とも同じ）:
  - **受信中だけ動く**: 下の FIFO 2 本とその制御ブロック、`0x10036774`〜`0x100368ac`
    （2 語組の待ち行列）、`0x10034c90`〜`0x10034de0`、`0x1003058c`〜`0x100305ec`
    （計数）、`0x1003330c`/`310`、`0x1003e908`、`0x1003e9c4`〜`9cc`。
  - **信号が無くても動く**: 時刻だけ（`0x1003e964`、各制御ブロック `+0x54`、
    `0x1003fe18`〜`0x1003fff4` の組）。

### 73.6 ★★ TS 入力の FIFO＝SRAM に生の 188 バイトが到着時刻つきで入る

| FIFO | スロット | 制御ブロック | 中身 |
|---|---|---|---|
| 0 | `0x10038004 + k×0xdc`（7 個） | `0x1003ec00` | 機器 1 のフィルタの PID（PAT・`0x0130`） |
| 1 | `0x1003a264 + k×0xdc`（7 個） | `0x1003ec80` | 空（使われていない） |
| 2 | `0x10039134 + k×0xdc`（7 個） | `0x1003ed00` | 機器 2 のフィルタの PID（PAT） |

- スロットは待ち行列の値 `p` が指す 220 バイト: `p+0x00 = 80020bc0`、
  **`p+0x0c` 到着時刻 4 バイト**、**`p+0x10` から TS パケット 188 バイト**
  （`0x47` から、ヘッダ込みで素のまま）、残りは詰め物。
  到着時刻は **27.000 MHz**（制御ブロック `+0x54` の刻みを実機の `/proc/uptime` と
  約 77.6 秒突き合わせた実測・X/Y とも）。FIFO 内の PAT 7 個の間隔は 2,695,751〜
  2,706,119 刻み＝**100.0ms**、continuity_counter は連番（4→10）で欠けなし
  ＝**PAT は毎秒 10 個**（実測）。
- 制御ブロック（`0x80` 刻み）: `+0x00` 構成（起動時 `000a0000` → `130a0842`/`43`）・
  `+0x04` 有効・`+0x08`/`+0x10` 溜まっている個数・`+0x14`〜`+0x2c` **7 スロットの
  番地の待ち行列**・`+0x34 = 0xbc`（188）・`+0x38 = 471fff00` と `+0x3c = ff1fff00`
  （sync `0x47` と PID の値・マスクの形）・`+0x54` 時刻。
- **入るのは PID 表に載せた PID だけ。** 全 PID 素通し（`nr=0x48 {機器 1, 0}`・
  `ret=0`）にして 300ms おきに 12 枚撮っても、見えたのは PID `0x0000` 160 回・
  `0x0130` 8 回だけで、**スクランブルされたパケットは 0**。素通しの設定は FIFO の
  入口を広げない（§70.5 と整合）。
- **全パケットがこのスロットを通っているのではない。** PAT は毎秒 10 個（実測）、
  映像 PID `0x0100` だけで毎秒約 9,140 個（実測・§73.9）。全パケットがここを通って
  から捨てられるのなら、スナップショットのスロットはほぼ全部が PAT 以外で埋まるはず
  だが、PAT と字幕だけを表に載せた 12 枚×21 スロットで、表に無い PID は 1 度も
  見えなかった。
- **選別をハードがやっているかは推測。** 「ハードが全パケットを別の場所に書き、
  FW が表の PID だけをこのスロットへ写す」形も上の観測と矛盾しない。到着時刻を
  ハードが付けているか FW が付けているかも未判別。なお PID 表の bit15 は FIFO の
  入口の門ではない（§73.9）。

### 73.7 判定

| 問い | 答え |
|---|---|
| (A) DMA の宛先をメイン CPU から見えるレジスタが持つか | **持たない。** BHIF／XDMA／TIMER と `0x100xxxxx` 全域（Ether 除く）に無い。持っているのは**裏 CPU のデータ SRAM の変数**（§73.3 の 12 語）だけ |
| (B) TS 入力の制御・状態のレジスタ | **MMIO には見えない**（ロック有無・ON/OFF とも 0 語）。見えるのは SRAM 上の FIFO と、その制御ブロック・PID 表 |
| 「AV レジスタはメイン CPU から読めない」か | **ハードのレジスタ本体については、その通り**（見つからない）。ただし**裏 CPU の作業メモリ（SRAM 64KB）は丸ごと読める**——これは §10.7.3 の時点では知られていなかった |

∴ **ハードのレジスタ番地は、読むだけの観測では出ない。** 静的なファーム解析
（別セッション・clean-room の手順どおり）でしか取れない、という結論になる。

### 73.8 段階 C への含意

- **TS 入力のハードは、選んだ PID のパケットを 188 バイトのまま SRAM に置く。**
  PES やセクションに組み立てているのはその後段（裏 CPU）である。∴「生 TS を DMA で
  落とすだけ」の経路は、**ハードの能力としては SRAM の手前まで来ている**。
- 問いは 2 つあった:
  1. **映像・音声の PID をフィルタに載せたとき、FIFO に入るパケットの
     `transport_scrambling_control` が非 0 のままか**（＝FIFO はデスクランブラの手前か）。
     → **非 0 のまま入る**（§73.9）。
  2. **PID 表は 108 項**（§73.4）で、8192 PID の「全部」は載らない。ただし 1 つの
     多重に実際に流れる PID は数十個で、PAT/PMT から数えられる。∴ ハード側の
     経路として残るのは「**存在する PID を全部フィルタに載せて FIFO に入れる**」形
     （ヌルや未告知の PID は落ちる＝部分 TS）。**未検証。**

### 73.9 ★★★ 映像 PID を載せると、スクランブルされたままの 188 バイトが FIFO に入る（2026-09-27）

§43 の列のペイロードフィルタの PID を `0x0130`（字幕）から **`0x0100`（MPEG-2 映像・
§55.2 の PMT）**に替えただけの走行（557 MHz）。PAT のフィルタはそのまま残し、
測り方の検算に使った。

**道具**: `yb1-mem 0x10030000 0x10000 fifo <ms> [dump]`（新規）。21 スロットの
到着時刻をタイトループで見張り、変わったスロットの**到着時刻と 4 バイトの TS ヘッダ
だけ**を記録する。**ペイロードは読まない。** スロットの集合は待ち行列を 2000 回読んだ
和集合で作る（1 回読みだと回転中の待ち行列を読んで重複・欠落が出た）。`nice -n -20`
で走らせる。

| 状態 | 見えた本数 | 連番から見た取りこぼし | 時間 | レート | scrambling_control |
|---|---|---|---|---|---|
| 取込中 | 19,960 | 56（8 か所） | 2,189 ms | **9,142 本/s＝13.75 Mbps** | even 11,922 / odd 8,038 / **平文 0** |
| `nr=0x0e` の後 | 19,960 | 49（6 か所） | 2,187 ms | 9,147 本/s | even 11,886 / odd 8,074 / 平文 0 |
| ioctl のプロセス終了後 | 19,960 | 46（7 か所） | 2,183 ms | 9,163 本/s | even 3,756 / odd 16,204 / 平文 0 |

同じ窓の PAT は毎秒 10 本（FIFO 0・2 とも）＝**測り方は §73.6 の実測と一致**。
取りこぼしは見張りが最大 7.7ms 止まった所に集中しており、それ以外は連番が 1 ずつ
進む。到着間隔は 32µs（864 刻み）単位に量子化されている。

**分かったこと**:

- **FIFO には、スクランブルされたままのパケットが 188 バイトで入る。**
  scrambling_control は全部 2 か 3。デスクランブラ（`val_m2dsc`／`val_dscram`）は
  開いておらず、鍵も無い。∴ **この FIFO は、少なくとも OpenIRD の構成では
  デスクランブルされていない TS の置き場**である。
- **メイン CPU はそれを `/dev/mem` の mmap で読める。** 映像 1 本ぶん（13.75 Mbps）の
  到着を、ユーザランドのタイトループで 99.7% 追えた。
- **PID 表の bit15 は FIFO の入口の門ではない。** `nr=0x0e` で `80008100` →
  `80000100` になっても、プロセスが終わっても、映像は同じ勢いで入り続けた
  （表の PID 値は残っている）。門が PID 値なのか、見えないレジスタなのかは未判別。

**扱い**: 見たのはヘッダの PID・scrambling_control・afc・cc と到着時刻だけで、
ペイロードは実機の外に出していない（§10.3 と同じ扱い）。

### 73.10 次

1. **1 つの多重に流れる PID を全部フィルタに載せる**（PAT/PMT/NIT から数える。
   108 項に収まるか）。載せた PID の到着レートの合計が、復調器の出す TS の全体に
   どこまで近づくかを見る。
2. **FIFO はスロットが 7 個しかなく、裏 CPU が回し続ける。** 取りこぼさずに全部を
   DRAM へ出すには、(a) メイン CPU 側のポーリング（今回ユーザランドで 99.7%。ただし
   CPU を 2 秒間占有した数字で、取りこぼしはすべて 7.7ms 前後の横取りの所。スロット
   7 個は映像だけでも約 0.8ms で一巡する）、
   (b) 裏 CPU のコマンドで FIFO から DRAM へ運ばせる経路があるか、のどちらかになる。
   (b) は関数表に「生 TS を DRAM に落とす」名前が無い（§70.6）ので、見込みは (a)。

**限定受信には一切触れていない。** デスクランブラ（`val_m2dsc`／`val_dscram`）は開いていない。

## 74. ★★★ カーネルドレイナーで FIFO を取りこぼしなく吸い出せた（2026-09-27）

§73.10 の 2 の答え。ユーザランドのポーリングは、CPU を占有しても横取りの 7〜8ms で
落ちる（§73.9）。**メイン CPU のカーネルで、高分解能タイマから吸えば落ちない**ことを
実機で示した。

### 74.1 作り（`tools/yb1-fifodrain`・`fifodrain.ko`）

- **hrtimer**（`HRTIMER_CB_IRQSAFE_UNLOCKED`＝ハード割り込み文脈）を `period_us` ごとに
  回す。ベンダカーネルは `CONFIG_HIGH_RES_TIMERS=y`・分解能 1ns（`/proc/timer_list`）。
- 1 周期の仕事: 7 スロットの到着時刻を読み、前回取った値と違うスロットを**古い順**に取る。
  **最新の 1 本だけは次の周期まで待つ**（到着時刻がパケットより手前に置かれるので、
  書き込み途中を掴まないため。後ろに次のパケットが来ていれば、それは書き終わっている）。
  写した後に到着時刻を読み直し、変わっていれば「破れ」として捨てる。
- 写した先は自前のリング（既定 8192 本＝1.5MB）。**`/dev`（misc）から 188 バイト単位で
  読める**。読み手は「64 本溜まるか 10ms 経つ」ごとに起こす。`/proc/yb1fifo` に
  本数・破れ・溢れ・PID ごとの連番の途切れ・コールバックの所要時間を出す。
- **SRAM は読むだけ**。裏 CPU にもデスクランブラにも触れない。どの PID を FIFO に
  入れるかは、先に demux の ioctl で設定しておく（§73.9 の列）。
- スロットの一覧は、ロード時に待ち行列を最大 200ms 読んで 7 個揃えてから確定する
  （揃う前にロードすると `-ENODEV`。1 回目の試行で 6 個しか見えずに一度踏んだ）。

### 74.2 ★ 結果（557 MHz・映像 PID `0x0100`＋PAT・読み手は `/dev/null` へ）

| 周期 | 時間 | 本数（毎秒） | 連番の途切れ | 破れ | 溢れ | 遅れ | コールバックの CPU |
|---|---|---|---|---|---|---|---|
| **100µs** | 20.2 s | 184,407（9,120） | **0** | **0** | 0 | 0 | **19.1%** |
| 200µs | 20.2 s | 135,985（6,739） | **0** | 0 | 0 | 0 | 13.7% |
| 300µs | 20.2 s | 139,659（6,927） | **0** | 0 | 0 | 0 | 13.3% |
| 400µs | 20.2 s | 184,218（9,134） | **1,174** | 604 | 0 | 0 | 16.3% |

- **100〜300µs で取りこぼし 0。** 毎秒の本数が周期で違うのは、映像の可変ビットレートの
  せいである（連番が途切れていないので、取りこぼしではない）。
- **`/dev` 経由の中身も欠けない。** 100µs で 5 秒間 tmpfs に取り込んで実機上で検査した
  （`yb1-mem @file … stats`。中身は実機の外に出していない）: **34,682 本・同期の乱れ 0・
  PID `0x0100` の連番の途切れ 0**。scrambling_control は even 10,998 / odd 23,634、平文は
  PAT の 50 本だけ。
- **余裕は設計どおり。** 最新の 1 本を次の周期で取るので、取るまでの遅れは最大で周期の
  2 倍。スロットは 7 個×約 110µs ≒ 770µs で再利用されるので、2×300＝600µs は間に合い、
  2×400＝800µs で欠け始めた。流量が上がると再利用が早まるので、周期はその分短くする。

### 74.3 コストの内訳

モジュール自身が `ktime_get()` で測ったコールバック内の時間（割り込みの出入りの分は
含まない）。パケットを取らなかった周期の平均を固定費として引くと、残りは本数に比例する。

| 周期 | 1 周期の固定費 | 1 パケットのコピー |
|---|---|---|
| 100µs | 5.0µs | 15.5µs |
| 200µs | 6.3µs | 15.7µs |
| 300µs | 7.3µs | 15.7µs |
| 400µs | 8.1µs | 15.7µs |

- **1 パケット約 15.6µs＝188 バイトを 47 回の非キャッシュ読みで取る代金**
  （1 語あたり約 0.33µs）。映像 1 本（毎秒約 9,100 本）で約 14%、100µs の固定費が 5%。
- 1 回のコールバックの最大は 0.10〜0.14ms（遅れて溜まった数本をまとめて写した回）。
  その間は割り込み禁止である。
- `/proc/stat` はリグの常駐ループ（CSD のログを 20ms ごとに吸う `dmesg -c`）の負荷と
  混ざるので、ドレイナーの負荷としては使っていない。

### 74.4 まだ言えないこと

- **多重の全体を運んだときの負荷は未実測。** 全 PID を載せると流量は映像 1 本より多く、
  コピー代は本数に比例するので、上の単価からの**換算**では 20% を超える。
- **FIFO に入るのは PID 表（108 項）に載せた PID だけ**（§73.6）。全 PID を載せたときの
  網羅率は未検証（§73.10 の 1）。
- FIFO の配置（`0xec00` の制御ブロック・220 バイトのスロット）は、この FW 版で実機から
  読んだもの。ロード時に待ち行列から毎回読み直すので固定番地は持たないが、形が変われば
  追従しない。

### 74.5 次

1. **コピー代を下げる。** 非キャッシュの 1 語読みが支配的なので、(a) SRAM をキャッシュ
   付きで読み、読む前に該当行を無効化する（窓がバースト読みを受け付けるかの確認が先）、
   (b) GPL に定義のある XDMA（`0x10009000`）で SRAM→DRAM を運ばせる、を比べる。
   → **§75。(a) は効かず、(b) で 1 パケット 15.5µs → 2.8µs、CPU 16.5% → 5.4%。**
2. **全 PID を載せて、網羅率と負荷を実測する**（§73.10 の 1）。
3. **裏 CPU のファームの書き換え**（CLAUDE.md の課題 (1)）との比較。こちらはメイン CPU の
   コピー代がゼロになる代わりに、§1-1/§1-3 との整合の検討が要る。

**限定受信には一切触れていない。** 扱ったのはスクランブルされたままのパケットで、
デスクランブラは開いておらず、中身は実機の外に出していない。

## 75. ★★ コピー代を 3 分の 1 にした＝XDMA にリングへ直接運ばせる（2026-09-27）

§74.5 の 1。1 パケット約 15.6µs のうち、ほとんどは SRAM の非キャッシュ読み
（1 語 0.23µs×47 語）だった。

### 75.1 キャッシュ読みは効かない（`sramspeed.ko`）

SRAM をキャッシュ付き（KSEG0）で読み、読む前に該当行を無効化する方式を測った
（計測後にロードを失敗させて常駐しないモジュール。書くものは無い）。

| 読み方 | 1 語 | 188 バイト |
|---|---|---|
| 非キャッシュ | 240 ns | 11.3 µs |
| キャッシュ行を無効化してキャッシュ経由 | 229 ns | 11.5 µs |

キャッシュ経由でも固まらず、1,024 語を 1 語ずつ突き合わせても違いは 0 だった。ただし
行単位で読んでも速くならない＝**この窓は読み出し 1 回ごとに約 0.23µs で、上限は約
17 MB/s**。CPU で読む限り、この待ちは避けられない。

### 75.2 ★ XDMA は SRAM から DRAM へ運べる（`xdmaspeed.ko`）

XDMA（`0x10009000`）は GPL のヘッダが名前を、GPL の NAND ドライバが使い方を持っている
（`Origin: gpl-derived`）。チャネル n の `MEM_LIST0..4` は `0x100 + 0x20·n` から、
`LIST_ENABLE`（`+0x088`）の bit n で開始する。`platform.c` のコメントでは **チャネル 0 が
NAND、1 がベンダアプリ**。段階 C のリグでは**チャネル 1・2 のレジスタが電源投入時の乱数の
まま**（起動ごとに値が違う）＝一度も使われていないので、**チャネル 2** を使った。

実機で分かったこと（書いたのはチャネル 2 のレジスタだけ）:

| `MEM_LIST3` | 転送 | 結果 |
|---|---|---|
| `0x30000000`（増加だけ。NAND の手順で `um` 無し） | DRAM→DRAM、SRAM→DRAM | アドレスは進み `LIST2` も 0 になるが、**DRAM に何も届かない** |
| `0x3C000000` | DRAM→DRAM | **一致** |
| **`0x38000000`** | **SRAM→DRAM** | **一致**（188B・1540B・4KB。4KB で違ったのは動いている計数 3 語だけ） |
| `0x3C000000` | SRAM→DRAM | 不一致 |

- **bit 26/27 は「転送元/転送先が DRAM」の印**である。NAND ドライバが立てるのはメモリ側
  だけ（読み出しは bit 29＋27、書き込みは bit 28＋26）で、実機の結果もそれと合う。
  SRAM は DRAM ではない側。
- **完了は `MEM_LIST2`（残り長さ）が 0 になること**でわかる。`LIST_ENABLE` の bit は
  自分で落ちる。`MAIN_INT2`（`+0x508`）の bit n は、`LIST3` の bit 16 を立てたときだけ
  立つ（CPU への割り込み許可 `MAIN_INT_EN2` は 0 のまま）。
- 速さ: SRAM→DRAM 188B で約 12.5µs、4KB で 239µs＝**約 17 MB/s**。CPU で読むのと同じで、
  SRAM 側の口が律速する。**違いは CPU が待たないこと**で、CPU の仕事は 6 レジスタの設定
  約 0.95µs だけ。

### 75.3 ドレイナーの作り（`fifodrain.ko`・`use_xdma=1` が既定）

- **リングを物理的に連続させ、1 要素を 220 バイトのスロットの写し**（到着時刻＋188B＋
  詰め物）にした。`__get_free_pages` で 1MB（4,766 要素）。DMA が書く前に全体を書き戻し・
  無効化しておく。
- 1 周期: ① 前の周期の転送を回収する（`LIST2 == 0` を見て、写しの範囲だけキャッシュを
  無効化し、写しの到着時刻を確かめる）。② 新しいパケットを確定する。③ 並んだスロットを
  **1 回の転送でリングの並んだ要素へ直接**運ばせる。スロットが 6→0 と一巡をまたぐときと、
  リングの終端では切って、残りは次の周期。**188B の取り出しは読み手の `read()` の中**で
  行い、割り込みの中で CPU はパケットの中身に触れない。
- **写しを信用する条件**: DMA は各スロットを到着時刻から順に読むので、写しの到着時刻が
  期待どおりなら、DMA が読み始めた時点ではスロットは書き換わっていない。読み終わるまでに
  再利用されないことは、**パケットの到着間隔の下限**から言える。実測（6 万本×3）で最小は
  **863〜865 刻み＝32.0µs**（27MHz。到着時刻は 32µs 単位に量子化されている）、32µs 間隔の
  連続は最長 2 回。後ろに m 本届いているパケットのスロットは、もう 1 本割り込むのを許しても
  (7 − m − 2)×32µs は再利用されない。これが DMA がそのスロットを読み終える時刻
  （2 + 13×(バッチ内の位置 + 1) µs）より後なら信用する。それ以外（こちらが遅れているとき）
  だけ、回収時に SRAM の到着時刻を読み直す。当初は全部読み直していたが、回収は 1 周期後なので、
  **写し終えた後の正当な再利用まで「破れ」として捨てていた**。
- **到着時刻は「次に書かれるスロット」から読む**（スロットはアドレス順に使われる）。
  ふつうは 1〜2 回の非キャッシュ読みで済む。64 周期ごとに 7 個全部を読み、順番の食い違いを
  数える（`resync`。以下の走行で 0〜1）。
- 所要時間の計測は CP0 Count（ロード時に `ktime` と突き合わせて 200 刻み/µs）。
- **ロード直後の欠けを直した。** スロットの現状を控えてから CP0 Count の較正（100ms）を
  していたため、最初の周期に FIFO が一巡以上進んでいて、開始 3〜17 周期目に数本ずつ欠けた
  （欠けたときの状況をモジュールに記録させて突き止めた）。控えるのをタイマ起動の直前に移した。

### 75.4 ★ 結果（557 MHz・映像 PID `0x0100`＋PAT・100µs・読み手は `/dev/null`）

| 走行 | 方式 | 時間 | 本数 | 連番の途切れ | コールバックの CPU | 1 周期の固定費 | 1 パケット |
|---|---|---|---|---|---|---|---|
| 常駐ループ停止 | CPU コピー | 20 s | 185,581 | 0 | 16.5% | 2.1µs | 約 15.5µs |
| 常駐ループ停止 | **XDMA** | 30 s ×2 | 272,523 / 275,584 | **定常 0**（欠けは修正前の開始直後のみ） | **5.4%** | 2.9µs | **約 2.8µs** |
| 常駐ループ稼働 | CPU コピー | 20 s | 187,004 | 0 | 19.8% | — | — |
| 常駐ループ稼働・開始直後修正後 | **XDMA** | 30 s | **280,748** | **0** | 11.6% | 7.7µs | — |
| 同上 | XDMA・ファイル 5 秒 | — | 46,232 | **0**（同期の乱れ 0。実機上で検査） | — | — | — |
| 同上 | XDMA・**150µs** | 30 s | 278,384 | 2（各 1 本。一巡またぎの切断が重なった周期） | 10.5% | — | — |

- **コールバックの CPU は 16.5% → 5.4%（約 3 分の 1）、1 パケットは 15.5µs → 2.8µs。**
- 「常駐ループ」はリグの CSD ログ吸い出し（`dmesg -c` を 20ms ごと）で、製品には無い。
  これが動いていると固定費が 2.9 → 7.7µs に増える（キャッシュを荒らされる）。2.6.27 の
  `dmesg` はログを写す間割り込みを止めるので、タイマの「遅れ」も時々出る。
- 周期は **100µs**。150µs では、一巡またぎで切った残りが 1 周期待つぶん余裕が足りず、
  まれに 1 本欠けた。

### 75.5 残り

- **読み手の CPU**（`cat` から `/dev/null`）は 30 秒で 1.6〜5.1 秒（5〜17%）とばらついた。
  188B ずつ `copy_to_user` しているので、まとめて渡す（大きい `read`、または `mmap`）余地がある。
- 1 パケット 2.8µs の中には、診断用の SRAM 読み直し（安全なものも数えるため）と PID ごとの
  集計が入っている。製品では外せる。
- 多重の全 PID を載せたときの網羅率と負荷は未実測（§73.10 の 1）。

**限定受信には一切触れていない。** 扱ったのはスクランブルされたままのパケットで、中身は
実機の外に出していない。XDMA で書いたのはチャネル 2 のレジスタだけで、転送先は自前の DRAM。

## 76. ★★★ 参照受信機と突き合わせた＝YB1 の生 TS はビット単位で一致した（2026-09-27）

YB1 の中には「本物の全体 TS」を得る手段が無い（ベンダの経路は再多重後・§55）ので、
**別ホストの PC 用チューナ**（PLEX PX-M1UR・recisdb 1.2.4・`--no-decode`）で同じ局を
取り込み、正解データにした。スクランブルされた中身はどちらのホストからも出さず、
比べたのは**ヘッダ・平文の PSI/SI・パケットごとの CRC-32** だけ。

道具: 参照側は `tools/host/tsref.py`（PID ごとの本数・流量・スクランブル・連番、
PAT/PMT/CAT の解析、パケットごとの `PID 連番 CRC-32` の一覧）。YB1 側は
`yb1-mem @file 0 <len> hashes`（同じ形式。zlib と同じ CRC-32 で、試験パケットで一致を確認）。
照合は `tools/host/tshashcmp.py`（一意なハッシュで位置を合わせ、順に歩く）。

### 76.1 ★ 557 MHz（TSID `0x7fe0`）の多重の実測

参照側 30 秒（PCR で 29.958 秒）:

- **344,412 本＝毎秒 11,496 本＝17.29 Mbps**（ヌル込み）。どの PID も連番の途切れ 0・TEI 0。
  §73.6 で換算とした「毎秒約 1 万本」は、この実測に置き換わる。
- **存在した PID は 36 個**（ヌル `0x1fff` を除くと 35 個）。PID 表の 108 項に収まる。
  ヌルは毎秒 464 本（全体の 4.0%）。
- PAT は 5 項目: NIT `0x0010`、番組 `0x0400`→PMT `0x01f0`、`0x0401`→`0x03f0`、
  `0x0580`→`0x1fc8`（ワンセグ）、`0xfff0`→`0x1cf0`。

| 分類 | PID（毎秒の本数） | スクランブル |
|---|---|---|
| 映像 | `0x0100`（9,234） | あり |
| 音声 | `0x0110`（138）`0x0111`（138） | あり |
| 字幕・文字スーパー | `0x0130`（1.3）`0x0138`（1.0） | なし |
| データ（type `0x0d`） | `0x0140`（151）`0x0160`（691）`0x0161`（12）`0x0162`（26）`0x0170`（196）`0x0171`（51）`0x0172`（2.0） | あり |
| ワンセグ | `0x0580`（30）`0x0581`（134）`0x0583`（39）`0x0587`（0.5）`0x058b`（2.0）、PCR `0x05ff`（4.3） | なし |
| PSI | PAT `0x0000`（10）、CAT `0x0001`（0.1）、PMT `0x01f0`/`0x03f0`（各 20）`0x1fc8`（2.0）`0x1cf0`（1.0）、PCR `0x01ff`（17.3） | なし |
| SI（TR-B14 の固定 PID。PAT/PMT には出ない） | `0x0011`（table `0x42`）`0x0012`（`0x4e,50,51,58,59`）`0x0014`（`0x73`）`0x0024`（`0xc4`）`0x0027`（`0x4e`）、NIT `0x0010` | なし |
| PMT が告知する CA 関係の PID | `0x0901`（10・CA 記述子 `0x09`）＝table_id `0x82`（ECM）・42B、**`0x0902`（10・アクセス制御記述子 `0xF6`）＝table_id `0x82`（ECM）・182B** | なし（中身には触れない） |
| CAT が告知する CA 関係の PID | **`0x0900`（24・アクセス制御記述子 `0xF6`）＝table_id `0x84`（EMM）・3.4〜4KB** | なし（中身には触れない） |
| 番組 `0xfff0` の ES | `0x1c61`（9.5）＝table_id `0x3c`（DSM-CC のデータブロック＝データカルーセル）。PMT が告知する他の 11 本（`0x1c60`〜`0x1c63`・`0x1c71`〜`0x1c78`）は 30 秒間に 1 本も無い | なし |
| ヌル | `0x1fff`（464） | — |

CAT のセクションは CRC が合い、CA 記述子（`0x09`）は 0 個、アクセス制御記述子（`0xF6`）が 1 個
（`0x0900` を指す）。

> **訂正**: 当初の解析は CA 記述子（`0x09`）しか読んでおらず、`0x0900`・`0x0902` を「PAT・PMT・
> CAT のどこからも辿れない」と書いた。ARIB TR-B14（英訳 v6.7-E1）Vol.4 の Table 5-6 と 30.2.2.2 に
> よれば、地上デジタルでは **アクセス制御記述子（タグ `0xF6`。構造は CA 記述子と同じで、
> CA_system_ID・伝送路 3 ビット・PID 13 ビット）** も ECM/EMM の PID を告知する（CAT に置けば EMM、
> PMT に置けば ECM）。これも読むと、**存在する PID はすべて PSI か固定 PID で告知されていた**。

固定 PID は TR-B14 Vol.4 Table 5-4「Assignment of PID Values to PSI/SI」による: `0x0000` PAT、
`0x0001` CAT、`0x0010` NIT、`0x0011` SDT、`0x0012`/`0x0026`/`0x0027` EIT、`0x0014` TOT、
`0x0023`/`0x0028` SDTT、`0x0024` BIT、`0x0029` CDT（ST はこれらの PID に載る）。今回の取り込みに
現れなかった `0x0023`・`0x0026`・`0x0028`・`0x0029` は、まれにしか送られない表である。

table_id の意味は ARIB STD-B10（英訳 v4.6-E2）Table 5-2「Allocation of table_ID values」による:
`0x3A`〜`0x3F` DSM-CC section、`0x82`〜`0x83` ECM/ECM-S、`0x84`〜`0x85` EMM/EMM-S。
見たのは各 PID の table_id とセクションの大きさだけで、ECM/EMM の中身は見ていない。
（「`0x0900` はデータ放送のカルーセル」とする説明を目にしたが、実物は EMM の table_id だった。
この局のデータ放送は PMT が告知するスクランブルされた PID `0x0140`/`0x016x`/`0x017x` の側で、
平文のカルーセルは番組 `0xfff0` の `0x1c61` にある。）

### 76.2 ★★★ ビット単位の一致

参照側で 40 秒取り込んでいる間に、YB1 で §75 のドレイナー（XDMA・100µs）から
`/dev/yb1fifo` を 10 秒読み、両方でパケットごとの CRC-32 を取った。

| PID | YB1 | 参照側の同じ区間 | ビット単位で一致 | YB1 の取りこぼし | YB1 にしか無い（化け） |
|---|---|---|---|---|---|
| **`0x0100`（映像・スクランブル）** | **91,659** | **91,659** | **91,659** | **0** | **0** |
| `0x0000`（PAT） | 100 | — | 照合せず（同じ内容のパケットが繰り返し、位置が決まらない） | — | — |

∴ **SRAM の TS 入力 FIFO → XDMA → リング → `read()` の経路は、放送の TS を 1 ビットも
変えずに運ぶ。** 連番の検査（§74/§75）より強い、正解データとの直接の照合である。

### 76.3 見つかった弱点（取り込みの後に出た）

同じ走行のモジュールの記録で、**取り込みを終えた後**（YB1 上でハッシュ計算と `gzip -9` が
CPU とメモリを占有していた時間帯）に連番の途切れが 5 回（38 本）出た。記録された状況は
「前の転送が 1 周期で終わっていない（`still_running`）」→「7 スロットが全部新しい（遅れた）」。
**重い処理で DMA が遅れると、その周期を丸ごと飛ばす作り**なので、そこで取り残す。
転送が終わっていなくても到着時刻の走査は続け、遅れが大きいときは CPU コピーに切り替える
などの手当てが要る。

### 76.4 PID の列挙への含意

- **ヌル以外の 35 PID を全部表に載せれば、ヌル以外のパケットは網羅できる**（108 項に余裕）。
- **列挙の規則は規格だけで閉じる。** ① TR-B14 Table 5-4 の固定 PID 12 個、② PAT → NIT・PMT、
  ③ PMT → PCR・ES、および番組ループと ES ループの CA 記述子（`0x09`）・アクセス制御記述子
  （`0xF6`）の PID、④ CAT → 同じ 2 種の記述子の PID。557 MHz では、これでヌル以外の 35 個が
  すべて揃う（実在しない告知 PID を含めても 50 個前後で、108 項に収まる）。
- **PMT は変わる。** 15 秒後の取り込みでは、番組 `0x0400` の PMT から副音声 `0x0111` が
  消えていた。PAT/PMT/CAT の版番号を見張って表を追従させる。
- **CA 関係の PID も、生 TS の一部として運ぶ対象に入る。** 下流（OpenIRD の外）で所有者の
  カードを使う道具は ECM/EMM を必要とする。OpenIRD は他の PID と同じくパケットを運ぶだけで、
  中身は解釈しない（px4_drv が全 PID をそのまま渡すのと同じ立場）。
- 番組 `0xfff0` のように、告知されていても流れていない PID もある（載せても害は無いが項を使う）。
- ヌルは FIFO に入れる手段が無い限り落ちる（全体の 4%）。

**限定受信には一切触れていない。** 参照側もデコードせず取り込み、カードリーダは使っていない。

## 77. ★★★ 多重の全 PID を FIFO に載せた＝ヌル以外 100%・ビット単位で一致（2026-09-27）

§76.4 の規則で列挙した PID を全部 FIFO に載せ、§75 のドレイナーで取り出して、参照受信機と
突き合わせた。途中で FIFO について 3 つの前提が崩れたので、それも記録する。

> §73〜§77 を実装向けにまとめ直したものが [poe-br100-yb1-ts-fifo.md](poe-br100-yb1-ts-fifo.md)。

### 77.1 ★ FIFO に入れる最小の設定＝種別 7 を 3 手

PID フィルタ（種別 7）を「確保（`nr=0x0b`）→ PID（`0x0c`）→ 有効化（`0x0d`）」の 3 手だけで
張ると、その PID のパケットが FIFO に入る。リングも PES の組み立ても要らない。機器 1 で音声
`0x0110`・EIT `0x0012`・データ `0x0170` を張り、どれも連番の途切れ無く FIFO 0 に来た。
機器 2 は、ベンダの列（§24.3）に倣って有効化の前に `nr=0x32 {filt,1}` を出した。

### 77.2 ★ フィルタは 1 機器 36 個＝PID 表 108 項の 3 等分

確保を繰り返すと、機器 1 は番号 `0x23` から下へ `0x00` まで出し、37 個目で `ret=-6`。
機器 2 は `0x6b` から `0x48` までで同じく 36 個（PID 表の `0x1003066c + 2×番号` で確認）。
∴ **1 機器（＝1 FIFO）に載る PID は 36 個まで**。機器 1 → FIFO 0、機器 2 → FIFO 2。
§76.4 の規則で 557 MHz を列挙すると 50 個（実在しない告知 PID を含む）なので、**2 機器に分ける**。
同じ PID を両方に載せると同じパケットが 2 回出るので、重ならないようにする（PAT は機器 2 側だけ）。

### 77.3 ★ FIFO のスロット数は FIFO ごとに違い、埋まる順番はアドレス順ではない

制御ブロックの待ち行列（7 語）を 300 回読んだ和集合:

| FIFO | スロット | 数 |
|---|---|---|
| 0 | `0x8004`〜`0x852c` | 7 |
| 1 | `0xa264`〜`0xa6b0`（待ち行列に同じ値が 2 回） | 6 |
| 2 | `0x9134`〜`0x9738` | **8** |

- 7 語の待ち行列は**空きスロットの列**と読める。8 スロットの FIFO では常に 1 個が列の外にある。
  「7 個見つけたら打ち切る」探し方では FIFO 2 の 1 個を取りこぼし、ドレイナーが崩れた。
- ハードは列の先頭を使い、裏 CPU は処理を終えたスロットを戻す。**PID が多いと戻る順番が
  入れ替わり、埋まる順番はアドレス順にならない**（131 秒で 170 万回、アドレス順から外れた）。
  映像 1 本のとき（§74/§75）はたまたまアドレス順に巡っていた。
- スロットは 220 バイト刻みで並ぶ（ドレイナーはロード時にこれを検査する）。

### 77.4 構成とドレイナーの変更

| | 機器 2（FIFO 2・XDMA） | 機器 1（FIFO 0・CPU コピー） |
|---|---|---|
| PID | 番組 `0x0400` の映像・音声・字幕・データ・PCR（13）、ワンセグ（6）、番組 `0xfff0` の ES（12）、PAT | 固定 SI 11 個、PMT 4 個、ECM `0x0901`/`0x0902`、EMM `0x0900` |
| 数 | 31＋PAT | 18 |

ドレイナー（`fifodrain.ko`）:
- スロット数を FIFO ごとに実測（待ち行列を少なくとも 100ms、新しいスロットが 50ms 出なく
  なるまで読む。最大 16）。
- **毎周期、全スロットの到着時刻を読み、時刻順に並べる**（「次のスロットだけ読む」近道はやめた）。
- XDMA は、準備のできたスロットを含むアドレス範囲を 1 回で作業領域へ運び、次の周期に
  **到着時刻順に並べ直して**リングへ写す（同じ PID の順番を守るため。リングへの直接転送はやめた）。
  範囲に挟まった準備前のスロットも一緒に運ぶ（無駄になる）。
- XDMA で吸う FIFO は 1 本（`xdma_fifo`）。ほかの FIFO は同じ周期のうちに CPU コピー。
- 写しを信用する条件（§75.3）は、空き列の形に合わせて「スロット数 − 1」本後の再利用を前提にした。
- `read()` が、捨てた要素しか無いときに 0 バイト（EOF）を返していたのを直した。

### 77.5 ★★★ 結果（557 MHz・同時取り込み 10 秒・`tools/host/tshashcmp.py`）

| | 参照受信機 | YB1 |
|---|---|---|
| ヌル以外のパケット（同じ時間窓） | 110,911 | **110,911（100.0%）** |
| PID | 35＋ヌル | **35 すべて本数比 1.000** |
| ビット単位 | — | 位置を決められた 22 PID は全数一致（欠け 0・化け 0）。残り 13 PID は同じ内容のパケットが繰り返し位置が決まらないが、本数は一致 |
| ヌル `0x1fff` | 4,217 | 0（FIFO に入れる手段が無い） |

モジュールの記録（ロードから 131 秒・145 万本）: 連番の途切れ 0、破れ 0。

∴ **YB1 は、557 MHz の多重をヌル以外 1 本も欠かさず、ビット単位で同じ TS として出せる。**

### 77.6 残った問題＝CPU

| | 映像 1 本（§75） | 全 PID |
|---|---|---|
| 流量（FIFO 2＋0） | 毎秒約 9,200 本 | 毎秒約 11,000 本 |
| コールバックの CPU | 5.4% | **21.4%** |
| パケットを取らなかった周期の所要 | 2.9µs | 12.3µs |
| DMA が次の周期に終わっていない | 数回 | 19,130 回 |
| 範囲に挟まって無駄に運んだスロット | — | 83 万個 |

- 見立て（**未検証**）: SRAM の口（約 17 MB/s）を、DMA の広い範囲の転送と、毎周期 15 回の
  到着時刻の読み（FIFO 2 の 8 回＋FIFO 0 の 7 回）が取り合っている。取り合うと 1 回の読みが
  0.23µs より遅くなり、固定費が膨らむ。
- 下げる候補: 軽い FIFO 0 の走査を間引く（毎秒 150 本・7 スロットなら数 ms ごとで足りる）、
  無駄なスロットを運ばない分け方、安全と判定した写しの SRAM 読み直し（診断用）をやめる。

**限定受信には一切触れていない。** ECM/EMM は他の PID と同じくパケットを運んだだけで、
中身は見ていない。

## 78. ★★ 取り出しの CPU を下げた＝系全体 40% → 31%、ビット単位の一致は保ったまま（2026-09-27）

§77.6 の続き。まず時間の行き先を測り、見立て（SRAM の口の取り合い）を確かめてから直した。

### 78.1 測り方

- **コールバックの中**: 区間ごとの所要を CP0 Count で積算して `/proc/yb1fifo` に出す
  （`phase` 行）。読み手の `read()` の中も、割り込んできたコールバックの分を差し引いて測る。
- **系全体は `/proc/stat` では測れない。** 2.6.27 は CPU 時間をスケジューラの tick で標本化する
  ので、ハード割り込み文脈の時間が見えず、タイマに起こされる読み手はエイリアスする
  （コールバックだけで 14% あるのに系全体が 0.4% と出た）。
- 代わりに **`yb1-mem 0 1 burn <ms>`**（スタックしか触らない空回しの回数）を横で回し、
  ドレイナー無しの回数との比で「他の仕事に残る CPU」を出す。無しは 20 秒で
  **13,676〜13,685 回/秒**（前後・日を置いても一致）で、物差しとして十分に安定している。
- **ヌル以外の流量は番組で変わる**（557 MHz の多重は常に毎秒 11,496 本だが、ヌルが 4%〜47%。
  §76.1 の 4% は一時点）。コストは流量に比例する部分があるので、比べるのは流量の近い走行同士。

### 78.2 ★ 時間の行き先（§77.4 の版・全 PID・読み手 `cat`）

| 区間 | 1 回 | 読み |
|---|---|---|
| FIFO 0 の走査（**XDMA の転送中**） | **12.3µs**（7 語＝1 語 1.76µs） | 転送中でなければ 2.1µs（1 語 0.3µs） |
| 回収: 作業領域 → リングの写し | **4.7µs／本** | DRAM から DRAM（キャッシュ経由）。行の取り損ね 1 回 ≒ 0.35µs |
| 回収: PID ごとの統計 | 2.0µs／本 | |
| 回収: SRAM の到着時刻の読み直し | 0.7µs／本 | |
| 回収: キャッシュの無効化 | 1.1µs／転送 | |
| 周期の入口（`hrtimer_forward_now` ほか） | 1.3µs／周期 | |

- **SRAM の口の取り合いは実在した。** XDMA が SRAM を読んでいる間、CPU の SRAM 読みは
  1 語 0.3µs → 1.76µs になる。§77.4 の版は転送を起こしてから FIFO 0 を走査していた。
- **FIFO の一巡の最短**（あるパケットから「スロット数 − 1」本後まで＝そのスロットが再び
  書かれうる最短）: FIFO 0（固定 SI・PMT・ECM・EMM）は 6.8〜11.8 ms、FIFO 2（映像ほか）は
  **352µs**（30 秒×数回）。
- **タイマ割り込みそのもの**（コールバックが何もしない `dry=1`）: 100µs で 6.2%、200µs で 3.0%
  ＝割り込み 1 回あたり約 6µs が、コールバックの外（割り込みの出入りと hrtimer の処理）で消える。

### 78.3 変えたこと

1. **CPU で写す FIFO を、XDMA の転送を起こす前に済ませる**（`cpu_first`）。
2. **XDMA にリングへ直接書かせ、到着順は要素番号の待ち行列で読み手に渡す。** 作業領域からの
   写しをやめた。転送はスロットのアドレス順に並ぶので、到着順は待ち行列の側で表す。リングの
   末尾に 16 要素の予備を置き、転送をリングの端で切らない。
3. **リングの行をキャッシュ上で決して dirty にしない。** 220 バイトの要素は 32 バイトの行に
   揃わず隣と行を分け合うので、書き戻しが隣に DMA が置いたばかりの中身を潰しうる。CPU で写す
   FIFO は非キャッシュの別名で書き、DMA の後も CPU の写しの後も、その範囲を無効化してから公開する。
4. 安全と判定した写しでは SRAM を読み直さない（危ないときだけ読む）。安全の判定には、走査から
   転送開始までの遅れを足した。
5. 統計を切れるようにした（`stats=0`）。
6. **読み手が待っていないのに毎周期 `wake_up` していた**のを直した（`waitqueue_active` を見る）。

効かなかったもの（外した）:

- 静かな FIFO 0 を 8 周期に 1 回だけ見る: 系全体で差が出ない（走行間のばらつき以下）。
- 読み手の `read()` で次のパケットの行を先読み（prefetch）: 1 本 3.40／3.41µs → 3.53／3.56µs。

### 78.4 ★ 結果（557 MHz・全 PID・各 20 秒）

| 版 | 読み手 | ヌル以外の流量 | コールバック | **系全体** |
|---|---|---|---|---|
| §77.4 の版 | `cat` | 毎秒約 10,900 本 | 25.6% | **40.4%** |
| 新・`stats=0` | `cat` | 毎秒約 10,900 本 | 13.2% | **30.8%**（2 回とも） |
| 新・`stats=0` | なし | 毎秒約 10,800 本 | 11.2% | 21.5% |
| §77.4 の版 | `cat` | 毎秒約 7,200 本 | 19.1% | 30.5% |
| 新・統計あり | `cat` | 毎秒約 7,400 本 | 12.3% | 27.1% |
| タイマだけ（`dry=1`） | なし | — | 0 | 6.2% |

- どの走行も連番の途切れ 0・破れ 0・溢れ 0。
- 新しい版のコールバックの内訳（`stats=0`・流量約 10,900 本）: 入口 1.1µs と FIFO 2 の走査
  3.3µs（8 語）と FIFO 0 の走査 2.1µs（7 語）が毎周期、回収 1.7µs と転送の起動 2.2µs が転送ごと。
- 読み手（`cat`・4KB 読み）: `read()` の中が 1 本 3.4µs（約 3.5%）、システムコール・切り替え・
  `cat` 自身で約 2 点。47KB 読みの `dd` は 1 本 5.3µs で悪い（読み手側の緩衝がキャッシュに
  収まらない）。行はどれも DMA の後の取り損ねで、先読みは効かなかった。

### 78.5 ★ 参照受信機との照合（新しい版）

§76 と同じ手順（参照側 40 秒・YB1 10 秒・同時）。YB1 は 61,170 本・34 PID
（`0x0023`／`0x0028` はこの 10 秒に来なかった）。位置を決められた 23 PID は**全数ビット単位で
一致**（映像 `0x0100` 44,013 本を含む）、欠け 0・化け 0。ヌル以外の網羅は 1.000
（参照 61,151 本・YB1 61,170 本。差は窓の端）。この窓のヌルは 114,860 本中 53,709 本（47%）。
YB1 上の検査でも同期の乱れ 0・全 PID で連番の途切れ 0。

∴ **待ち行列に変えても、リングへの直接転送と非キャッシュの書き込みでも、TS は 1 ビットも
変わらない。**

### 78.6 残り

- 系全体約 31% の内訳は、コールバック 13%・タイマ割り込みの出入り 6%・読み手 5〜6%・残り数点。
- **割り込みの回数**を減らすには周期を延ばすしかないが、FIFO 2 の一巡の最短 352µs に対し、
  到着から取り出しまでが最大「周期 × 2 ＋ 転送」なので 100µs が上限に近い。最新の 1 本を
  1 周期待つのをやめられれば（到着時刻が十分古ければ書き終わっている、と言えるか。未検証）
  150µs 程度まで延ばせる。
- **走査の読み**（毎周期 15 語）を 1 語にできる「単調に増える到着の計数」が SRAM にあれば、
  何も来ていない周期が安くなる（§73.5 の「計数」の語が候補。未確認）。
- 読み手の 1 本 3.4µs は DMA の後の DRAM の読みで、`read()` の契約を保つ限り残る。

**限定受信には一切触れていない。** ECM/EMM は他の PID と同じく運んだだけで、中身は見ていない。
参照側もデコードせずに取り込み、照合の後に TS を消した。

## 79. 裏 CPU を起こすときに何が起きるか（読むだけの観察・2026-09-28）

ドライバをベンダの `.ko` 無しで成立させるための「1」（csd 抜きの起動）の下調べ。
**我々は読んだだけで、書いたのは csd 自身**（段取りは `openird-a8` と分担・受信スタック仕様の
担当は 54）。

### 79.1 ★ 低位 8KB（物理 `0x2000`）の中身は FW のイメージから来ている

`subcpu-control.md` §5.1 で「csd の load で変わる」とした 8KB を、`yb1-mem 0x2000 0x2000 locate
<ファイル>…`（新規。完全一致で探し、**位置だけ**を出す。バイトは表示しない）で調べた。

- 16 個の 512 バイトの塊のうち 15 個は 0。**0 でない語は 6 語だけ**: `+0x000` `+0x004` `+0x008`
  と `+0x180` `+0x184` `+0x188`。
- 6 語とも、mtd5 の **`emma3sep_driver_std_op0.mem` の「同じ位置 + `0x210`」の語と一致**した
  （ファイルの `0x210`〜`0x21b` と `0x390`〜`0x39b`）。`csd.ko`・ほかの `.ko` 全部・`blur`・
  `dione_transcode` には無い。
- DRAM `0x30000` のイメージは、ファイルの先頭からそのまま一致した（ヘッダを剥がしていない）。
- `+0x000` と `+0x180` は MIPS の例外ベクタの位置である。∴ **FW のイメージの 2 か所の 3 語が、
  ベクタの位置へ写されている。** 中身は裏 CPU のコードなので見ていない。

### 79.2 ★ 起動の順番（`avwatch.ko`・20µs ごと・2 回）

`openird-nocsd-once` で csd 抜きに起動し、`avwatch.ko`（新規。読むだけ。BHIF の 8 語・クロック／
リセットのページの 4 語・低位の 2 語が 0 か否か・共有メモリを見張り、変化を時刻つきで記録）を
入れてから csd を load した。

| 時刻（2 回目・見張りの開始から） | 変化 |
|---|---|
| 2.14 s | `insmod csd` 開始 |
| 2.265〜2.267 s | `SW_INT_EN`（`+0x1a4`）が 1 ビットずつ立つ＝割り込みの登録（GPL の `bhif.c` の unmask） |
| **2.266609** | **`+0x190` ← `80030000`、`+0x180` 3 → 0**（1 回目は 3 → 2 → 0 と 2 段で、間が 21µs） |
| **2.28 s** | **`insmod csd` が返る** |
| **2.347911**（解除から 81 ms） | クロック／リセット `0x101113b0`: `0a220000` → `00020000` |
| 2.347929〜.949 | 低位の `+0x180`・`+0x000` が 0 でなくなる（§79.1 の 6 語） |
| 〃（1 回目の見張り） | 共有メモリ `0x10000`〜`0x1ffff` が**全部 0 になる**（電源投入時はゴミ） |
| 2.350529〜.569 | `+0x1ac` ← `a0010000`、`SUB_INT_EN_0` ← `80`、`+0x1b4` ← `00070000`（1 回目は同じ時に `SW_INT` = 2＝CSD_SPECIAL） |

- `CSD_MODE=FORCE` の load で変わるのは `SW_INT_EN` だけ（共有メモリ・低位の 8KB も不変）。
  `rmmod` で 1 ビットずつ落ちる。
- 起動の 3 秒後、共有メモリは**全部 0** のまま。
- 見張りの最長の間隔は 115〜120µs（20µs より近い書き込みは同じ標本に入り、順番が付かない）。

### 79.3 読み

- **csd がこの時刻にしているのは、`+0x190`（起動ベクタ）と `+0x180`（リセット解除）だけ**に見える。
  81 ms 後の出来事（クロック／リセットの 1 語・ベクタ・共有メモリの 0 埋め）は **`insmod` が
  返ってから約 60 ms 後**に起きていて、**起動した FW が自分でしている**と読むのが自然【推定】。
  csd の遅延処理である可能性は、読むだけでは消せない。
- 最後の 3 語（`+0x1ac` `SUB_INT_EN_0` `+0x1b4`）は、FW が合図（`SW_INT` bit 1）を上げた直後に
  書かれる。csd の割り込み処理か FW かは未判別。
- ∴ **csd 抜きの起動の候補手順**: FW を `0x30000` に置く → `+0x190 = 80030000` → `+0x180` を
  2 → 0 → 合図を待つ → 3 語が書かれていなければ csd と同じ値を書く → §72.5 の手順で
  Initialise（`0x1900`）・OpenDevice。**未検証。**
- **`subcpu-control.md` §5.5 との食い違い**: `0x10111030`/`034`/`058` は、csd の load の 20 秒後も、
  `val_merge` の load の後も**変わらなかった**（`0`/`0`/`3` のまま）。変わるのは `0x101113b0` だけ。
  起動には要らない（FW は動いた）。何が変えるのかは未確認。

**限定受信には一切触れていない。** 低位の 8KB と FW のイメージは、位置と「0 か否か」しか見ていない。

## 80. ★★ csd を「起こすだけ」に使い、外した後は自前の経路だけで裏 CPU と話せた（2026-09-28）

§79 の後、**裏 CPU を自分で起こす方向は打ち切った**（その先の調べが止められたため・ユーザ判断）。
代わりに、起動はベンダの `csd.ko` に任せ、起きた後はベンダの `.ko` を外して §72.5 の手順だけで
話せるかを確かめた。

手順（通常のリグで起動した後）:

```
rmmod val_merge ; rmmod csd                    # 裏 CPU は止まらない（+0x180 = 0 のまま）
entry 1: Initialise (0x1900)  要求 0・応答 0   → 合図 0x100001b0 ← 0x00020000
entry 2: OpenDevice (0x1901)  要求 {1}・応答 4 → 同上
```

| | 結果 |
|---|---|
| `rmmod` の後 | `+0x180 = 0`（走ったまま）。`SW_INT_EN` は `0` に戻る（割り込みの解放）。`+0x1ac = a0010000`・`SUB_INT_EN_0 = 80`・`+0x1b4 = 00070000` は残る |
| **Initialise** | **`word0 = 00040002`・`word5 = 0`（成功）**。ベンダの `.ko` を 1 つも通さない Initialise は初めて |
| **OpenDevice** | **`word5 = 0`・応答 `0x00010000`**＝ベンダの経路と同じハンドル |
| `SW_INT` | 応答の合図 bit 0 が立ったまま残る（受け手がいない）。残っていても次のコマンドは完了した＝消さなくてよい |

∴ **ドライバに残るベンダ依存は「起動時に所有者の機器上の `csd.ko` で裏 CPU を起こす」1 点に
できる見込み。** 次は、段階 C の定型列（デマルチプレクサの設定・種別 7 のフィルタ）を
コマンドで積む。ioctl の番号からコマンド ID は一定の差では決まらない（`nr=0x01 → 0x1900`、
`nr=0x26 → 0x1929`・§66.1）ので、使う ioctl ごとのコマンド ID・要求長・応答長を、ベンダの経路で
積まれたエントリから控えてから、同じものを自前で積む。

**限定受信には一切触れていない。**

## 81. ★★★ ベンダの AV 側 `.ko` 無しで、デマルチプレクサを組み、FIFO に TS を入れた（2026-09-28）

§80 の続き。**起動だけ csd に任せ、`val_merge` と csd を外した後、段階 C の全 PID の設定
（§77）を自前のコマンドだけで積んだ。**

### 81.1 やり方＝ベンダの経路を 1 度記録し、同じコマンドを積む

- `yb1-ioctl` に `trace` を足した: ioctl のたびに、それが生んだコマンドスタックのエントリ
  （エントリ番号・コマンド ID・通し番号・状態・要求と応答の語）を出す。読むだけ。
- 全 PID の列（§77 の ioctl 約 290 本）をベンダの経路で流して記録した。**コマンドは 260 本**。
  ioctl のうち 5 本はコマンドを生まない（ドライバだけで答える）。
- ioctl の番号 → コマンド ID（一定の差ではない）:

| ioctl | コマンド | 要求／応答（バイト） | 読み |
|---|---|---|---|
| `0x01` | `0x1900` | 0／0 | Initialise |
| `0x02` | `0x1901` | 4／4 | OpenDevice（最初の 1 本で 2 つ開く: `0x00010000` と `0x01010001`） |
| `0x05` | `0x1904` | 8／0 | 入力源 |
| `0x65` / `0x64` | `0x1919` / `0x1913` | 12／0 | 2 語目は `val_merge` のカーネル空間の番地（裏 CPU には不透明な値） |
| `0x48` | `0x194f` | 8／0 | |
| **`0x0b`** | **`0x190a`**（＋`0x191a`） | 16／4（＋4／4） | **フィルタの確保**。要求 2 語目は**自分の 4 語目を指す番地**（要求 ＋ 12）、4 語目は params。`val_merge` は確保のたびに `0x191a`（添字の取得）を続けて積む |
| **`0x0c`** | **`0x190b`** | 8／0 | **PID の設定** |
| **`0x0d`** | **`0x190c`** | 8／0 | **有効化** |
| `0x32` | `0x194c` | 8／0 | |
| `0x30` / `0x33` / `0x34` | `0x194a` / `0x194d` / `0x194e` | 16／4・4／0・4／0 | |
| `0x19` `0x1a` `0x14` `0x15` `0x16` `0x26` `0x29` `0x2a` `0x2b` `0x2e` `0x2f` | `0x191a` `0x191b` `0x1914` `0x1915` `0x1916` `0x1929` `0x192c` `0x192d` `0x192e` `0x1931` `0x191b`＋`0x1932` | | ペイロード側（§43） |
| `0x0a` / `0x47` / `0x67` | `0x1909` / `0x1928` / `0x1927` | | 状態 3（既知の失敗・§34.3） |

- **エントリ 0 は環ではない。** ベンダは状態の問い合わせ（`0x191b`）をエントリ 0 に積む
  ことがあり、環（1〜31）はそれを飛ばして続く。§72.5 の合図ではエントリ 0 は処理されない
  （積むと待ったままになった）。別の出し方があるとみられる（未確認）。問い合わせなので省いた。
- **順番は通し番号の順**（下位 8 ビットが数え、回る）。環の端をまたぐ呼び出し（エントリ 31 →
  1）を表の順に積むと、裏 CPU は次に来るはずのエントリで待ったままになる（一度踏んだ）。

### 81.2 ★ 結果（電源投入 → csd で起動 → `rmmod val_merge csd` → 自前で 257 本）

| | 結果 |
|---|---|
| コマンド | **257 本すべて、状態と応答がベンダの経路と一致**（違い 0・待ったまま 0） |
| PID 表 | 機器 1・機器 2 とも**ベンダの経路で組んだときと 1 語も違わない** |
| FIFO | FIFO 0（7 スロット）・FIFO 2（8 スロット）に入る |
| ドレイナー → `/dev/yb1fifo` の 10 秒 | 109,343 本・同期の乱れ 0・全 PID で連番の途切れ 0・破れ 0 |

∴ **受信の経路（選局 → デマルチプレクサ → FIFO → ドレイナー → `/dev`）から、ベンダの
`val_merge.ko` が外れた。** 残るベンダ依存は、起動時に裏 CPU を起こす `csd.ko` だけ。
参照受信機との照合は、参照側のチューナが空いたときに行う（§81.3）。

道具: `tools/yb1-cmd`（コマンドを積み、状態と応答を期待値と突き合わせる）、
`tools/host/trace2cmd.py`（`trace` の記録から `yb1-cmd` の列を作る。通し番号順に並べ直し、
エントリ 0 は注記だけにする）。

**限定受信には一切触れていない。** ECM/EMM の PID も他と同じくフィルタに載せて運ぶだけで、
デスクランブラの関数・`val_m2dsc`／`val_dscram` には触れていない。

## 82. ★★ 受信の経路に要るベンダの `.ko` は csd だけ（フロントエンド側のモジュールも要らない）（2026-09-28）

§81 は、フックがベンダアプリの代わりに load するモジュール一式（`ei2c_ofdm_drv`・
`io_controller_drv`・`fe_common_drv`・`fe_main_drv`・`mem2`）と、`setup.sh` の残り（`h57fmb.ko`・
`dione_transcode -w`・`8192du.ko`・`cmd_executor_server`）が入った状態だった。これらが load 時に
何か（GPIO・ピン・クロック）を整えていて、それに頼っている可能性を消すため、**入れない起動**を試した。

- リグのフックに `openird-min-once` を足した（1 回きり。`tools/vendor-rootfs/openird-pre-stagec.sh`）:
  ベンダのモジュールは **csd と GPL の `i2c-emma`／`i2c-algo-emma` だけ**を load し、`setup.sh` は
  何もしない版に差し替える（Dione も Wi-Fi も起こさない）。

| 手順 | 結果 |
|---|---|
| 起動直後の `lsmod` | `i2c_algo_emma`・`i2c_emma`・`csd` の 3 つだけ。ベンダのプロセスも無い |
| `yb1-mxl`（MBIN の転送・選局 557 MHz） | `0xa8cc = 0xabcd`、`0x112d = 1018`（ロック） |
| `rmmod csd` → `yb1-cmd`（§81 の 257 本） | **257 本すべて一致** |
| ドレイナー → `/dev/yb1fifo` の 10 秒 | **109,586 本・同期の乱れ 0**。映像 PID で 1 本の欠け（ドレイナーの「破れ」1 回。起動直後でカーネルログの吸い出しが割り込みを止めていた時間帯。ドレイナーの頑健性の課題で、この起動の形とは別） |

∴ **受信の経路（選局 → デマルチプレクサ → FIFO → `/dev`）に要るのは、起動時の `csd.ko` だけ**。
フロントエンドの復調器は、ベンダのフロントエンドモジュール無しで（GPL の I2C だけで）立ち上がる。
TS は Dione（MB86H58）を起こさなくても SoC に届く＝Dione は TS の経路に挟まっていない（少なくとも
電源投入時の状態では素通し）。所有者の機器から実行時に使うものは、裏 CPU の FW（mtd5）・MXL683 の
MBIN（`blur` の中）・`csd.ko` の 3 つ。

**限定受信には一切触れていない。**

## 83. ★★ フィルタの付け替え＝FIFO に入るかは PID の値だけで決まる。ヌルも入れられる（2026-09-28）

chardev の選局（別の多重への切り替え）に要る「フィルタの PID を差し替える手順」を確かめた。

### 83.1 無効化はコマンド `0x190d`

ベンダの経路（`nr=0x0e`）を `trace` で記録した: **`0x190d`・要求はハンドルだけ（4 バイト）・応答なし**。
§81.1 の表に足す。

### 83.2 ★ 1 本のフィルタの状態と FIFO（csd・`val_merge` を外した後、`yb1-cmd` で）

| 手順 | PID 表 | FIFO 0 に入るもの |
|---|---|---|
| 有効のまま PID を変える（`0x190b`） | 変わらない | **状態 8 で拒まれる** |
| PID `0x1fff`・有効 | `9fff` | **ヌル（毎秒約 6,000 本）** |
| 無効化だけ（`0x190d`） | `1fff` | **ヌルが入り続ける** |
| 無効のまま PID `0x0900` | `0900` | **`0x0900` が入る（有効化しなくても）** |
| 有効化（`0x190c`） | `8900` | `0x0900` |
| 無効化 → PID `0x1fff`（無効のまま） | `1fff` | ヌル |
| PID `0x0012` → 有効化 | `8012` | `0x0012` |

- **確保済みのフィルタは、有効・無効にかかわらず、PID 表の値の PID を FIFO に入れる。**
  有効化のビット（bit15）は FIFO の門ではない（§73.9 と合う）。確保していない項（`1fff`）は
  何も入れない。有効化は、裏 CPU の側の処理（セクション・PES）のためのものとみられる【推定】。
- **PID を変えられるのは無効のときだけ**（有効なら状態 8）。
- ∴ **選局の切り替え＝フィルタごとに「無効化 → PID の差し替え」**。FIFO のためには有効化も
  要らない。使わないフィルタは、多重に現れない PID に置いておく（確保したままでは止められない。
  解放の呼び出しは未確認）。同じ PID を 2 本に載せない約束（§5.2 of ts-fifo）は変わらない。
- **ヌル（`0x1fff`）をフィルタに載せれば FIFO に入る**（§73.6/§77.5 の「ヌルを入れる手段は
  見つかっていない」を訂正）。∴ **多重の全体（ヌル込み）を運べる。** ヌルは多重の 4〜47% を占める
  ので、載せるなら XDMA で吸う側（FIFO 2）に置く。

### 83.3 選局し直し（フィルタはそのまま）

557 MHz の全 PID の設定のまま 521 MHz に選局し直すと、FIFO には新しい多重のうち**既存の
フィルタの PID と一致するものだけ**が入った（例: `0x0111` が毎秒約 5,500 本・スクランブルあり）。
557 MHz に戻すと元どおり。∴ ドライバは、選局のあと新しい多重の PAT/PMT/CAT（固定 PID の
フィルタで入ってくる）から PID を列挙し直し、§83.2 の手順で差し替えればよい。

**限定受信には一切触れていない。**

## 84. 選局の値（周波数の書き方・ロックまでの時間・C/N）（2026-09-28）

chardev の `SET_CHANNEL`／`GET_CNR`（px4_drv 互換）が返すべきものの下調べ。フロントエンドは
§82 の最小構成（ベンダのフロントエンドモジュール無し・`yb1-mxl`）で測った。

- **周波数の書き方**: §29.4 の選局語 `(448·X + 64) / 7`（X は物理チャンネルの呼び MHz）は
  **`64 × (X + 1/7)`＝中心周波数を 1/64 MHz 単位で表したもの**で、地上デジタルの 1/7 MHz の
  ずれを含んでいる。UHF の物理チャンネル ch（13〜62）なら `X = 473 + 6·(ch − 13)`。
- **ロックまで 0.5〜0.6 秒**（選局語を書いてから `0x112d ≠ 0` まで。3 局に 6 回・読み 1 回約 45 ms
  の刻み）。信号の無い 479 MHz は 2.7 秒たっても 0 のまま。∴ `SET_CHANNEL` は 1 秒ほど待って
  0 のままなら「ロックしない」と返せばよい。
- **C/N の値**: ロックの直後の 1 回目だけ定常値の数倍（`0x1232`〜`0x1c98`）が出ることがある。
  0.8 秒後にはもう定常の範囲に入り、±15% ほど揺れる（521 MHz で 635〜902・557 MHz で 765〜1107）。
  ∴ ロック直後の値は捨て、数回の平均を使う。
- **dB への換算**: §4.3 の 10 組（ベンダの `Cn` と `0x112d`）は `Cn ≈ K − 10·log10(raw)`・
  K = 58.1〜60.4 に乗る。ばらつきは瞬時値と平滑値を比べたためなので、平均した raw を使えば
  **`Cn ≈ 59.3 − 10·log10(raw)`（±1 dB 程度）**が当面の式。広い範囲での較正は未了（§4.3）。

**限定受信には一切触れていない。**

## 85. ★★ 生 TS に要るコマンドは 6 種だけ（2026-09-28）

§81 はベンダの列 257 本をそのまま積んだ。ドライバに要る最小の組を確かめるため、
**思いつく限り削った組**を、電源投入 → csd で起動 → `rmmod val_merge csd` の後に積んだ。

```
0x1900  Initialise
0x1901  {1}  ×3          → 0x00010000 / 0x01010001 / 0x02010002（機器 0・1・2）
0x1904  {機器 0, 3}  と  {機器 2, 3}        入力源
0x190a  {7, 自分 + 12, 機器, 0xffffff00}   → フィルタ     ┐ PID ごとに
0x190b  {フィルタ, PID}                                   ┘（有効化はしない）
```

`0x1919`/`0x1913`（ctx つきの機器の設定）・`0x194f`・ペイロード側・`0x191a`・`0x194c`・
有効化（`0x190c`）は**積んでいない**。16 本すべて状態 0 で、FIFO には載せた 5 PID がすべて入った:

| FIFO | PID | 毎秒 |
|---|---|---|
| 2 | `0x0100`（映像・スクランブルあり） | 6,263 |
| 2 | `0x0110`（音声） | 129 |
| 2 | `0x0000`（PAT） | 10 |
| 0 | `0x0012`（EIT） | 41 |
| 0 | `0x0900`（EMM） | 27 |

∴ **ドライバが積むコマンドは `0x1900`・`0x1901`・`0x1904`・`0x190a`・`0x190b`（と選局し直しの
`0x190d`）の 6 種だけ**。ベンダの列の不透明な値（ctx）は要らない。機器 2（FIFO 2）を使うには
OpenDevice を 3 回呼ぶ（機器の番号は開いた順）。

**限定受信には一切触れていない。**

## 86. ★ ロックの判定は `0x1129 == 0x1f` かつ `0x112e == 1`（`0x112d ≠ 0` ではない）（2026-09-28）

§84 のロックの時間を使って UHF の全チャンネルを走査したら、`0x112d ≠ 0` の判定では
ロックしていないチャンネルでも一瞬 0 でない値（`0xa140` など）が出た。§4.3 でベンダの状態読みが
触っていた「全チャンネルで値の動かない」レジスタのうち、**信号の有無で変わるもの**を探した。

| レジスタ | 557 MHz（ロック） | 479 MHz（信号なし） |
|---|---|---|
| **`0x1129`** | **`0x1f`** | `0` |
| **`0x112e`** | **`1`** | `0` |
| `0x1147` / `0x1148` | `0xdf` / `0x0e` | `0x03` / `0` |
| `0x112d` | C/N の値 | `0` |
| `0x002b` / `0x002c` / `0x101c` / `0x0034` | 同じ（7 / 8 / 4 / 3） | 同じ |

選局の直後の動き（約 0.17 秒刻み）:

```
          0x1129   0x112e   0x112d
0.34 s    0x1c     0        0x2189   ← 一時的な値
0.84 s    0x1f     0        0x0354   ← ここから C/N は定常の範囲
1.67 s    0x1f     1        0x02ce   ← 最終段
```

- **`0x1129` は段ごとのロックのビット**（0 → `0x1c` → `0x1f`）とみられ、`0x112e` はその後の
  最終段【推定】。**`0x1129 == 0x1f` かつ `0x112e == 1` を「ロック」とし、1.4〜1.7 秒かかる。**
  `0x112d` は `0x1129 == 0x1f` になった後の値だけを C/N に使う（§84 の「1 回目だけ大きい」は
  この一時的な値だった）。
- ∴ `SET_CHANNEL` は 2〜3 秒まで待ってロックの成否を返す。

**この場所で受かるチャンネル**（上の判定・3 秒まで）: ch 16・17・18・19・21・22・23・24・26・27
（10 局）。C/N は約 29.6〜32.9 dB（§84 の式）。ch 25（545 MHz）は `0x1129 = 0x1f` のまま 3 秒で
`0x112e` が立たなかった（§30 では受かっている＝判定の時間切れの見込み）。ch 13・60 は
`0x1129 = 0x0f`・`0x112e = 1` という中途の状態（弱い信号とみられる）。

**限定受信には一切触れていない。**

## 87. ★★★ ドライバと同じ構成（csd は起動だけ・コマンド 6 種）で、参照受信機とビット単位で一致（2026-09-28）

§85 の最小の組で 557 MHz の全 PID（機器 2 に 32 本・機器 0 に 18 本）を載せ、csd と `val_merge` を
外した状態でドレイナーから取り出して、参照受信機（PX-M1UR・`--no-decode`）と同時に取り込んだ。

| | 参照受信機 | YB1 |
|---|---|---|
| ヌル以外（同じ時間窓） | 109,559 | **109,559（1.000）** |
| PID | 35＋ヌル | **35 すべて本数比 1.000** |
| ビット単位 | — | 位置を決められた 23 PID は全数一致（映像 92,363 本を含む）・欠け 0・化け 0 |
| ドレイナー | — | 破れ 0・同期の乱れ 0 |

∴ **ベンダの AV 側の `.ko` を起動時の csd だけにした経路で、放送の TS をビット単位で取り出せる。**

### 87.1 ★ ヌルは FIFO には入るが、ドレイナー（100µs）が追いつかない

同じ構成に PID `0x1fff` のフィルタ（§83）を足すと、ドレイナーが大きく取りこぼした（10 秒で映像に
1,104 か所の途切れ・破れ 134 万回）。ヌルのフィルタだけを外すと 0 に戻った。

- FIFO を見張ると、**ヌルは最小間隔 32µs で長く続けて来る**（§75.3 の「32µs 間隔の連続は最長
  2 回」はヌル抜きの話だった）。7 スロットの FIFO は約 190µs で一巡し、周期 100µs・最新の 1 本を
  1 周期待つドレイナーの余裕（§74/§77）を超える。
- ∴ **いまのドレイナーではヌルは運ばない**。運ぶなら周期を詰めるか、ヌルだけ別の扱い（数えて
  作り直す等）が要る。

### 87.2 FIFO 2 のスロット数は設定で変わる

最小の組では **FIFO 2 のスロットは 7 個**（`0x9134`〜`0x965c`。待ち行列にも 7 つしか出ない）。
ベンダの列（§77.3）では 8 個（`0x9738` まで）だった。省いたコマンドのどれか（`0x194a` の確保など）
が 1 つ足しているとみられる【推定】。ドレイナーはロード時に数えるので、どちらでも動く。

道具と手順の補足: 参照側が空いているかは `pgrep -a recisdb | grep "device /dev/pxm1urvideo1"` で
確かめる（`pgrep -f` だと確かめている ssh のシェル自身に当たって、いつも「使用中」になる）。

**限定受信には一切触れていない。** 参照側もデコードせずに取り込み、照合の後に TS を消した。
