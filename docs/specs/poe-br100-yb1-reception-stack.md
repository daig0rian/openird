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

**チューナは復調器の I2C ゲートの向こう**にある。根拠となるシンボル:

| モジュール | シンボル |
|---|---|
| `ei2c_ofdm_drv` | `ei2c_get_through_demod_tc90512` / `ei2c_set_demod_tc90512` |
| `fe_common_drv` | `TunerRead_viaDemod` / `TunerWrite_viaDemod` / `Tuner_set_slave_addr` / `getSlvAdr_Demod1` / `getSlvAdr_Demod2` |

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

### 4.2 検討して否定した筋（記録）

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

### 5.4 この個体はチャンネルスキャン未実施

`/app/setting/DTVS_ChannelList.data` / `DTVS_ServiceList.data` はいずれも 12 バイトの
全ゼロ。BS/CS 側も 16 バイトの空。∴ 番組表・サービス ID 経由の経路は使えず、
**周波数直指定（§5.1）が観察の唯一の入口**。

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

1. **`0x60` の品種とアクセス手順**。バス 1 で応答する唯一の素子で、選局の
   トラフィックはここ宛て。§2.1 の構造から復調器と読めるが、素直なレジスタ
   読みでは中身が出ない（§4.1）。**最優先**。
   - **有望な筋**: `fe_main_drv` が `FE_debug_show_demod_reg_all`（復調器レジスタの
     全ダンプ）と `FE_set_debuglevel` を持ち、`fe_common_drv` には
     `demod_saddr=… tuner_slv=…` を出す printk がある。これらは `/dev/fe_main` の
     ioctl から届くはずで、**blur はこの chardev を使っていない**（§3.1）＝
     試しても受信を乱しにくい。ioctl 番号が分からないので、そこをどう割るかが課題。
   - UPnP 経由でログレベルを上げる筋は否定済み（§4.2）。
2. **チューナの品種**。復調器のゲート越しなのでバス走査では見えない（§2.1）。
   `0x60` を開ける手順が分かれば、その先に出る。
3. ~~`/dev/fe_main` の ioctl 契約~~ — **解決（否定的に）**。`fe_main` は使われない（§3.1）。
   周波数は syscall 境界を渡らない。段階 C はこの経路を再現しない（§3.2）。
4. **生 TS の取り出し口**。`/dev/val_demux` + `/dev/mem2` の mmap が本命だが、
   `val_merge` は Proprietary＝観察でしか辿れない。スクランブル解除より**上流**で
   タップできることを確認する必要がある（§0）。
5. ~~`i2c_emma` の GPL ソース未同梱~~ — **誤りだった**。GPL tarball は完全なツリーで、
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
