# SB-TV06-WRIP (POE-BR310L-SB1) bring-up と factory 経路

<!-- openird-spec
Status: verified
As-of:  実機 SB-TV06-WRIP (2026-09-26)
Impl:   host/openird-mkimage/boards/sb-tv06-wrip.json, host/openird-mkimage/openird_mkimage.py, tools/vendor-rootfs/mkshell.sh
Tests:  tests/mkimage/
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
- **フロントエンド専用 I2C**: **`/dev/ei2c`（major 222・`ei2c_ofdm_drv`）**。demod は標準 i2c-dev
  ではなくこの OFDM 用 embedded I2C 経由でアクセスされる。`demod_tc90522_90532_drv` が使う。
- **復調器 TC90522/90532 は 2 つのスレーブアドレスを持つ**: `s_slave_addr_ofdm`（ISDB-T 地上波）
  と `s_slave_addr_psk`（ISDB-S 衛星）。T/S それぞれにレジスタ表（`demod_90522_T/S`・
  `demod_90532_T/S`）。**チューナ（MxL601RF/135RF）は demod の I2C ゲートの奥**にいる
  （`Tuner_set_slave_addr` が demod 側でチューナのスレーブアドレスを設定）。
- ∴ **クリーン選局の足場**: mainline `tc90522.c`（T=地上/S=衛星の 2 コア・ゲート付き I2C リピータ）
  ＋ MxL601（`mxl301rf`/`mxl5007t` 系）＋ 実バス観察。アドレス・レジスタ意味論は datasheet と
  mainline から取り、ベンダ .ko の内部レジスタ表は転写しない（clean-room）。

### 5.3 ★ フロントエンド I2C は GPIO ビットバング（GPL emma i2c ではない）

実機（noblur・モジュール load 済み）で確定した重要事実。**YB1 と決定的に違う**:

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
- **`i2c_emma` は "used by 0"** ＝ `ei2c_ofdm` は **GPL の emma i2c バスを使っていない**。
  だから `yb1-i2c`（`/dev/i2c-1`）で走査しても demod は出ない（bus 1 は空）。
- **`/proc/iomem` に ei2c 専用のレジスタ枠が無い**が、**`zpio.0`（GPIO）が `0x10110d00`** に居る。
  ∴ **`ei2c_ofdm` は GPIO を bitbang して TC90532 と話している**（`/dev/ei2c` major 222）。
  YB1（復調器が標準 emma i2c 上）とはトランスポートが根本的に違う。
- ∴ **段階 C の frontend トランスポートは「GPIO bitbang I2C の再実装」から**: ①ei2c が使う
  SCL/SDA の GPIO ピンを観察で同定（demod アクセス中の GPIO 変化を追う。zpio は GPL）→
  ②クリーンな GPIO bitbang I2C master を書く（`wrip-ei2c`）→ ③その上に mainline `tc90522.c`
  意味論で TC90532、demod ゲート越しに MxL601。**プロプラ .ko の中身は読まない・転写しない。**

## 6. 未確定・次アクション

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

1. **I2C 地形の同定＝完了**（§5.3）: frontend は GPL emma i2c ではなく **プロプラ ei2c_ofdm の
   GPIO bitbang**（zpio `0x10110d00`）と判明。`/dev/i2c-1` は空、bus 0 は走査しない。
2. **SCL/SDA ピン＝同定済み（2026-09-26・観察）**: ei2c は **zpio_data[0]（GPIO0-31）** を bitbang
   していると実機で確認。同定手順（再現可）: UPnP `X_SetTuning`(tb) で選局→lindberg が復調器を
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
- ∴ **段階 C frontend のクリーンな組み立て方**:
  1. demod の **SCL/SDA GPIO 番号だけ**を実機観察で同定（唯一 GPL に無い値。§7.2）。
  2. GPL `i2c-gpio` を、その 2 本の GPIO でインスタンス化（zpio は GPL gpiolib）→ 標準 `/dev/i2c-N`。
  3. その上に**後代 mainline の `tc90522.c`**（2.6.27 には未収録＝別途 kernel.org から参照）＋
     `mxl301rf`/`mxl5007t` 系の意味論で TC90532/MxL601 を選局（`Origin: gpl-derived` ＋ `Spec-Ref`）。
  ＝ **観察に依存するのは「2 本のピン番号」だけ**に縮小できた。

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
