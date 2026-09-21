# POE-BR100-YB1 受信スタックの構成と境界（段階 B の観察）

<!-- openird-spec
Status: observing
As-of:  実機 POE-BR100-YB1 / ベンダ FW 2.1.17 r3 (2026-09-22)
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

節名から数えたブロックと関数本数:

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
足した像を焼き、そのフックで**裏 CPU ファームの転送と `csd` の load を自分でやる**。
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
2. **バッファの数。** ベンダは **28 個**作ってから `nr=0x0a` を呼ぶ。
   我々は 1〜2 個。数に意味がある可能性は低いが、確かめるのは簡単
   （ループで 28 個作ってから叩く）。
3. **そもそも `nr=0x0a`/`0x47`/`0x67` を飛ばす。** この 3 本は TS 入力ポートの
   設定に見えるが、**推測である**。§33.1 のとおりデータが来るのはデバイス 1 の
   PID フィルタなので、入力が通らない理由が別（復調器の TS 出力が無効、
   SoC 側の TS 受け口が未初期化）である可能性は残っている。
   **復調器側で TS 出力に関わるレジスタ（§9.1 の A3 に残る 19 本）を洗う**のは、
   デマルチプレクサ側とは独立に進められる。

**次は 1 と 3 を並行させるのが良い。** 3 は `tools/yb1-mxl` で完結し、
実機のリグも既にある。

---

**運用の手順は [../poe-br100-yb1-rig.md](../poe-br100-yb1-rig.md) に分けた**
（リグの切り替え・電源の入れ直し・ファイルの授受・トレースの採り方・実機に置いて
ある道具）。段階 C の定型列は
[tools/yb1-ioctl/stage-c-demux.script](../../tools/yb1-ioctl/stage-c-demux.script)。
