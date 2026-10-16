# SB-TV06-WRIP (POE-BR310L-SB1) bring-up と factory 経路

<!-- openird-spec
Status: verified
As-of:  実機 SB-TV06-WRIP (2026-09-26・段階 C の見通しは 2026-10-12)
Impl:   host/openird-mkimage/boards/sb-tv06-wrip.json, host/openird-mkimage/openird_mkimage.py, tools/vendor-rootfs/mkshell.sh
Tests:  tests/mkimage/
Note:   Status は factory 経路（§2〜§4）のもの。§5〜§9 は 2026-09-26 の観察記録（§5.3・§6・§7.2 は 2026-10-12 に訂正）。
        §10 が段階 C の見通し、§11 がリグの現状、§12 が GPL のカーネルの起動確認（1 回だけの RAM 起動・2026-10-16）
-->

<!-- Open 層: 観察の出自は (a) 基板写真の型番シルク (b) 実機 U-Boot ブートログ
     (c) 純正コンテナ POE-BR310L-SB1_v1.0.2 の平文シェルスクリプト（write1Gb.sh /
     setup.sh）の読解 (d) 実機 telnet root での read-only 観察（/proc/mtd・lsmod・
     /proc/cmdline・/dev・/proc/kallsyms のシンボル名）(e) 公開 GPL ソース(Pixela)の静的読み。
     逆アセ由来の内部ロジック・具体アドレス・コード由来の数式は置かない。鍵・パスワード、
     その在処も書かない（CONTRIBUTING.md §0）。 -->

## 0. 一行と現在地

SB-TV06-WRIP は BMMPX1301(YB1) と同じ Renesas EMMA3SEP・同じ NAND・同じ AV スタック
（`val_merge`/`csd`/`mem2`/`h57fmb`）を持つ 3 波録画機。**復調器が MaxLinear MXL683 では
なく Toshiba TC90532（mainline `tc90522.c` に参照あり）で、MBIN ファーム転送が無い**＝
YB1 で最大の関門だった箇所が丸ごと無い。

- **Milestone 0 達成（2026-09-26）**: 純正 factory 経路（`fwup` の UPnP 投入）で telnet-root
  入りの LINDBERG コンテナを実機に流し、**再起動後に root telnet を実機確認**した。
  ベンダアプリ `lindberg` は温存され通常どおり動く（UPnP も上がる）。
- 使った経路は YB1（`docs/specs/poe-br100-yb1-nand-update.md`）と同一。区画表・鍵・
  書き込み機構が同じで、**族タグだけ `LINDBERG`** に替えれば通る。実証済み。

## 1. ハードウェア（基板 TVB3000 REVB）

| 役割 | 部品 | 根拠 |
|---|---|---|
| SoC | Renesas EMMA3SEP `D61425F1`（MIPS32r2・**BE**） | 基板シルク・`BHIF version 0x0000f011 (EMMA3SE/P 1.1)` |
| DRAM | Samsung `K4B2G1646Q`（DDR3・256MB） | シルク・`DRAM Size 256 Mbyte` |
| NAND | Toshiba `TC58DVG02D5TA00`（1Gb SLC・128MB） | 裏面シルク・`NAND: 128 MiB`。YB1 と同一部品 |
| 復調器 | **Toshiba TC90532XBG**（TC90522/90532 系） | シルク・`demod_tc90522_90532_drv.ko` |
| チューナ | MaxLinear **MxL601RF**（地上）＋ **MxL135RF**、Philips **TDA8263HN**（衛星） | `.ko` 名・lsmod |
| トランスコーダ | 富士通 **MB86H58A**（"Dione"） | シルク・`h57fmb.ko` |
| Ether PHY | SoC 内蔵 NEC-Candy ＋ 外付（Realtek A8263 近傍） | `Net: NEC-Candy` |
| Wi‑Fi | Realtek RTL8192DU（`8192du.ko`） | setup.sh の `insmod 8192du.ko` |
| B-CAS | フルサイズ カードスロット実在 | 基板・`/dev/val_scard`・`B-CAS` チャンネルデータ |
| USB | 背面に USB-A ホスト 1 口（RJ45 の下に段積み） | 側面写真。R グレード＝録画用 |
| ラベル | 品番 `SB-TV06-WRIP` / `POE-BR310L-SB1`、DC12V 0.8A、MAC `00:E0:C3:xx:xx:xx` | 底面ラベル・`ifconfig eth0` |

- **U-Boot**: `2014.2.25.00-emma3sep-01-svnRev.16 (Nov 13 2014)`。`PRODUCT_FWIDS: PIX_AIRTUNER_BR310L`。
  YB1 と同じく**インタラクティブ入力は実行ロック**（コマンドはエコーされるが実行されない）。
  ただし factory 経路は `bootcmd` の自動実行なのでロックに触れない（YB1 と同じ立て付け）。
- **カーネル**: `Linux 2.6.27.39 #16 PREEMPT (gcc 4.3.5 Buildroot 2010.11)`。uImage 契約は
  load `0x82280000` / entry `0x822841b0`（実機ログ実測・YB1 と同値）。

## 2. NAND と区画（1Gb / write1Gb.sh 分岐）

実機 `/proc/mtd` と純正 `write1Gb.sh` の nand 引数が一致。**YB1 と完全に同一の offset 体系**。

| mtd | 名前 | offset | size | 用途 / policy |
|---|---|---|---|---|
| 0 | Boot loader(u-boot) | 0x0000000 | 0x300000 | preserve（復旧の安全網・絶対に書かない） |
| 1 | u-boot env + MAC | 0x0300000 | 0x300000 | preserve（更新機構が先頭 0x800 に FWUP マーカーを書く。MAC は最終ブロック） |
| 2 | Kernel(vmlinux) | 0x0600000 | 0x480000 | **writable**（uImage・write_len 0x300000） |
| 3 | FileSystem(SQUASHFS) | 0x0A80000 | 0x900000 | **writable**（`/`・write_len 0x680000） |
| 4 | App_binary(SQUASHFS) | 0x1380000 | 0x1B80000 | preserve（`/app/bin`・ベンダ受信部品・write_len 0x1900000） |
| 5 | EMMA_FW(SQUASHFS) | 0x2F00000 | 0x400000 | preserve（裏 CPU FW・write_len 0x180000） |
| 6 | Initial setting | 0x3300000 | 0x300000 | preserve（v1.0.2 コンテナには同梱されず不発） |
| 7 | Flight Record Log(YAFFS2) | 0x3600000 | 0x300000 | preserve |
| 8 | Application Setting data(YAFFS2) | 0x3900000 | 0x400000 | preserve（`/app/setting`＝`/dev/mtdblock8`。**devel_mode.txt の置き場**） |
| 9 | MassStorage(YAFFS2) | 0x3D00000 | 0x4000000 | preserve（`/app/flashDev`。更新ファイルの一時展開先 `/flash/tmp_update`） |
| 10 | Bad block table | 0x7D00000 | 0x300000 | preserve |

- **fstab**: `mtdblock3→/`（squashfs ro）, `mtdblock4→/app/bin`（squashfs ro）,
  `mtdblock8→/app/setting`（yaffs2 rw）, `mtdblock9→/app/flashDev`（yaffs2 rw noauto）。
- **cmdline**（実機）: `rootfstype=squashfs root=/dev/mtdblock3 ro ip=none PRODUCT_FWIDS=PIX_AIRTUNER_BR310L U_BOOT_REV=16`。
  **`console=` が無い**＝カーネルの起動/panic メッセージは製品 bootargs で捨てられている
  （U-Boot はシリアルに出るがカーネルの printk は出ない・配線の問題ではない）。**ただし
  getty はカーネルコンソールとは無関係にポートを開くので、リグの getty で ttyS1 に root
  シェルは取れる（§7）。** カーネルログは shell の `dmesg` で読む。**`ip=none`**＝eth0 は
  カーネルでは設定されず、`lindberg`（udhcpc）が上げる。

ボード定義は [`host/openird-mkimage/boards/sb-tv06-wrip.json`](../../host/openird-mkimage/boards/sb-tv06-wrip.json)。

## 3. factory 経路（UPnP 投入・実証済み）

形式は `LINDBERG_UPDATE_v1b`（[update-v1b-container.md](update-v1b-container.md) §2.1）。
機器側の書き込み機構は YB1 と同一（[poe-br100-yb1-nand-update.md](poe-br100-yb1-nand-update.md)）。

- **鍵は族横断で単一**＝回収済みの `BLUR` 用鍵・ZIP パスワードでそのまま復号・再暗号化できる。
  `probe-factory` で `LINDBERG_UPDATE_v1b v1.0.2`・`h38 = SHA-1(body)` を確認。
- **投入クライアント = `fwup`**（[`host/openird-fwup`](../../host/openird-fwup/)。ベンダサーバに触れない。
  手順の仕様は [emma3sep-upnp-update.md](emma3sep-upnp-update.md)）。
  UPnP で機器を発見 → ローカル HTTP でコンテナを配って `X_UpdateFirmware` を送る →
  機器が Wget で pull → 機器内で `update.sh`→`write1Gb.sh` が u-boot 更新 env を mtd1 に
  書いて reset → U-Boot が `bootcmd=run update` で各区画を焼く。
  - 機器の UPnP: `friendlyName=PIXELA Digital TV Tuner` / `model=POE-BR310L-SB1` /
    `X_AirTunerControl [control]`。ポート 49152。
  - **版数はゲートではない**: 実機 fw と同じ v1.0.2 の自作コンテナが受理された（YB1 と同じ挙動）。
  - クロスフラッシュ拒否は多段（族タグ→モデル名→鍵/SHA-1）。自作コンテナは `LINDBERG` 族・
    正しいモデル名・回収済み鍵・正しい SHA-1 を満たすので通る。

**実機で観測した書き込みシーケンス**（シリアル）: kernel 3,145,728 / rootfs 6,815,744 /
app 26,214,400 / EMMA FW 1,572,864 を各 erase→write、`Erase update environment`→
`Put F/W Update Marker`（`46 57 55 50 00`="FWUP\0" を mtd1 先頭 0x800）→ reset。
**mtd0(u-boot) は書かない**＝ブリックしても U-Boot は健在＝復旧可能。

## 4. 初 root の作り方（再現手順・実証済み）

段階 B の観察環境を、純正との差を最小にして用意する（差が大きいほどベンダアプリが落ちる）。

```
# 1. 純正コンテナを開封（PowerShell/uv 側。WSL には uv/pycryptodome が無い）
cd host/openird-mkimage
uv run openird_mkimage.py unpack-factory \
  --key-file <鍵> --zip-pw-file <PW> \
  ../../temp/fw/POE-BR310L-SB1_v1.0.2_data.bin ../../temp/br310l

# 2. rootfs に telnet-root の入口を足す（WSL・fakeroot・3.1BE で梱包し直し）
tools/vendor-rootfs/mkshell.sh temp/br310l/payload/rootfs.img <out-rootfs.img> \
  --observe <観察ツール dir> --tag WRIP1
#   - rcS の `setsid cttyhack /app/bin/setup.sh` の直前に telnetd + getty を挿す
#     （lindberg は戻らないので rcS 末尾の getty は死コード＝ここが唯一の介入点）
#   - --observe で /etc/securetty に pts/0..9 を足す（telnet の root ログインが通る）
#     ＋ /opt/openird に静的 ELF ツールを置く ＋ /app/setting のフラグで効く gate

# 3. LINDBERG コンテナに組み直し（rootfs だけ差し替え。他 7 点はバイト一致）
uv run openird_mkimage.py repack-factory --rebuild all \
  --replace rootfs.img=<out-rootfs.img> \
  ../../temp/fw/POE-BR310L-SB1_v1.0.2_data.bin ../../temp/wrip-factory-telnet.bin

# 4. 投入（destructive・毎回ユーザ確認）
uv run host/openird-fwup/openird_fwup.py flash <device-ip> temp/wrip-factory-telnet.bin \
  --serve-ip <PC IP> --confirm-flash

# 5. 再起動後: telnet <device-ip>（login: root / パスワード空）
```

- **fakeroot 必須**（デバイスノード 223 個を落とすと `/dev/console` 不在で無音死）。
- rootfs は **squashfs 3.1BE**（`output/host/squashfs3/mksquashfs -be`）。生成物は 5.04MB＝
  mtd3 write_len 0x680000(6.82MB) の 77%＝切り詰めなし。
- **devel_mode.txt を mtd8 に置けば以後は焼き直し不要**: `touch /app/setting/devel_mode.txt`。
  mtd8 は factory が書かない＝純正 rootfs に戻しても telnet が上がる。実機で作成済み。
- ベンダ `setup.sh` の `if [ -e /app/setting/devel_mode.txt ]; then telnetd -l sh; fi` も
  同じ効果だが、そちらは `-l sh`（ログイン無しの直シェル）。mkshell の telnetd は通常 login。

## 5. 受信スタックの観察（段階 B・read-only）

実機 `lsmod` / `/dev` / `/proc/kallsyms`。**AV スタックは YB1 と同じ族**＝
`docs/specs/poe-br100-yb1-reception-stack.md` §49–§72 の裏取り対象。

- **ロードされているモジュール**: `val_merge`(412992, used by 21)・`csd`・`mem2`[permanent]・
  `h57fmb`(Dione)・`io_controller_drv`・`fe_main_drv`・`fe_common_drv`・`ei2c_ofdm_drv`・
  **`demod_tc90522_90532_drv`**・`tuner_MxLinear_MxL601RF_drv`・`tuner_PHILIPS_TDA8263HN_drv`・
  `i2c_algo_emma`/`i2c_emma`。**MBIN/mxl683 は無い。**
- **`/proc/kallsyms` の MMAC_* シンボルは 722 本**。ブロック族は YB1 と同じ：`AUDIO / BITBLT /
  CAPTURE / CCAPTION / COPYPROTECT / DCNV / DEEPC / DEMOD / DEMUX / DISP / DMA / DSCRAM /
  ELU / JPEG / OSD / PCMCIA / PLAYBACK / PQC / PRCG / PVR …`。
- **キャラクタデバイスの major（`/proc/devices`）**: `220 /dev/fe_main`（フロントエンド枠）、
  `230 val_*`（AV 多重・多数の minor）、`231 val_demux`（minor 0/1/2 = 3 波）。
  **`210 tsout_demux` は `/proc/devices` に無い**＝**major 210 は現在未登録**。node は rootfs に
  静的に切ってあるが裏のドライバが今は載っていない＝**TS-out 経路を開いたときに on-demand で
  登録される休眠ノード**。`/dev/tsout_demux` は `crw-rw----`（root 限定）で他の val_*（rw-rw-rw-）
  より権限が絞られている。
- **ベンダ `lindberg` が開いている AV デバイス列**（全スレッド共通・`/proc/<pid>/fd` 観察）:
  `fe_main → val_demux / val_demux1 → val_dscram → val_record → val_video`＋`val_scard`。
  = 選局(fe_main) → PID フィルタ(val_demux) → B-CAS 解除(val_dscram) → 録画(val_record) /
  デコード(val_video)。**この idle 状態では `tsout_demux` は誰も開いていない**（＝録画/配信が
  走ったときだけ開く）。
- **`val_record` / `val_pvr_trick` という録画/PVR 系 minor が居る**（R グレード）。YB1 では
  見なかった系統。
- **配信ポート**: `netstat` で `0.0.0.0:49152`（UPnP）・**`50125`/`50127`**（lindberg の TS 配信/
  制御）が LISTEN。エリアフリー＝placeshifting のリモート視聴に TS を TCP で流す口。観察に
  `tcpdump` が使える（rootfs 同梱・`strace` は無い）。
- **裏 CPU(CSD) は稼働中**（`/proc/interrupts`: `CSD_API_RESPONSE` 178 万・`CSD_INTEVENT00`
  367 万・`emma-iic` 116 万）。AV コプロセッサは idle でも回っている。

### 5.1 demux ABI は YB1 と同一（76 本）＋ StreamOut の受け手

- **`MMAC_DEMUX_*` は 76 本**＝YB1 §49.5/§70.6 の demux 関数表と**同数・同名**
  （`AllocatePayloadFilter` / `EnablePidFilter` / `SetPidFilterBuffer{,WritePointerOffset}` /
  `InstallPcrCallback` / `InstallPesCallback` / `InstallSectionCallback` / `GetPcrValue` /
  `SetDeviceInputSource` / `CpuInputPlay` …）。∴ **§49–§72 の demux 制御がそのまま転用できる。**
- TS の出口は 3 系統: **PidFilterBuffer**（PSI/SI・セクション）、**PayloadBuffer**（PES）、
  **StreamOut**（`MMAC_DEMUX_{Open,Close}StreamOutDevice` / `SelectStreamOutGroup` /
  `Start/StopStreamOut`）。**76 本に「生 188B を DRAM に落とす関数」は無い**（YB1 §70.6 と同じ）。
- ★ **YB1 との決定的な差**: YB1 §71.3 は「StreamOut は動くがこの基板に受け手がいない（ピン）」
  と結論した。BR310L は R グレード録画機で、**StreamOut の受け手が実在する**——
  `tsout_demux`(major 210・on-demand 登録) ＋ `val_record` ＋ 配信ポート 50125/50127。
  ∴ **BR310L の生 TS 経路は「StreamOut → tsout_demux」**であり、demux バッファ読みではない。
  これが段階 C の本線候補。
- `val_scard` は B-CAS への PC/SC トランスポート先＝スコープ内（復号はスコープ外・§4.5 CLAUDE.md）。

### 5.2 フロントエンド I2C の地形（観察・段階 C の足場）

`lsmod` / `/proc/devices` / `/sys/bus/i2c` / kallsyms のシンボル名から（値の中身は転写しない・
実装は mainline `tc90522.c` ＋ 実バスの観察から起こす）:

- **SoC の I2C**: `/dev/i2c-0`/`-1`（major 89・i2c-dev・名前 `emma_i2c_0`/`_1`）＋ `-2`/`-3`。
  Renesas GPL の `i2c_algo_emma`/`i2c_emma`。`yb1-i2c` が `/dev/i2c-N` に直接届く（ベンダ非依存）。
  **注意（YB1 の教訓）**: bus 0 は何も載っておらず転送がアダプタ内でブロックする＝走査しない。
- **`ei2c_ofdm_drv`（`/dev/ei2c`・major 222）は、GPL の emma アダプタに付く I2C クライアント
  ドライバ**。`demod_tc90522_90532_drv` はこれを通して復調器と話すが、線上のやり取りは
  **バス 1（`emma_i2c_1`）**を流れる（§5.3。2026-10-12 に訂正）。
- **復調器 TC90522/90532 は 2 つのスレーブアドレスを持つ**: `s_slave_addr_ofdm`（ISDB-T 地上波）
  と `s_slave_addr_psk`（ISDB-S 衛星）。T/S それぞれにレジスタ表（`demod_90522_T/S`・
  `demod_90532_T/S`）。**チューナ（MxL601RF/135RF）は demod の I2C ゲートの奥**にいる
  （`Tuner_set_slave_addr` が demod 側でチューナのスレーブアドレスを設定）。
- ∴ **クリーン選局の足場**: mainline `tc90522.c`（T=地上/S=衛星の 2 コア・ゲート付き I2C リピータ）
  ＋ MxL601（`mxl301rf`/`mxl5007t` 系）＋ 実バス観察。アドレス・レジスタ意味論は datasheet と
  mainline から取り、ベンダ .ko の内部レジスタ表は転写しない（clean-room）。

### 5.3 ★ フロントエンド I2C は GPL の emma アダプタのバス 1（「GPIO ビットバング」は誤りだった）

**2026-10-12 訂正。** 2026-09-26 にこの節は「`ei2c_ofdm` は GPIO をビットバングしている」と
結論していたが、**誤り**だった。フロントエンドは YB1 と同じく、**GPL の `i2c-emma`（バス 1）**に
載っている。根拠（すべて実機・観察）:

- `ei2c_ofdm_drv.ko` の輸入シンボル（`readelf -s`）は `i2c_register_driver`・`i2c_attach_client`・
  `i2c_master_send`・`i2c_master_recv`・`i2c_smbus_read_*` と、メモリ・chardev のものだけ。
  **GPIO の関数も `ioremap` も輸入していない**＝自分でピンを動かす手段を持たない。
- `/sys/bus/i2c/drivers/ei2c_drv`（モジュール `ei2c_ofdm_drv`）が、2 つのアダプタに
  クライアント `0-0000`・`1-0000` を付けている。
- `/proc/interrupts` の `emma-iic`（Linux IRQ 65＝`EMMA_IRQ_PIIC1`）が、選局と状態読みに
  合わせて増える。IRQ 64（バス 0）は 0 のまま。
- **GPL の `i2c-algo-emma` に記録を足したもの（カーネルパッチ 0002）をベンダより先に load すると、
  復調器とチューナへの全メッセージがバス 1 に出る**（[フロントエンド仕様](sb-tv06-wrip-frontend.md)）。

誤りの原因: ① `lsmod` の `i2c_emma` が「used by 0」だった（クライアントが付いていても
参照数には現れない）、② `/proc/iomem` に I2C の枠が無かった（emma のアダプタ自体が枠を
出していない。`ei2c` に固有の話ではない）、③ 選局中に zpio のデータレジスタの
GPIO29・GPIO0 が激しく動いた。③は **I2C のピンが GPIO と兼用で、データレジスタにピンの
電位が映る**ためで、ビットバングの証拠ではなかった（GPIO29＝SCL・GPIO0＝SDA という対応は
その意味では合っている）。**「選局しないと `/dev/i2c-1` に復調器が出ない」も誤り**で、復調器は
待機中（スリープ）でも ACK する（待機に入れる操作も I2C で行われている）。

以下は 2026-09-26 の記録のうち、いまも有効なもの:

- **モジュールライセンス**（`.ko` の `.modinfo` の `license=` キーを raw バイトから観察）:
  `i2c-algo-emma`/`i2c-emma` = **GPL v2**、`h57fmb`(Dione) = **GPL**、`mem2` = **Dual BSD/GPL**、
  `val_merge` = **Proprietary**（明示）、`ei2c_ofdm_drv`/`demod_tc90522_90532_drv`/`fe_main_drv`/
  `fe_common_drv`/`tuner_*`/`io_controller_drv`/`csd` = **MODULE_LICENSE 宣言なし**（空。カーネルは
  これらと val_merge で `Tainted: P`）。**「宣言なし」は「著作権的にプロプラ確定」ではない**が、
  いずれも**ソースを持っていない**＝mainline＋観察で書く点は変わらない。GPL カーネル木にも無い。
  - **GPL ソースの所在と検証**（2026-09-26 調査）: 正しい EMMA3SEP の公開 GPL ページは
    **`pixela.co.jp/gpl/bmmpx1301/`（「地デジチューナー(R)」）**。ここが提供する EMMA3SEP 版は
    **`Linux_kernel_2.6.27.39_r557_001.001.008-r06.tar.bz2` の 1 本のみ**＝
    `br-ext-chip-renesas/board/emma3sep/kernel/build.env` が pin している**手元の r557 ツリーと同一**。
    公開ページ (`sbs_dtvtuner`・`wtv`) は別で、いずれも Fujitsu MB86H61 / 2.6.31 / FAPex（SB-TV02 系）。
    - **retail 対応＝実証済み（OEM 同一プラットフォーム）**: 手元の純正コンテナを開封して比較。
      **PIX-BR310L(`pix_br310l_v2.1.3`) と PIX-BR310W(`pix_br310w_v1.0.0/0.1`) は族タグ `BR310L`**、
      **SB-TV06-WRIP(`POE-BR310L-SB1_v1.0.2`) は族タグ `LINDBERG`**。しかし PIX-BR310L の
      **vmlinux は `Linux 2.6.27.39 … gcc 4.3.5 Buildroot 2010.11 #160 (2014)` ＝ EMMA3SEP**
      （WRIP は同 2.6.27.39 の `#16`）。ペイロード 8 点の**サイズは一致**（イメージ md5 は build 差で相違）。
      `pix_br310l_v2.1.3` の本体長は WRIP と**バイト長一致**。∴ **ピクセラ retail(BR310L 族) と
      ソフトバンク OEM(LINDBERG 族) は同一 silicon(EMMA3SEP) の別ブランド／別ビルド**。
      GPL は共通で `/gpl/bmmpx1301/`(r557)＝手元の物。retail 側にも別リビジョン GPL は無い。
    - **実際にダウンロードして検証（2026-09-26）**: `/gpl/bmmpx1301/data/Linux_kernel_2.6.27.39_
      r557_001.001.008-r06.tar.bz2`（52,488,565B・**sha256 が build.env の pin と一致**＝正真の完全 tar）
      を取得。**⚠ 手元の `temp/gpl/BMMPX1301/{full,x,bb}` は 22/23/4 ファイルの部分展開に過ぎず不完全**
      （過去セッションが必要分だけ抜いた・DrvFs 展開失敗の疑い）。**認定ソースは `dl/…r557….tar.bz2`**
      （canonical 名で配置済み）。
    - **完全アーカイブ 26,583 ファイルで再検証**: `arch/mips/emma` は SoC 共通 13 本のみ（board variant
      無し）。**`ei2c/_ofdm/tc905/mxl601/mxl135/8263/demod/dione/h57/fapex/br310/bmmpx` は全 26,583 中 0 件**
      ＝ **frontend/demod/tuner のソースは GPL 未公開**（Fujitsu 側の `FAPI_FAPexK/U` 相当も無い）。∴
      **SCL/SDA の GPIO ピン同定は観察が clean-room の正道**（確定・完全アーカイブ根拠）。
    - **★ 収穫**: 完全カーネルには **`drivers/i2c/busses/i2c-gpio.c`（Atmel 2007・GPLv2・i2c-algo-bit +
      GPIO API の bitbang I2C）** と `i2c-emma.c/.h` が入っている（手元の部分ツリーには**欠けていた**）。
      ∴ `wrip-ei2c` は**この mainline i2c-gpio を土台に `Origin: gpl-derived`** で書ける（ゼロから起こさない）。
      demod は TC90522 が 2.6.27 mainline に**無い**（後代でマージ）＝**新しい mainline の `tc90522.c` を別途
      参照**、tuner は `mxl301rf`/`mxl5007t` 系。ピン値のみ観察で補う。
- ∴ **段階 C のフロントエンドのトランスポートは、YB1 と同じ GPL の I2C（バス 1）**。
  自前のビットバングは要らない。その上に mainline `tc90522.c` の意味論で TC90532、復調器の
  ゲート越しに MxL601（地上）と TDA8263（衛星）。**プロプラ .ko の中身は読まない・転写しない。**
  線上の列は [フロントエンド仕様](sb-tv06-wrip-frontend.md)。

## 6. 未確定・次アクション

> **2026-10-12: この節の 1〜3 は §10 で決着した。** 生 TS は YB1 と同じ「AV の SRAM の TS 入力
> FIFO」から取れる（StreamOut／`tsout_demux` は要らない）。裏 CPU の合図・コマンドは YB1 と同一。
> 復調器は [フロントエンド仕様](sb-tv06-wrip-frontend.md) の手順で、ベンダのモジュール無しに
> 選局できる。**いまの残件は §10.5。** 以下は 2026-09-26 時点の記録。

1. **StreamOut → tsout_demux の実体（最優先）**: `MMAC_DEMUX_OpenStreamOutDevice` →
   `SelectStreamOutGroup` → `StartStreamOut` を叩くと `tsout_demux`(major 210) が登録され、
   そこから生 TS(188B) が読めるか。読めれば YB1 §71 の壁が BR310L には無い＝段階 C（chardev 契約）の
   見通しが大きく変わる。**観察の順**: (a) ベンダが録画/配信している最中の TS を `tcpdump`
   （50125/50127）と `tsout_demux` の登録有無で見る（要 RF 信号）→ (b) それが無理なら段階 C で
   StreamOut を自前で駆動（電源リセットの規律・§31.3）。demux 制御は §5.1 のとおり §49–§72 転用。
2. **裏 CPU の合図が YB1 と同一か**（§72：`word0=0x00020000` の保留印・`0x100001b0` の鳴らし・
   `SUB_INT_EN_0`）。demux 関数表 76 本は一致済み（§5.1）なので、合図も一致すれば PSI/SI・PES・
   PCR/PTS はベンダ `.ko` 無しで取れる。`yb1-mem`/`yb1-mmac`（/opt/openird 同梱）で裏取りする。
3. **復調器のクリーン実装**: TC90532 は mainline `drivers/media/dvb-frontends/tc90522.c`、
   MxL601 は `mxl301rf`/`mxl5007t` 系が参考。I2C 観察（`i2c-algo-emma`）＋ mainline 意味論で
   選局・ロック・C/N をクリーン実装（`Origin: gpl-derived` ＋ `Spec-Ref`）。ロック判定レジスタは
   YB1 の `0x112d` 相当を実機 I2C トレースで当てる。
4. **B-CAS(PC/SC)**: `val_scard` 経由のカード ID 読み取り（スコープ内）。物証は撮影済み。
5. **危険操作（YB1 から持ち越し・BR310L で未確認）**: `dione_transcode -w`（SPI-ROM 書き込み・
   setup.sh がベンダ起動時に呼ぶ正規操作。単発で叩かない）、`val_demux` の危険 nr、`0x11000000`、
   `0x100001b4` 書き込み、BHIF ページのウォッチポイント、`val_dcnv`（鍵を持つ DES 変換器）。
   **未知 nr の総当たりは避ける。**

## 7. 実機リグの現状（2026-09-26）

- NAND は telnet-root 入り LINDBERG コンテナを投入済み（rootfs = mkshell 版・tag `WRIP1`）。
  kernel/app/emma_fw は純正と同一（コンテナ内の 7 点はバイト一致）、mtd0 不変。
- `/app/setting/devel_mode.txt` 作成済み＝純正 rootfs に戻しても telnet は上がる。
- IP は DHCP で `192.168.11.13`（MAC `00:E0:C3:xx:xx:xx`）。`lindberg` は通常起動＝視聴も可。
- `/opt/openird` に静的 ELF（`yb1-{ioctl,mem,mxl,i2c,poke}`・`yb1_trace.so`）を同梱済み。
  トレースは `/app/setting/openird-trace` / `openird-i2ctrace` / `openird-pre.sh` で切り替え
  （既定は無効）。焼き直し無しで段階 B ⇄ 通常を行き来できる。
- **シリアル root シェルが開いている**: **COM3 = `/dev/ttyS1`**。mkshell リグが rcS で
  `getty -L 115200 ttyS1` を起動しているので、`buildroot login: root`（パスワード無し）で
  即 `#`（uid=0）。∴ **root への口は 2 系統（シリアル ttyS1 ＋ telnet）**＝noblur で
  ネットワークが壊れてもシリアルで救える。
- **ただしカーネルの起動/panic メッセージはシリアルに出ない**（製品 bootargs に `console=` 無し・
  §2）。カーネルログは shell から `dmesg`、AV の Oops は `/tmp/csd.log`（csd drain）で見る。
  live なカーネルコンソール（panic の実時間表示）を得るには mtd1 の U-Boot env に
  `console=ttyS1,115200` を足す必要があるが、env 書き換えは起動を壊す危険があり保留
  （U-Boot 実行ロックで失敗時のフォールバックが効かない）。
- 段階 C のリグ切替は焼き直し不要（`/app/setting/openird-pre.sh` ＋ フラグ）。WRIP 版フックは
  [`tools/vendor-rootfs/openird-pre-wrip.sh`](../../tools/vendor-rootfs/openird-pre-wrip.sh)
  ＝**1 回だけ noblur** の安全フラグ `openird-noblur-once`（読んだら自分で消す＝次回は通常起動＝
  UPnP 復帰＝fwup で救える）／静的 IP（既定 192.168.11.20）＋ DHCP 保険／lindberg が入れる
  frontend・AV モジュール一式の代替 load を持つ。

### 7.1 noblur リグ稼働（実機・2026-09-26）

`openird-noblur-once` ＋ 電源リセットで **lindberg 抜きの排他環境が起動することを実機確認**:

- `lindberg`=0 / `cmd_executor_server`=0（ベンダアプリは起動しない＝AV 資源を誰も押さえない）。
- フックが代わりに load したモジュール: `mem2 i2c-algo-emma i2c-emma io_controller_drv
  ei2c_ofdm_drv fe_common_drv fe_main_drv demod_tc90522_90532_drv tuner_MxLinear_MxL601RF_drv
  tuner_MxLinear_MxL135RF_drv tuner_PHILIPS_TDA8263HN_drv val_merge` ＋ `csd`（CSD_DEBUG=1）。
  裏 CPU FW（mtd5 の `emma3sep_driver_std_op0.mem`）を phys `0x00030000` に配置済み。
- **root への口 2 系統**: シリアル `ttyS1`（getty）＋ telnet。**IP は DHCP 保険が静的 .20 を上書き
  して `192.168.11.13` になった**（udhcpc fallback が勝つ＝リグの IP は毎回 `fwup discover` か
  シリアルの `ifconfig` で確認する。決め打ちしたいなら静的優先にフックを直す）。
- 復旧: 次の電源投入は通常起動（フラグ消費済み）＝lindberg/UPnP 復帰＝fwup で救える。
- **AV 実験ごとに電源リセット**（AV 状態はプロセス終了で消えない・§31.3）。恒久 noblur が要る
  段になったら `openird-noblur`（消えないフラグ）に替える（救済はシリアルのみになる点に注意）。

### 7.2 次の実装ステップ（段階 C フロントエンド）

排他環境ができたので、次は TC90532/MxL601 のクリーン選局を起こす:

> **2026-10-12: この節の 1〜3 は前提が誤っていた（§5.3 の訂正）。** フロントエンドは GPL の
> I2C（バス 1）に載っており、ビットバングの再実装（`wrip-ei2c`）は要らない。下の 2 の測定は
> 「I2C のピンの電位が GPIO のデータレジスタに映る」ことの記録として残す。いまの手順は §10。

1. **I2C 地形の同定**（§5.3）: ~~frontend は GPL emma i2c ではなく プロプラ ei2c_ofdm の
   GPIO bitbang~~ → **GPL の emma I2C のバス 1**。bus 0 は走査しない。
2. **SCL/SDA ピン（2026-09-26・観察）**: 選局中に **zpio_data[0]（GPIO0-31）** の 2 本が動く
   （I2C のピンの電位）。測った手順: UPnP `X_SetTuning`(tb) で選局→lindberg が復調器を
   連続ポーリング→`yb1-mem 0x10110d00 0x38 spin` で `zpio_data[0]` の全変化を記録→ビット反転数を集計。
   結果（tune 後の 2s・3000 変化）: **GPIO29=2248 反転(≈50%H・クロック)＝SCL、GPIO0=1445(≈48%H)＝SDA**
   が主バス（地上/OFDM 復調器）。**GPIO1=550・GPIO9=448（常時 H 寄り）＝活動の少ない第2バス**
   （衛星/PSK 復調器の可能性）。SCL/SDA の割当は「反転数が多い＝SCL」の定石＝`wrip-ei2c` の
   アドレス ACK 試験で最終確認する（2 通り試せば確定）。**⚠ 復調器 I2C の観察には選局が要る**
   （idle では I2C が流れない＝churn 0）。捕捉は foreground `spin`（background だと SIGHUP や
   即終了で取れない）。
3. **`wrip-ei2c`（GPIO bitbang I2C master）**: GPL `drivers/i2c/busses/i2c-gpio.c` を土台に、
   SCL=GPIO29 / SDA=GPIO0 で zpio を叩く（GPL `emma-gpio.c` の zpio_data/mask/dir。base 0x10110d00・
   data +0x00 / mask +0x18 / dir +0x30）。`Origin: gpl-derived`。まず TC90522 のアドレス scan で
   SCL/SDA 割当と復調器アドレスを確定。
4. **`wrip-fe`**: ③の上に mainline `tc90522.c`（T/S 2 コア・ゲート付き I2C リピータ）＋ MxL601 の
   意味論で選局→ロック→C/N をクリーン実装（`Origin: gpl-derived` ＋ `Spec-Ref`）。ロック判定は
   TC90522 の CN/ロックレジスタ（mainline にロジックあり）。
5. **demux → StreamOut → tsout_demux**: §5.1 の 76 本 demux ＋ StreamOut で生 TS を
   `tsout_demux`(major 210) に落とせるか（§6-1）。§49–§72 転用。

## 8. GPL 参照ツリーの土台（長期作業の前提・コンテクストで失いやすいので明記）

段階 C は長い反復になる。その土台となる「どの GPL を、どこに、どう用意したか」を固定する。

### 8.1 認定 GPL ソースと展開場所

- **tarball（pin 済み・唯一の正）**: `dl/Linux_kernel_2.6.27.39_r557_001.001.008-r06.tar.bz2`
  （Pixela 公開 `/gpl/bmmpx1301/data/` から DL・**sha256 = build.env の pin と一致**を確認）。
  EMMA3SEP 系（YB1 / SB-TV06-WRIP / retail PIX-BR310L/W）は**全部この 1 本**。
- **完全参照ツリー（ext4・25,021 ファイル）**: `~/.openird/gpl/emma3sep/linux-2.6.27.39`。
  生成/再生成は **`scripts/prepare-gpl-kernel.sh`**（冪等・sha256 検証・ファイル数 >=10000 を要求）。
- **⚠ DrvFs(`/mnt/c`=リポジトリ)へ tar 展開すると不完全になる**（権限/utime で途中停止。
  実例: 旧 `temp/gpl/BMMPX1301/{full,x,bb}` は 22/23/4 ファイルで止まっていた＝**信用しない**）。
  ∴ 参照ツリーは **WSL の ext4** に置く。grep/read は `wsl -e bash -c '… ~/.openird/gpl/…'`。
- ビルド用の展開は別（`build-kernel.sh`/`build-yb1-kmod.sh` が `output/` にパッチ込みで展開）。
  §8 の参照ツリーは**未パッチ vanilla**＝mainline のコードをそのまま読む用。

### 8.2 frontend 実装に効く GPL の事実（この参照ツリーで確認）

- **GPIO(zpio) は GPL で完全に分かる**:
  - `include/asm-mips/emma/emma3sep.h`: `EMMA_ZPIO_BASE = EMMA_TIMER_BASE + 0x0d00`
    （= phys `0x10110d00`。`/proc/iomem` の `zpio.0` と一致）。
  - `drivers/usb/host/ehci-emma.c`: **FWID(PIX_AIRTUNER_W/R/LITE)ごとの GPIO 操作**が GPL にある。
    アクセスは `*(volatile u32*)(EMMA_ZPIO_BASE | EMMA_BASE | 0x04)`（+0x04 = データ）。
    実名ピン例: `GPIO[23]=USB1_ON` `GPIO[24]=USB_HUB_RST` `GPIO[36]=USB_POWER_ON_TAB_H`。
    ※ ただし `PIX_AIRTUNER_BR310L`(WRIP) の分岐はこの r557 には**無い**（R="Nevada基板"が最も近い）。
  - `arch/mips/emma/common/gpio.c` ＋ `emma3sx/platform.c`: zpio の gpiolib 登録。
  - **`drivers/i2c/busses/i2c-gpio.c`（Atmel・GPLv2・i2c-algo-bit + GPIO API）**が入っている。
    SCL/SDA の GPIO 番号を platform_data で受ける汎用 bitbang I2C＝**標準の `/dev/i2c-N` を生やせる**。
- **frontend/demod/tuner のソースは GPL に無い**（`tc90522`/`tc90532`/`MxL601`/`MxL135`/`ei2c_ofdm`
  の厳密文字列は全 25,021 ファイルで **0 件**。demod の SCL/SDA ピンも GPL に無い）。
- ∴ **段階 C frontend のクリーンな組み立て方（2026-10-12 に改めた。§5.3 の訂正）**:
  1. トランスポートは GPL の `i2c-emma`（バス 1）をそのまま使う。`i2c-gpio` は要らない。
  2. その上に**後代 mainline の `tc90522.c`**（2.6.27 には未収録＝別途 kernel.org から参照）の
     意味論で TC90532、衛星のチューナは mainline `tda826x.c`、地上の MxL601 は線上の列
     （[フロントエンド仕様](sb-tv06-wrip-frontend.md)）から（`Origin: gpl-derived` ＋ `Spec-Ref`）。

## 9. 受信・TS パイプラインの観察（視聴・録画・2026-09-26）

`LD_PRELOAD` ioctl トレーサ（`yb1_trace.so`）＋ `/dev/mem` の TS 走査（`yb1-mem scan/stats`）で、
ベンダ `lindberg` の視聴・録画中の**経路**を観察した。**復号の鍵・手順には触れない**（存在と
パイプライン上の位置だけ）。ioctl の分解は uapi `_IOC`、TS 走査は 0x47 同期の統計＝観察のみ。

### 9.1 選局（frontend）— `/dev/fe_main`（type=0xb0）

実チャンネルをアプリで選局したときの `fe_main` の write コマンド:

| nr | 意味（観察） | 地上波 | BS |
|---|---|---|---|
| 0x03 | 波/モード選択 | `0x01`（地上） | `0x02`（BS） |
| 0x08 | **周波数（MHz・16bit）** | `0x022d`=**557**（NHK総合＝物理27ch=557MHz） | `0x0526`=1318 |
| 0x0a | **BS 固有**（TSID/トランスポンダ選択の可能性） | 無し | `0x40f1` |
| 0x04 | ステータス polling（`00 01 fc e0`→64B 構造体） | ○ | ○ |
| 0x05/0x06/0x07 | CN/lock/BER 読み（`nr=0x07`=BER・`0x0001869f`=99999 で未ロック） | ○ | ○ |

- ★ **周波数エンコードは MHz**（アプリは 557 を渡す）。UPnP `X_SetTuning` に kHz(557143) を渡すと
  16bit 切り詰め（`0x88057`→`0x8057`）で化けて未ロックになる＝**MHz で渡すべき**。
- ロック判定は `nr=0x07`(BER) 等。**ロックには実在チャンネルの周波数が要る**（当てずっぽうは SignalStrength=0）。

### 9.2 復号はインライン、生 TS バッファは平文のみ

DRAM 全域（256MB）を `stats` で走査（視聴中・地上波と BS の両方）:

- **クリーンな 188B 整列 TS バッファは `0x0a000000` と `0x0b000000` の2つだけ、いずれも
  `scrambling_control=none`＝平文**（復号後）。
- `0x00000000`–`0x09000000` にスクランブル片（even/odd）は散在するが**すべて desync 数万＝
  整列した持続バッファではない**（作業領域中の断片）。**クリーンなスクランブル TS バッファは
  DRAM に存在しない。**
- ∴ **`demod → demux → 復号(`val_dcnv`/`val_m2dsc`) → 平文 DRAM` はハード/インライン**。ベンダは
  トランスコード配信/録画のため常に復号する。**★重要（訂正）: 日本の地上波もスクランブルされて
  おり B-CAS が要る**＝平文バッファは「復号済み」を意味する（当初「地上波は無スクランブル」と
  書いたのは誤り）。
- **∴「暗号化状態の TS」の捕捉設計**: 受動的に読めるスクランブル TS バッファは無い。OpenIRD は
  **demux（§72 クリーン制御）で PID フィルタ後の TS を DRAM に書かせる際、復号ステージを
  バイパス**する → その出力にスクランブル TS が落ちる。**scope 準拠**（復号は下流＝正規 B-CAS＋
  PC/SC）。demux 制御は §5.1 の 76 本 ABI＋YB1 §49–§72 の転用。

### 9.3 視聴（place-shift）の経路

`fe_main`(選局) → `val_demux`(PID フィルタ・nr=0x0c=PID/0x0d=有効化/0x2f=書込ポインタ poll、
YB1 ABI と一致) → `val_dcnv`/`val_m2dsc`(復号) → **`h57fmb0`/Dione(トランスコード)** → ネット配信。
**`tsout_demux`/StreamOut/`val_record` は視聴では 0 回**＝place-shift は生 TS 出口を使わず H.264 化して配信。

### 9.4 HDD 録画の経路

USB HDD（`/dev/sda1`・約1TB・**非 FAT 独自フォーマット**・マウントせず raw ブロック）。録画中に
lindberg が open しているのは **`/dev/val_record`（録画TS経路）＋`/dev/val_pvr_trick`（PVR制御）＋
`/dev/sda`**。**`tsout_demux` は録画でも未使用**。書込みは ~95KB/write × ~25回/秒 ＝ **約19Mbps**
（BS HD 相当）。

- ∴ **R グレードの録画も「生 TS 出口(tsout_demux/StreamOut)」を使わず `val_record`→sda**。
  YB1 §71（tsout_demux 未使用）と一致。生 TS の chardev 化は **demux 出力の自前確保（§72）が本線**で変わらない。
- 録画ファイルは復号後にローカル鍵で再暗号化（機器紐付け）の公算＝平文 TS ではない見込み。
  **録画内容そのものは読まない**（コピー保護＋scope 外）。

### 9.5 観察の運用メモ

- **録画中はトレースが猛烈に増える**（demux polling で 32MB/数分）＝ /tmp(51.9M・tmpfs) を溢れさせ
  録画に影響する恐れ。**低負荷のモニタは `/sys/block/sda/stat` の sectors_written**（内容非依存）。
- `LD_PRELOAD` トレースは `/app/setting/openird-trace` フラグ＋再起動で入切。ログは `/tmp/yb1-trace.log`。

## 10. 段階 C の見通し＝YB1 の AV 側がそのまま当たる（2026-10-12）

実機（ベンダ FW 1.0.2 r1）での観察。**限定受信には一切触れていない**（デスクランブラの
デバイスは開かず、ECM/EMM を含めパケットの中身は解釈しない）。

### 10.1 AV 側は YB1 と同じ物

| 比べたもの | 結果 |
|---|---|
| 裏 CPU の FW `emma3sep_driver_std_op0.mem`（mtd5・2,734,892 B） | **YB1（v2.1.17）のものと sha256 が一致** |
| `csd.ko`・`ei2c_ofdm_drv.ko`・`io_controller_drv.ko`・`mem2.ko`・`i2c-emma.ko`・`i2c-algo-emma.ko` | 一致 |
| `val_merge.ko`・`fe_*`・`demod_*`・`tuner_*`・`h57fmb.ko` | 大きさは同じか近いが中身は違う（別ビルド） |
| カーネルの設定（`/proc/config.gz`） | YB1 から起こした OpenIRD の `linux.config` との差は、loop と I2C が `=m` か `=y` かだけ |
| 実機の読み（ベンダアプリ動作中） | BHIF `+0x180 = 0`・`+0x190 = 0x80030000`・`+0x1ac = 0xa0010000`・`+0x1b4 = 0x00070000`、エントリ表の形、FIFO の制御ブロック（SRAM `0xec00`〜・`0x130a0842`・`0xbc`・`0x471fff00`）、スロットの番地、PID 表（`0x1003066c`）が **YB1 の仕様どおり** |

∴ [AV のコマンド経路](poe-br100-yb1-av-command.md) と [生 TS の取り出し](poe-br100-yb1-ts-fifo.md) は
この機種にもそのまま当たる。YB1 のベンダカーネル向けに作ったモジュール（記録つきの
`i2c-algo-emma.ko`・`fifodrain.ko`）は、作り直さずに load できた。

### 10.2 ★ TS 入力 FIFO には、ベンダアプリの動作中もスクランブルされたままの TS が入る

ベンダアプリに UPnP で選局させ、SRAM の FIFO のヘッダだけを読んだ（`yb1-mem … fifo`）:

| 選局 | FIFO 2 の映像 PID `0x0100`（1 秒間に読めたヘッダ） |
|---|---|
| 地上 557 MHz | 7,816 本のうち 7,566 本が `scrambling_control` = 3、250 本が 0 |
| BS 1318 MHz（BS-15） | 11,236 本のうち 10,881 本が 2 または 3、355 本が 0 |

- ∴ **FIFO にはスクランブルされたままのパケットが届く**（デスクランブラは FIFO の入口より後ろ）。
  ベンダアプリの動作中に約 3% が 0 と読めるのは、スロットに入った後で処理されたものを読んだと
  みられる【推定】。**デスクランブラを開かない構成（§10.4）では、0 と読める映像パケットは無かった。**
- §9.2 の「生 TS のバッファは平文のみ」は DRAM を走査した結果で、SRAM の FIFO は見ていなかった
  （FIFO は 2026-09-27 に YB1 で見つかった）。「復号の段を迂回する」工夫は要らず、YB1 と同じ
  取り出しでスクランブルされたままの TS になる。
- **入力源は波で変えない**。ベンダは起動時に `nr=0x05` を 3 回出す（機器 0 と機器 2 に `3`、
  機器 1 に `4`）だけで、地上・BS・CS を切り替えても出し直さない。どの波の TS も同じ FIFO に出る
  ＝復調器の TS 出力は 1 系統（[フロントエンド仕様](sb-tv06-wrip-frontend.md) §3）。

### 10.3 ベンダアプリの起動時の呼び出し（`LD_PRELOAD` の記録・参考）

- `/dev/val_demux`: `nr=0x01`（Initialise）→ `nr=0x02`（デバイスを開く・`val_demux` で 2 回、
  `val_demux1` で 1 回）→ `nr=0x05`（入力源）→ `nr=0x65`/`0x64`/`0x48` → フィルタの確保（種別 6・7）。
  YB1 の列（AV のコマンド経路 §3）と同じ形で、ハンドルも同じ値（`0x00010000`／`0x01010001`／
  `0x02010002`、フィルタ `0x0210006b`〜）。
- `/dev/fe_main`: フロントエンド仕様 §7。
- `/dev/io_controller`: レジスタの読み書きの口。毎秒 1 回の書き込み先は GPL の `emma_wdt.0` の
  枠（`0x10110900`〜）＝**ベンダアプリがウォッチドッグを蹴っている**（アプリを止めると基板ごと
  再起動するのはこのため。アプリ抜きの起動ではウォッチドッグは始まらない）。
- `val_dscram`・`val_m2dsc`・`val_dcnv`・`val_scard` も開かれ、呼ばれている（存在の列挙まで。
  引数は見ていない）。

### 10.4 ★ 段階 C の試行＝ベンダアプリ抜きで、地上と BS の生 TS が取れた

リグのフックの `openird-noblur-once`（1 回きり）で起動し、次の順に行った。**ベンダのものは、
裏 CPU の FW・`csd.ko`（起こすだけ）と、ピンを上げるのに使った `gpiotool` だけ。**

| 手順 | 結果 |
|---|---|
| 起動直後 | 裏 CPU は走っている（`+0x180 = 0`）。**復調器は I2C に応答しない**（`-EREMOTEIO`） |
| `gpiotool set 43`（SoC の GPIO43 を 1 に） | 復調器が応答する＝**フロントエンドの電源**（フロントエンド仕様 §1） |
| `wrip-fe.sh init` → `t 557`（`/dev/i2c-1` だけ） | ロック。C/N・AGC はベンダの表示と同じ値 |
| `rmmod val_merge csd` → `yb1-cmd min-rawts.cmd`（最小の 6 種・24 本） | **24 本すべて状態 0**。ハンドルは YB1 と同じ値 |
| FIFO のヘッダ | FIFO 2 に映像 `0x0100`（毎秒 9,273 本・すべてスクランブルされたまま）・音声・PCR・PAT、FIFO 0 に SI |
| `fifodrain.ko`（CPU で写す・10 秒） | **106,755 本・破れ 0・連番の途切れ 0** |
| 同（XDMA・10 秒） | 105,821 本・1 本欠けが 6 回（Wi‑Fi と Dione のモジュールを外す前は 23 回） |
| `wrip-fe.sh s 1318`（BS-15）→ 同じフィルタのまま | ロック。FIFO 2 に映像（毎秒約 11,000 本・スクランブルされたまま） |
| `fifodrain.ko`（BS・10 秒） | **CPU で 134,558 本・XDMA で 132,258 本、どちらも破れ 0・途切れ 0** |

- ∴ **この機種の段階 C は「YB1 のドライバ（`ird-tuner-emma3sep`）＋新しいフロントエンド」で届く。**
  新しく要るのは、GPIO43 の電源、TC90532・MxL601・TDA8263 の I2C の手順
  （フロントエンド仕様）、衛星の TS の選択（同 §5）だけ。
- 地上の XDMA での 1 本欠けは、YB1 でも見えていた課題（[生 TS の経路](../poe-br100-yb1-ts-drain.md)
  §4）と同じ形で、この機種では試作のドレイナーで頻度が高かった。CPU で写せば 0。
- 参照受信機とのビット単位の照合は §10.8（地上と BS は全数一致）。

### 10.5 残件

1. ~~参照受信機との照合~~ → **済み**（§10.8）。地上 3 波と BS-15 の 2 つの TS は全数ビット一致。
   CS は化け 0 だが、試作では取りこぼす（下の 3）。
2. ~~LNB への給電とアッテネータの場所~~ → **決着**。アッテネータは復調器の地上側 `1c` の bit4
   （フロントエンド仕様 §6.3）。**アンテナへの給電は、この機種のハードに無い**（製品のガイドに
   明記・端子の実測 0 V・設定を切り替えても何も動かない。同 §8.1）。
3. **CS の流量への対処**（§10.6）。全 PID の流量は測った。CS は試作の XDMA の形では数 % 落ち、
   CPU で写すと欠けは 0 だが CPU を 4 割強使う。製品のドレイナーでの振り分けと写し方が課題。
4. ~~カードの口（`val_scard`）~~ → **運び役は観察済み**（[カードの口](sb-tv06-wrip-card-slot.md)）。
   B-CAS ではないカードを挿して、リセット → ATR → コマンドの往復を、ベンダのモジュール無しで
   通した。残りはカードの抜き挿しの合図と、製品のドライバ。
5. **製品への組み込み**: 機種の defconfig・`ird-tuner-emma3sep` の機種グルー（GPIO43・フロントエンド）・
   起動スクリプト。カーネルは YB1 と同じ GPL から作れる見込み（設定の差は §10.1、機種名の
   分岐は §10.7）。**この機種で GPL のカーネルが起動するかは未検証**で、確かめるにはカーネルの
   区画を書く必要がある（失敗したときの戻し方を先に決める）。

### 10.6 ★ 全 PID を載せた流量（地上・BS 3 波・CS 12 波すべて）

§10.4 と同じ構成で、PAT → PMT・CAT から規則どおりに PID を列挙し（生 TS の取り出し §6。
ECM/EMM の PID も載せて運ぶだけ）、全部をフィルタに載せて、試作のドレイナーで 10 秒ずつ測った。
「XDMA」は映像の PID を機器 2（FIFO 2）に集めて XDMA で写し、残りを機器 0（FIFO 0）から CPU で
写す形。「CPU」は両方を CPU で写す形。括弧はタイマのコールバックが使った時間の割合。

| 波 | TSID | 番組 | PID | 流量（毎秒） | XDMA | CPU |
|---|---|---|---|---|---|---|
| 地上 557 MHz | `0x7fe0` | 4 | 50 | 約 11,000 本 | 破れ 19（別の起動では 1）（16%） | **0**（24%） |
| BS-1 | `0x4010` | 3 | 25 | 約 10,700 本 | 0（17%） | 0（23%） |
| BS-3 | `0x4030` | 3 | 26 | 約 14,200 本 | 0（19%） | 0（29%） |
| BS-15 | `0x40f1` | 6 | 43 | 約 13,200 本 | 0（18%） | 0（27%） |
| **CS ND2〜ND24（12 波とも）** | `0x6020` ほか | 4〜8 | 33〜49 | **約 22,400〜24,200 本** | **破れ 6,000〜10,400（2.7〜4.3%）**（21〜23%） | **0**（**43〜45%**） |
| CS ND2・ND6、映像を 2 つの FIFO に分ける | | | | 同上 | 破れ 3・24（0.001〜0.01%）（35%） | 0（43〜45%） |

- **PID の数は 72 本（36 × 2 機器）に収まった**（最大は地上の 50〜52）。BS は 1 つのトランスポンダに
  3 つの TS（測った 3 波とも）、CS は 1 つ。
- **BS は地上と同じ程度の流量**で、試作のドレイナーのままで欠けない。
- **CS は 1 つの TS が毎秒約 2.3 万本（約 35 Mbps）**あり、FIFO 2 の 7 スロットが最短 230 µs で
  一巡する（地上は 301 µs、BS は 345〜518 µs）。1 周期遅れで回収する XDMA の形では数 % 落ちる。
  **CPU で写せば欠けは 0 だが、コールバックだけで CPU の 4 割強を使う**（読み手と配信に残る余力が
  小さい）。映像の PID を 2 つの機器に分けて流量を半分ずつにすると、XDMA ＋ CPU で 35%・欠けは
  10 秒に数本まで減った。∴ **CS では PID の振り分け（重い PID を 2 つの FIFO に分ける）と、
  写し方の選び方が設計の要点**になる。地上と BS は YB1 と同じ設計で足りる。
- ヌル（`0x1fff`）も載せると、地上は毎秒約 2 万本（ヌルが約 9,600 本）、BS-15 は約 15,900 本
  （ヌルは約 2,500 本）。BS-15 は CPU で欠け 0。地上は破れ 0（CPU）だったが、連番の検査は
  ヌルで乱れるので、他の PID が欠けていないかはこの測り方では判定できていない。
- 測定の間、I2C の記録は入れていない。ベンダの Wi‑Fi・Dione のモジュール（`8192du`・`h57fmb`）は
  load されたまま。数字は試作（`tools/yb1-fifodrain`）のもので、製品のドライバのものではない。

### 10.7 製品化に効く機種差（カーネル・GPIO・カード）

- **ベンダのカーネルは機種名を 2 つ多く知っている**: この機種のカーネル（`#16`・2014-11-13）には
  `PIX_AIRTUNER_DT274` と `PIX_AIRTUNER_BR310L` の文字列があり、YB1 のカーネル（`#924`・2016-05-23）と
  GPL（r557）には `W`／`R`／`LITE` しか無い（`strings` の比較）。GPL で機種名を見ているのは
  `drivers/usb/host/ehci-emma.c` の USB の電源の GPIO（`usb_ehci_emma_power_on`）だけ。
  ∴ **GPL から作ったカーネルをこの機種で動かすと、USB の電源の GPIO を誰も設定しない**
  （`BR310L` の分岐が無い）。チューナの経路は USB を使わない（USB に載っているのは Dione と
  Wi‑Fi と背面の口）ので、受信には影響しない見込み【未検証＝この機種で GPL のカーネルを
  起動したことはまだ無い】。
- ベンダのカーネルが動いているときの GPIO: GPIO36 = 1、GPIO37 = 0、GPIO23・24 = 0、GPIO47・48 = 0
  （ベンダアプリ抜きでも同じ）。GPL の `LITE` の分岐（GPIO36 だけ上げる）と同じ形に見える【推定】。
- **zpio の向きは 1 が出力**（GPL の `emma-gpio.c`）。`dir` は `c1fff1fc 0fefdf7f`。
- **フロントエンドの電源 GPIO43 は、カーネルではなくベンダアプリが上げている**（アプリ抜きの
  起動では 0）。製品では機種グルーが gpiolib で上げる。
- **カードの口**: 裏 CPU の FW が持っていて、コマンド `0x11xx` とイベント 16 で扱える
  （[カードの口](sb-tv06-wrip-card-slot.md)。YB1 と同じ FW だが、YB1 には口が無い）。ベンダの
  `/dev/val_scard` との対応も同じ文書の §5。観察は B-CAS ではないカードで行い、カードが運ぶ
  中身には触れていない。

### 10.8 ★ 参照受信機との照合（地上 3 波・BS 2 つの TS・CS 2 波）

§10.6 と同じ構成（全 PID・CPU で写す）で `/tmp` に取り込み、同じ時刻に別ホストの参照受信機
（PX-M1UR・recisdb 1.2.4・`--no-decode --no-strip`）でも取り込んで、両方をそれぞれのホストで
パケットごとの「PID・連番・CRC-32」にし、**ハッシュだけ**を突き合わせた（TS の実体はどちらの
ホストからも出さず、ハッシュにした直後に消した。道具は `tools/host/tsref.py`・`tshashcmp.py`・
`yb1-mem … hashes`）。本機は 12 秒（CS は 7 秒）、参照は 26 秒。

| 波 | TSID | 流れた PID | ビット一致 | 欠け | 化け | ヌル以外の本数（参照の同じ窓／本機） |
|---|---|---|---|---|---|---|
| 地上 ch27（557 MHz） | `0x7fe0` | 35 | 132,461 | **0** | **0** | 133,566 ／ 133,566（1.000） |
| 地上 ch24（539 MHz） | `0x7fe5` | 34 | 128,523 | **0** | **0** | 130,614 ／ 130,614（1.000） |
| 地上 ch21（521 MHz） | `0x7fe4` | 30 | 137,091 | **0** | **0** | 139,258 ／ 139,247（1.000） |
| BS-15 の 1 つめ | `0x40f1` | 28 | 162,978 | **0** | **0** | 163,834 ／ 163,834（1.000） |
| BS-15 の 2 つめ | `0x40f2` | 16 | 81,861 | **0** | **0** | 87,061 ／ 87,061（1.000） |
| CS ND2 | `0x6020` | 41 | 157,799 | 1,944 | **0** | 160,095 ／ 158,147（0.988） |
| CS ND6 | `0x7060` | 45 | 150,558 | 18,337 | **0** | 169,671 ／ 151,565（0.893） |

- 「ビット一致」は、参照の中で位置を決められた PID の本数（PAT・PMT のように同じ内容が繰り返される
  PID は位置を決められないので、本数だけ比べている）。
- ∴ **地上と BS は、参照受信機とビット単位で同じ TS が出ている**（映像はスクランブルされたまま）。
  ベンダのものは裏 CPU の FW と、起こすだけの `csd.ko` だけ。選局は仕様から書いた試作。
- **CS の欠けは「化け」ではなく、丸ごとの取りこぼし**。ドレイナーの破れは 0 で、リングの溢れ
  （ND2 で 5,497 本・ND6 で 24,027 本）が出ている＝読み手（`cat` が tmpfs へ書く）が追いつかなかった。
  読み手が `/dev/null` に捨てるだけなら溢れは 0 だった（§10.6）。CS は「CPU で写す」と「書き出す」を
  合わせると、試作の作りでは CPU が足りない。届いたパケットは全部ビット一致している。
- **規則どおりの列挙では拾えない PID があった**: CS に `0x1c01`・`0x1c02`（7 秒で数本）、地上 ch21 に
  `0x0135`（12 秒で 12 本）。取り込みの直前に行った列挙（PAT・PMT・CAT・固定 PID）には
  出てこなかった PID で、なぜ出てこなかったかは調べていない。全 PID を素通しする受信機との差として残る。
- **固定 PID の一覧に衛星の `0x0025` を足す必要があった**（足す前の CS で 73 本が出ていなかった。
  地上の一覧＝生 TS の取り出し §6.1 には無い PID）。
- CAT は数秒に 1 回しか来ない。列挙の取り込みを 3 秒にしたときは取りこぼして EMM の PID を
  載せ損ねた（9 秒にして解消）。実装は CAT を待ち続けて、来たら足す形にする。
- 参照側の ch24 は 1 回目に「Channel selection failed」で受信できず、やり直して通った（参照側の事情）。

## 11. 実機リグの現状（2026-10-13）

- NAND は 2026-09-26 のまま（telnet-root 入りの LINDBERG コンテナ・rootfs タグ `WRIP1`）。
  今回フラッシュに書いたのは `/app/setting`（mtd8・yaffs2）のファイルだけ。
- 通常起動（ベンダアプリ動作）。IP は DHCP（今回は `192.168.11.19`）。telnet は root・パスワード空、
  シリアルは COM3＝`ttyS1`（getty）。電源は SwitchBot のプラグ。
- `/app/setting` に置いたもの: `openird-pre.sh`（[`openird-pre-wrip.sh`](../../tools/vendor-rootfs/openird-pre-wrip.sh)。
  旧版は `.bak`）・`openird-ip`（`192.168.11.19`）・`i2c-algo-emma.ko`（記録つき）・`wrip-fe.sh`・
  `yb1-mem`・`yb1-ioctl`・`yb1-cmd`・`min-rawts.cmd`・`fifodrain.ko`・`avwatch.ko`。
- ベンダの設定（アッテネータ・アンテナへの給電）は、観察のために `X_SetTunerSetting` で
  切り替えた後、元の値（アッテネータ切・給電入）に戻してある。設定ファイル
  `SettingInfo*.data` は書き換えのたびに名前の末尾（世代）が変わる。
- **フラグはすべて外してある**（`openird-trace`・`openird-i2ctrace`・`openird-avwatch`・
  `openird-noblur*`・`openird-csddebug`）。2026-09-26 から `openird-trace` が残っていて、起動のたびに ioctl の記録が
  `/tmp`（tmpfs 51.9MB）へ毎秒約 3.6KB 溜まっていた（約 4 時間で満杯になる）ので外した。
- 観察のやり方:
  - I2C の記録: `touch /app/setting/openird-i2ctrace` → 電源入れ直し → `/tmp/kmsg.log`。
    時刻は稼働 300 秒で一巡する（jiffies の折り返し）。
  - ベンダアプリ抜き: `touch /app/setting/openird-noblur-once`（＋ `echo 0 > openird-csddebug` で
    csd を静かに）→ 電源入れ直し。IP は `openird-ip`。次の電源投入は通常起動に戻る。
    §10.4 の手順は、`gpiotool set 43` → `wrip-fe.sh init` → `wrip-fe.sh t|s <MHz>` →
    `rmmod val_merge csd` → `yb1-cmd -v min-rawts.cmd` → `mknod /tmp/yb1fifo c 10 <minor>`
    （`/dev` は読み取り専用。minor は `/proc/misc`）→ `insmod fifodrain.ko fifos=5 [use_xdma=0]`。
  - 裏 CPU へのコマンドの記録: `touch /app/setting/openird-avwatch`（中身が `insmod` の引数。
    空なら `period_us=250 secs=90 sweep=0 cmdlog=1`）→ 電源入れ直し → `/proc/misc` の `avwatch` の
    番号で `mknod /tmp/avwatch c 10 <番号>` → `cat`。PC で
    [`tools/host/avwatch2cmds.py`](../../tools/host/avwatch2cmds.py) に通すと、コマンド 1 本ごとの
    要求と応答になる。**50 µs の周期で共有メモリを広く掃くと、起動が数倍遅くなる**（1 回、肝心の
    所が記録の時間から外れた）。
  - ベンダアプリの選局は UPnP の `X_SetTuning(BroadcastWave, Frequency)`（`tb`/`bs`/`cs`・MHz）。
    約 15 秒で待機に戻る。

## 12. GPL のカーネルの起動確認＝1 回だけの RAM 起動（2026-10-16）

**OpenIRD のカーネル（GPL から作るファミリー共通のもの）は、この機種で起動する。** カーネル区画
（mtd2）は書かずに、U-Boot に 1 回だけ RAM から起動させて確かめた。

### 12.1 手順（カーネル区画を書かない）

純正の更新機構の「更新用 env」（[NAND 更新機構](poe-br100-yb1-nand-update.md) §3.2 の形式）を使う。
U-Boot は mtd1 に有効な更新用 env があると、それを自動で実行する（対話の入力の実行ロックとは
関係ない）。

1. カーネルの uImage を mtd9（`/app/flashDev`・yaffs2）に置く（例 `openird/vmlinux.img`）。
2. 更新用 env を作る。`update=` のコマンド列は次の順:
   ```
   delenvu;ymount 3d00000 7d00000;yrdm /flash/openird/vmlinux.img 83000000;
   setenv bootargs console=ttyS1,115200 rootfstype=squashfs root=/dev/mtdblock3 ro ip=none PRODUCT_FWIDS=PIX_AIRTUNER_BR310L U_BOOT_REV=16;
   bootm 83000000;reset
   ```
3. `umount /app/flashDev && /app/bin/nand_flash_write -u /dev/mtd1 <env> && reboot`
   （`nand_flash_write -u` は先頭に 4 バイトの CRC を付けて書く）。

- **`delenvu`（更新用 env を消す）は必ず先頭に置く。** U-Boot はコマンドの失敗でコマンド列を
  打ち切ることがある（`bootm` の失敗で、後ろの `reset` まで届かなかった）。消す前に打ち切られると、
  更新用 env が残って毎回の起動で同じ列が走り、Linux に戻れなくなる。先頭で消しておけば、
  どこで止まっても電源の入れ直しで純正のカーネルが起動する（3 回とも、そのとおりに戻った）。
- **`yrdm` の読み込み先は `0x83000000`（純正の更新と同じ）にする。** `0x82000000` では、置いた
  ファイルも純正が書いたファイルも「File not found」になった（`yls` には出ている）。表示と原因が
  合っていない。
- `0x83000000` から起動すると、展開先（`0x82280000`〜）と重ならないので、NAND から起動するときの
  展開の上限（`0x280000`・NAND 更新機構 §2.1）はこの手順では確かめられない。uImage がその上限に
  収まっていることは別に確かめる。
- 失敗したとき（ファイルが読めない等）は U-Boot で止まる。電源の入れ直しで戻る。

### 12.2 結果

`48606df` のファミリー共通のカーネル（uImage 2,094,382 B）で試した。

| 項目 | 結果 |
|---|---|
| 起動 | 起動した。メモリ 144MB（`09000000 @ 00000000`）、NAND の 11 区画、純正の rootfs（squashfs）を mount、ベンダの起動処理は `lindberg` の起動まで進んだ |
| コンソール | ttyS1（`PIX_AIRTUNER` で始まる機種名で選ぶ・カーネルパッチ 0009）。機種ごとの部品（`airtuner`・`ird-tuner-yb1`）は機種名が一致しないので動かない |
| USB | 機器は 1 つも現れなかった。ただし**純正のカーネルでも**、OHCI の 2 口とも `over-current change` が出て、トランスコーダ（Dione・`06b8:102d`）が起動時に一度つながってすぐ切れるだけで、ふだんは USB の機器が無い。Dione は TS の経路に要らない（§10.4）。純正のカーネルの GPIO は 36 = H・37 / 23 / 24 / 47 / 48 = L（いずれも出力）で、GPL の `LITE` の分岐（36 だけ上げる）と同じ形。GPL には `BR310L` の分岐が無いので、機種の部品で 36 を上げる |
| ネットワーク | 上がらない。カーネルが `ethaddr is not set in boot prom` と言い、`eth0` を上げられない（`Cannot assign requested address`）。この手順では U-Boot の環境が更新用 env に置き換わるので、MAC が渡らないとみられる。NAND から起動したときに渡るかは、この機種ではまだ確かめていない（YB1 では渡る）。GPL のカーネルは、ブートローダの環境に無ければ起動引数の `ethaddr=` を読む（`arch/mips/emma/common/prom.c`）。MAC の元は mtd1 の最後のブロックの先頭（`xx:xx:xx:xx:xx:xx` の 17 文字＋1 バイト）。rootfs がここから読めば、起動の仕方によらず上げられる |
| シリアル | コンソールが ttyS1 になるので、ベンダアプリの出力が流れ、入力はアプリの工場テスト用のシェル（`->`）に取られる。この起動でシェルを使うなら、ベンダアプリ抜き（§11 の `openird-noblur-once`）で起動する |

### 12.3 ★ OpenIRD のこの機種用のイメージを RAM から起動した（2026-10-16）

機種を足した作業ツリー（defconfig `emma3sep_softbank_sb-tv06-wrip`・チューナ無し）から作った
カーネル（2,094,518 B）と rootfs（squashfs 3.1 BE・1,032,192 B）を、どちらも RAM から起動した。
**mtd2 と mtd3 は書いていない。**

- rootfs は initrd として渡す。2 つのファイルを mtd9 に置き、§12.1 の手順のコマンド列を次にする:
  ```
  delenvu;ymount 3d00000 7d00000;yrdm /flash/openird/vmlinux.img 83000000;
  yrdm /flash/openird/rootfs.img 84000000;
  setenv bootargs console=ttyS1,115200 root=/dev/ram0 ro rootfstype=squashfs ramdisk_size=<KB>
    rd_start=0x84000000 rd_size=<バイト数> ip=none PRODUCT_FWIDS=PIX_AIRTUNER_BR310L U_BOOT_REV=16;
  bootm 83000000;reset
  ```
  このカーネルは squashfs の initrd を RAM ディスクに読み（`RAMDISK: squashfs filesystem found`）、
  `root=/dev/ram0` で mount する。`ramdisk_size` はイメージより大きくする（既定は 4096KB）。

| 項目 | 結果 |
|---|---|
| 起動 | OpenIRD の起動処理が最後まで進み、シリアル（ttyS1）に root シェル |
| 書き込める層 | mtd9（yaffs2）の `openird/overlay` を unionfs で重ねた（純正のファイルと同じ区画に同居） |
| ネットワーク | `S40network` が mtd1 の最後のブロックから MAC を読んで設定し、DHCP でいつもの番地が付いた。ssh（dropbear）も上がる |
| LED | 4 本とも登録。起動の最後に電源の緑が点き、横の LED は消灯（所有者が実物で確認）。GPIO 19 だけ L |
| USB の電源 | GPIO36 が H（出力）。USB の機器は現れない（純正のときと同じ・§12.2） |
| 再起動 | `reboot` で `AirTuner restart (self-reset line).` が出て、0.6 秒後にブートローダから起動し直した。更新用 env は消えているので、純正のカーネルで立ち上がった |
| フロントエンドの電源 | GPIO43 は L のまま（チューナのドライバがまだ無い） |

### 12.4 ★ OpenIRD のチューナで地上を受けた（2026-10-16・RAM から起動）

ファミリー共通のチューナ（`ird-tuner-emma3sep` の `emma3sep_tuner.c`）に、この機種のフロントエンド
（`fe_wrip.c`・[フロントエンド](sb-tv06-wrip-frontend.md) §1〜§4・§6.1 の手順）を足したカーネルと、
チューナの起動スクリプト入りの rootfs を、§12.3 の手順で RAM から起動した。

- 起動: カーネルが `ird-tuner-wrip: /dev/irdvideo0 registered`、起動スクリプトが裏 CPU を立ち上げた後
  `ird-tuner-wrip: ready (front end initialised)`。復調器のファームは要らない。
- `ird-rec 27 6`（UHF 27ch・557 MHz）: 1.17 秒でロック、C/N 32.6 dB、6 秒で 66,109 パケット
  （16.5 Mbit/s）、PID 33 本、スクランブルされたまま、同期エラー 0。**連続性エラー 24**。
- `ird-rec 27 20`: 211,180 パケット、**連続性エラー 130**。ドレイナーの数え（`state`）は `torn` 165。
  `overrun`（読み手が追いつかず環が満杯）も数えるが、選局から読み始めるまでの間にも溜まる数で、
  欠けの本体は `torn`（写している間に FIFO のスロットが使い回された）。
- YB1 では同じ 27ch で `torn` 0、19〜20.6 Mbit/s の多重で 15 秒に 25 本前後（[ドレイナー](../poe-br100-yb1-ts-drain.md)
  §4 の 2）。この機種では 16.5 Mbit/s でも毎秒 6 本ほど。TS の届き方（フロントエンドの出し方）の違いと
  みられる【推定】。ドレイナーの頑健化（YB1 と共通の残件）で扱う。以前の試作でも、地上の XDMA で
  欠けが出て、CPU で写すと 0 だった（§10.6）。→ §12.5 で直した。

### 12.5 ★ ドレイナーの欠けを無くした（2026-10-16・RAM から起動）

§12.4 の欠けの原因は**ドレイナーの長い転送**だった（TS の届き方の違いという推定は外れ）。
ドレイナーに実行時に変えられる設定（`/sys/module/emma3sep_tsfifo/parameters/`）と、1 周期に
見つけた本数・転送が覆ったスロット数の数えを足して、地上 ch 13・27・60 で比べた。

- FIFO 2 のスロットはおおむねアドレス順に巡って埋まる。取るものが最後と最初のスロットにまたがると、
  1 回の転送が 7 スロット全部（1,512 バイト・約 100µs＝周期と同じ）になる。ch 13 では転送の約 11% が
  これで、次の周期にまだ続いていて（周期の約 7%）その周期を丸ごと休み、遅れが一巡（約 300µs）を超えた。
- 直し方（[生 TS の取り出し](poe-br100-yb1-ts-fifo.md) §7.4）: ①5 スロット以上の範囲に 2 つ以上
  続く隙間があれば、2 回に分けて運ぶ ②続いている転送は最大 40µs 待つ。周期は 100µs のまま。
- 結果（数字は同 §7.6）: ch 13 の 25 秒で連番の途切れ 196〜318 → 0、CPU（系全体）は 31% のまま。
  ch 27・60・13 を各 60 秒（計 224 万本）で途切れ 0・破れ 0。周期を 50µs に詰めても欠けは消えるが
  CPU は 47%、CPU で写すと 50%。
- 2 つに分けた後の 1 周期の最長は 123µs。
- この形を既定にしたカーネル（設定は触らずに起動）でも ch 13・27 を各 30 秒（計 74 万本）、途切れ 0・
  破れ 0・次の周期まで続いた転送 0。
- YB1 で 16.5 Mbit/s の ch 27 に欠けが出なかったのに、この機種では出た理由は分かっていない
  （FIFO 2 のスロット数はどちらも 7）。同じ直し方が YB1 でも効くかは、YB1 の実機で確かめる。
