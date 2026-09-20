# POE-BR100-YB1 NAND 更新機構（emma3sep・factory 経路の書き込み側）

<!-- Open 層: 機器内の平文シェルスクリプト読解（更新・容量別書き込み・後始末の各スクリプト）
     ＋ GPL ソース（nand_emma_ptt_*.h の区画表、ビルド Makefile の squashfs 上限定数、
     u-boot の main_loop）＋ ブートログ実測。逆アセ由来の内部ロジックは書かない。
     鍵・パスワード、およびその在処は書かない（CONTRIBUTING.md §0）。
     §x.y は機器仕様 spec_bmmpx1301.md（本リポジトリ未移設）の節番号。
     実装（openird-mkimage / factory イメージ生成）はこの仕様のみを根拠に書く（clean-room）。 -->

[更新コンテナ形式](update-v1b-container.md) で運ばれてきたペイロードを、機器が **実際に
NAND へ書く**までの手順。OpenIRD の factory 経路は「どの区画に何バイト書かれるか」を
この仕様に合わせる必要がある。

対象は POE-BR100-YB1 (BMMPX1301)。同一コードベースの emma3sep 兄弟機も同じ機構で、
**NAND 容量（1Gb=128MB / 512Mb=64MB）でオフセット体系が分岐する**（§5）。

## 1. NAND 区画表（1Gb = 128MB 機・GPL `nand_emma_ptt_*.h`）

区画は `MTDPART_OFS_APPEND` の連結＝offset は size の累積。下表の offset は GPL の
定義を展開したもので、**実機 NAND ダンプの使用ブロック分布と一致を確認済み**（下記）。

| mtd | 内容 | offset | size | factory で書かれるか | OpenIRD の扱い |
|---|---|---|---|---|---|
| 0 | u-boot | `0x0000000` | `0x300000` (3.0MB) | 枠はあるが実配布では書かれない（§4） | **温存**（絶対に書かない） |
| 1 | u-boot env（更新用）・MAC | `0x0300000` | `0x300000` (3.0MB) | **env 先頭は機構上必ず書かれる**（§3） | 条件付き（下記） |
| 2 | Kernel（uImage/gzip） | `0x0600000` | `0x480000` (4.5MB) | ○ | **書き換え対象** |
| 3 | FileSystem（squashfs = rootfs） | `0x0A80000` | `0x900000` (9.0MB) | ○ | **書き換え対象** |
| 4 | App_binary（squashfs・純正アプリ） | `0x1380000` | `0x1B80000` (27.5MB) | ○ | 温存（段階 B のベンダ受信部品） |
| 5 | EMMA_FW（squashfs・SoC FW） | `0x2F00000` | `0x400000` (4.0MB) | ○ | 温存 |
| 6 | 初期設定 | `0x3300000` | `0x300000` (3.0MB) | ○ | 温存 |
| 7 | Flight Record Log（YAFFS2） | `0x3600000` | `0x300000` (3.0MB) | 後始末で消去され得る（§4） | 温存 |
| 8 | App 設定（YAFFS2） | `0x3900000` | `0x400000` (4.0MB) | × | 温存 |
| 9 | MassStorage 録画（YAFFS2） | `0x3D00000` | `0x4000000` (64.0MB) | × ＝**更新ファイルの置き場**（§3） | 温存 |
| 10 | BBT | `0x7D00000` | `0x300000` (3.0MB) | × | 温存 |

- 各区画は不良ブロック用に 2.5MB の余裕を持たされている＝ **size と §2 の書き込み固定長は
  一致しない**（例: mtd3 は size 9.0MB に対し書き込み長 6.5MB）。
- **ダンプとの突き合わせ**（2026-09-20）: 生 NAND ダンプは 2048B ページ＋64B OOB の形式
  （65536 ページ＝138,412,032B）。OOB を剥がした論理像で、uImage マジックが `0x600000` に、
  squashfs マジックが `0xA80000` / `0x1380000` / `0x2F00000` に、いずれも上表どおり現れる。
  mtd1 の最終ブロック `0x5E0000` だけが使用中＝「MAC は mtd1 の最終ブロック」（§3.1）と整合。
- **kernel の uImage 契約**（同ダンプ実測）: Legacy uImage / os=linux / arch=mips / type=kernel /
  comp=gzip / name `emma3se` / **load `0x82280000` / entry `0x822841b0`**（load ≠ entry）。
- **rootfs の squashfs は 3.1 ビッグエンディアン**（マジック `sqsh`）。ベンダカーネルの
  squashfs は in-tree（4.0・2.6.29〜）ではなく patch 版 3.1
  （GPL `fs/squashfs/{inode.c,squashfs2_0.c}`・`include/linux/squashfs_fs.h` の
  `SQUASHFS_MAJOR 3` / `SQUASHFS_MINOR 1`）。mtd4/mtd5 の像も同じ。
  → **Buildroot の mksquashfs は 4.x ＝ 3.1BE を作れない**。OpenIRD の rootfs をどちらの
  形式で作るかは §6 の未決事項。

**mtd1 の扱い（重要）**: この更新機構は「u-boot の env を書き換えて次回起動時に u-boot 自身に
書かせる」設計のため、**mtd1 先頭（env 領域）への書き込みは回避できない**。一方
**MAC は mtd1 の最終ブロック**にあり（§3.1）、env 書き込みはそこに届かない＝個体固有の
MAC は保たれる。∴ OpenIRD でも「mtd1 = env 領域のみ書く・最終ブロックは触らない」を守る。

## 2. イメージと書き込み先・サイズ上限（1Gb 機）

| ペイロード内の名前 | 書き込み先 | 書き込みオフセット | **書き込み長（＝サイズ上限）** | 消去範囲 |
|---|---|---|---|---|
| `vmlinux.img` | mtd2 Kernel | `0x600000` | `0x300000` (3.0MB) | `0x480000` (4.5MB) |
| `rootfs.img` | mtd3 FileSystem | `0xa80000` | `0x680000` (6,815,744B) | `0x900000` (9.0MB) |
| `app_bin.img` | mtd4 App_binary | `0x1380000` | `0x1900000` (26,214,400B) | `0x1b80000` (27.5MB) |
| `emma_fw.img` | mtd5 EMMA_FW | `0x2f00000` | `0x180000` (1,572,864B) | `0x400000` (4.0MB) |
| `init_setting.img` | mtd6 初期設定 | `0x3300000` | `0x80000` (512KB) | `0x300000` (3.0MB) |

- **書き込み長は固定値**（イメージの実サイズではない）。∴ **上限を超えたイメージは超過分が
  黙って切り捨てられ、区画後半が破損する**。pack 側で必ず検証すること。
- rootfs / app_bin / emma_fw の上限は **GPL ビルド Makefile の `*_SQUASHFS_MAX` 定数と一致**
  （`ROOTFS_SQUASHFS_MAX` = `0x680000` 等）＝上限はベンダのビルド時制約と同一。
- 消去範囲は隙間なく連なる（`0x600000`+`0x480000`=`0xa80000` … `0x3300000`+`0x300000`=`0x3600000`）。
  区画表（§1）と整合＝オフセット体系の裏取りになる。
- 5 点とも任意。同梱しなかった区画は消去も書き込みもされない＝**差分アップデートが成立**。

## 3. 実書き込みの仕組み — u-boot env 経由（インタラクティブロックを経由しない）

**Linux ユーザランドは NAND を直接書かない。** 代わりに u-boot のコマンド列を **env として
mtd1 に書き込み**、次回起動時に u-boot 自身に実行させる。

```
① Linux 側: 純正アプリがペイロードを MassStorage(mtd9, YAFFS2) 上の /flash/tmp_update へ展開
② Linux 側: そのディレクトリを cwd にして write.sh を起動（§3.2）
             → 容量を判定して write1Gb.sh / write512Mb.sh が u-boot のコマンド列を組み立て、
               env として mtd1 先頭へ書く
③ reset
④ u-boot: bootcmd=run update → update を自動実行
     ・MassStorage の YAFFS 区画を ymount
     ・区画ごとに（同梱されているものだけ）:
         RAM ステージング領域を 0xff で**書き込み固定長ぶん**フィル
         → YAFFS から .img を RAM へ読み込み
         → nand erase <消去範囲>  →  nand write <固定長>       （§2 の表）
     ・更新用 env を削除（delenvu）＝ 無限ループ防止
     ・mtd1 先頭に完了マーカー "FWUP\0" を書く
     ・reset
⑤ 次回起動: Linux 側の起動スクリプトがマーカーを見て後始末（§4）へ合流
```

### 3.1.1 Linux 側の受け取り手順（ベンダの平文スクリプト `/app/update.sh` より）

`blur`（ベンダアプリ）が外側ヘッダを検査して AES を解き、**中間 ZIP をそのまま
`/app/flashDev/updatefile.zip` に書く**。以降は平文シェルの担当で、次の順に進む。

```
mount /app/flashDev                       # mtd9(YAFFS2)。fstab では noauto
cd /tmp                                   # tmpfs
unzip -o -P <パスワード> /app/flashDev/updatefile.zip   # → blur.zip と csum
  失敗したら中止
sha1sum -c csum                           # blur.zip の SHA-1 を検証
  失敗したら中止
rm -rf /app/flashDev/tmp_update ; mkdir   # 作業ディレクトリを作り直す
cd /app/flashDev/tmp_update
unzip -o /tmp/blur.zip                    # 区画イメージとスクリプトを展開
  失敗したら中止
./write.sh                                # ← **実行**する。env を mtd1 へ書く
  失敗したら中止
touch start_update.txt                    # 「u-boot で更新中」の印
umount/mount /app/flashDev ; reboot
```

- **`./write.sh` を実行する**＝ペイロード ZIP が実行ビットを保っていなければ
  ならない（[update-v1b-container.md](update-v1b-container.md) §5）。
- 機器の `/usr/bin/unzip` は **busybox ではなく Info-ZIP の実体バイナリ**。
  `sha1sum` は busybox 1.17.4。
- `mac.dat` が置かれていれば `nand_flash_write -l` で mtd1 に MAC を書く枠もあるが、
  配布コンテナには無い。**OpenIRD も置かない**。
- どの段で失敗しても後片付け（`rm -rf tmp_update` と `updatefile.zip` の削除）は
  走らない＝**失敗すると作業中のファイルが mtd9 に残る**。切り分けの手がかりになる。
- 更新後の再起動では `start_update.txt` があるため `delete.sh` の実行に分岐する（§4）。
- 関連する fstab: `/`=mtd3(squashfs,ro) / `/app/bin`=mtd4(squashfs,ro) /
  `/app/setting`=mtd8(yaffs2,rw) / `/app/flashDev`=mtd9(yaffs2,rw,noauto) / `/tmp`=tmpfs。

> 中間 ZIP のパスワードはこのスクリプトに平文で置かれているが、**値は本書にも
> リポジトリにも書かない**（CONTRIBUTING.md §0）。所有者が自機から回収する。

### 3.2 ペイロード内スクリプトの契約（OpenIRD が満たすべきもの）

`write.sh` は OpenIRD 側で仕様どおり生成する（純正から流用しない・
[update-v1b-container.md](update-v1b-container.md) §6-2）。そのために必要な契約:

| 項目 | 内容 |
|---|---|
| 起点 | `write.sh`。cwd は展開先 `/flash/tmp_update`。`.img` の有無は **cwd の相対パス**で見る |
| 呼ばれ方 | 終端が `return $?`＝**source される**（独立実行ではない）。終了ステータスを返す |
| 容量判定 | `/sys/block/mtdblock0..10/size` の総和（単位 512B）を求め、`>= 262144`（=128MB）なら 1Gb 版、それ未満なら 512Mb 版へ |
| env の書き込み | `/app/bin/nand_flash_write -u /dev/mtd1 <envfile>`。**機器上に既にあるベンダのツール**を呼ぶ |
| env の形式 | `key=value` を `\0` 区切りで連結し、末尾は `\0\0` で終端。先頭から `bootdelay` / `baudrate` / `bootcmd=run update` / `update=<コマンド列>` |
| u-boot コマンド列 | `;` 区切り。`ymount <start> <end>` → 区画ごとに `mw.b <staging> ff <write_len>` / `yrdm /flash/tmp_update/<name> <staging>` / `nand erase <off> <erase_len>` / `nand write <staging> <off> <write_len>` → `delenvu` → マーカー書き込み → `reset` |
| RAM ステージング | **1Gb 機 `0x83000000` / 512Mb 機 `0x86000000`**（実測。機種で異なる） |
| ymount 範囲 | 1Gb: `3d00000 7d00000`（mtd9 と完全一致）。512Mb: `2000000 3e40000`（区画表の終端 `0x3EC0000` より 512KB 手前。ベンダ側の不一致だが実配布どおり） |
| 完了マーカー | `0x800` を `0xffffffff` で埋め、先頭に `46 57 55 50 00`（"FWUP\0"）を置いて `nand write <staging> <mtd1 先頭> 800` |

- **`/app/bin/nand_flash_write` を呼んでよい。** factory 実行時点の機器はまだベンダ状態で、
  このツールは機器上に存在する。OpenIRD の配布物にベンダバイナリを含めるわけではない
  （SB-TV02 で `dione_server -w` を呼ぶのと同じ立て付け）。
- ブートローダを書く枠（`u-boot.img` → `/app/bin/nand_flash_write /dev/mtd0`）はスクリプトの
  先頭にあるが、実配布コンテナに `u-boot.img` が入らないので発火しない（§4）。**OpenIRD は
  この枠を生成しない。**

- **ロック回避が不要な理由**: u-boot の実行ロックは*インタラクティブなシリアル入力*だけを
  ゲートし、`bootcmd` の自動実行は妨げない（GPL の `main_loop` 実装・§3.5）。∴ この経路は
  **未解決の u-boot パスワード（§3.5/3.6）を一切必要としない**。純正アップデータ自身が
  「ロック済み u-boot でも動く」設計を採っている。
- **RAM を 0xff でフィルしてから読み込む**ため、実サイズが上限に満たないイメージでも、
  区画の残りは消去状態（0xff）で埋まる。
- 完了マーカーの書き込み先は NAND `0x300000` 長 `0x800`＝mtd1 先頭。**mtd1 が NAND
  `0x300000` から始まることは GPL の区画表で確定**（§1）。
- `.img` を `/flash/tmp_update` へ展開する主体は純正アプリ側の更新処理（確度: 高。展開先の
  パスはスクリプトが `yrdm` に渡す絶対パスから確定）。**OpenIRD 側で再現する必要は無い**
  ＝コンテナが受理されれば純正アプリが展開してくれる。どのコンポーネントが行うかだけが未追跡。

## 4. 後始末（再起動後の Linux 側）

- マーカーを見て「u-boot で書き込み済みの再起動後」と判定した場合のみ走る。
- ペイロード内の `delete.sh` が担う（確度: 確定・実測）。同梱の一覧に従ってベンダアプリの
  設定ファイルとそのミラー（`.1` / `.2`）を削除し、**Flight Record Log（mtd7）を
  `/app/bin/nand_flash_write -e /dev/mtd7` で消去して YAFFS2 で mount し直す**。
- **OpenIRD の factory は `delete.sh` を同梱しない。** 中身はベンダアプリの設定の始末で、
  OpenIRD には対応物が無い。同梱しなければ走らない。
- **ブートローダ（mtd0）を書く枠はスクリプトに存在する**が、対応ファイルが実配布
  コンテナに同梱されないため発火しない＝通常経路で mtd0 は書き換わらない。**OpenIRD は
  この枠を使わない**（復旧性＝ブートローダ健在を最後の安全網として残す）。

## 5. 512Mb（64MB）機の分岐

- 起動スクリプトが `/sys/block/mtdblockN/size` の合計から NAND 総容量を求め、
  **128MB 相当以上なら 1Gb 版・未満なら 512Mb 版**の手順を選ぶ。
- **区画表は GPL `nand_emma_ptt_512Mb.h` に全数あり**（確度: 確定）:

  | mtd | 内容 | offset | size |
  |---|---|---|---|
  | 0 | u-boot | `0x0000000` | `0x200000` (2.0MB) |
  | 1 | u-boot env・MAC | `0x0200000` | `0x180000` (1.5MB) |
  | 2 | Kernel | `0x0380000` | `0x340000` (3.25MB) |
  | 3 | FileSystem | `0x06C0000` | `0x7C0000` (7.75MB) |
  | 4 | App_binary | `0x0E80000` | `0x640000` (6.25MB) |
  | 5 | EMMA_FW | `0x14C0000` | `0x2C0000` (2.75MB) |
  | 6 | 初期設定 | `0x1780000` | `0x180000` (1.5MB) |
  | 7 | Flight Record Log | `0x1900000` | `0x300000` (3.0MB) |
  | 8 | App 設定 | `0x1C00000` | `0x400000` (4.0MB) |
  | 9 | MassStorage | `0x2000000` | `0x1EC0000` (30.25MB) |
  | 10 | BBT | `0x3EC0000` | `0x1C0000` (1.75MB) |

  Kernel 起点 `0x380000` は従来の部分観察と一致。
- **書き込み固定長も確定**（`write512Mb.sh` の実測。RAM ステージングは `0x86000000`）:

  | ペイロード内の名前 | 書き込み先 | 書き込みオフセット | **書き込み長** | 消去範囲 |
  |---|---|---|---|---|
  | `vmlinux.img` | mtd2 Kernel | `0x380000` | `0x200000` (2.0MB) | `0x340000` (3.25MB) |
  | `rootfs.img` | mtd3 FileSystem | `0x6C0000` | `0x680000` (6.5MB) | `0x7C0000` (7.75MB) |
  | `app_bin.img` | mtd4 App_binary | `0xE80000` | `0x500000` (5.0MB) | `0x640000` (6.25MB) |
  | `emma_fw.img` | mtd5 EMMA_FW | `0x14C0000` | `0x180000` (1.5MB) | `0x2C0000` (2.75MB) |
  | `init_setting.img` | mtd6 初期設定 | `0x1780000` | `0x40000` (256KB) | `0x180000` (1.5MB) |

  完了マーカーの書き込み先は mtd1 先頭 `0x200000`。**rootfs の上限は 1Gb 機と同じ 6.5MB** で、
  kernel だけが 3.0MB → 2.0MB に縮む。
- ∴ **factory イメージは容量世代を跨いで共用できない**。OpenIRD の defconfig も分ける。

## 6. OpenIRD への落とし込み

- **書き換えるのは mtd2(kernel) と mtd3(rootfs) のみ**。mtd4/5/6 は同梱しない
  ＝ App_binary・EMMA_FW・初期設定を温存する（段階 B のベンダ受信部品と個体設定を失わない）。
- **mtd0 は書かない。mtd1 は env 領域のみ**（MAC のある最終ブロックには触れない）。
- **サイズ上限は kernel 3.0MB / rootfs 6.5MB**（§2）。段階 A の Buildroot rootfs はこの枠に
  収める必要がある。超過時は「切り捨てて破損」なので、生成側で hard error にする。
- **rootfs の形式は未決**: ベンダは squashfs 3.1BE（§1）だが Buildroot の mksquashfs は 4.x で
  3.1BE を作れない。(a) mainline squashfs 4.0 をベンダカーネル 2.6.27 に backport して 4.0LE を
  使う（2.6.31 に overlayfs を移植したのと同じ手口）か、(b) 3.x 系のツールを別途用意するか、
  (c) 別の rootfs 形式にするか。**factory は kernel と rootfs を両方書く＝この整合は OpenIRD
  側で閉じられる**。
- クロス機種コンテナは全モードで拒否され版数も変わらない（§6.4 実証）＝**誤投入は
  ブリックしない**。危険なのは「族タグ等の整合が取れた自作コンテナの中身が不正」な場合。
- 復旧の最終手段は UART / NAND 直書き（ピンヘッダ増設を伴う改造）。通常導入手段にはしない。

## 7. 未解明（推測で埋めない）

| # | 項目 | 状態 | 決着のさせ方 |
|---|---|---|---|
| ~~U1~~ | ~~512Mb 機のオフセット／上限表~~ | **決着** | 区画表は GPL、書き込み固定長は `write512Mb.sh` の実測。§5 に全数あり |
| U2 | `/flash/tmp_update` への展開を行う純正コンポーネントの同定 | 未追跡（展開先パスは確定・§3） | 実フラッシュ中の機器側の観察。**pack のブロッカーではない**（展開はベンダ側が行う） |
| ~~U3~~ | ~~mtd1 の開始オフセット~~ | **決着** | `0x300000`（GPL `nand_emma_ptt_1Gb.h`）。NAND ダンプの使用ブロック分布とも整合＝§1 に反映済み |
| U4 | 更新完了フラグの意味（USB 経由更新との関係） | 推定 | 同上・ダンプ後に判断 |

## 8. シリアルコンソールと起動引数（観察の足場）

u-boot の既定環境には起動コマンドが 4 本ある（NAND ダンプの mtd0 から観察）。

| 名前 | bootargs | 用途 |
|---|---|---|
| `rom` | `rootfstype=squashfs root=/dev/mtdblock3 ro ip=none PRODUCT_FWIDS=…` | **製品用。`console=` が無い** |
| `rom_dev` | `console=ttyS1,115200` ＋ 上と同じ | 開発用 |
| `nfs` / `nfs_dev` | `root=/dev/nfs rw ip=:::::eth0:dhcp` | NFS 起動 |

- **製品出荷状態では `console=` が無く、Linux の出力は一切シリアルに出ない。**
  u-boot の `Starting kernel ...` の後が無音なのはこのため（カーネルが壊れている
  わけではない）。
- **実シリアルポートは Linux から見て `ttyS1`**（`/dev/ttyS1` = char 4,65）。
  ベンダ rootfs の `/etc/inittab` にあるコメント行は `ttyS0` だが、これは行末に
  `GENERIC_SERIAL` とある配布元の汎用テンプレートで、この機種の実ポートではない。
  **コメントを外すだけでは無反応**になる。
- `getty` はカーネルコンソールとは独立に `/dev/ttyS1` を開くので、`console=` が
  無いままでもログインできる。`/etc/securetty` には `ttyS0`〜`ttyS3` が載っており
  root ログインは許可される。`/etc/shadow` の root はパスワード空。
- カーネルの起動メッセージまで見たい場合は `rom_dev` 相当の bootargs が要るが、
  既定環境は u-boot に組み込まれているため、更新機構の env（`delenvu` で消される）
  では恒久的に変えられない。**まず getty で足場を作るのが先。**

## 9. 起動シーケンスの落とし穴 — `/etc/inittab` は使われない

**ベンダ rootfs の `/etc/inittab` を編集しても効かない。** 実機で 2 度踏んだ。

`/etc/inittab` は `::sysinit:/etc/init.d/rcS` を持ち、その `rcS` の末尾が

```sh
setsid cttyhack /app/bin/setup.sh     # メインアプリを起動
while : ; do setsid cttyhack /bin/sh ; done   # ← 無限ループ。戻らない
```

busybox init（1.17.4）は sysinit アクションを `waitfor()` で待つ
（`init/init.c` の `run_actions()`）。∴ **rcS は永久に戻らず、inittab の
`respawn` 行は一つも実行されない**。ベンダ自身が書いた `tty1`/`tty2` の getty も
`syslogd`・`klogd` も、実際には動いていない。

- **介入点は `rcS` の、この無限ループの直前。** ここならメインアプリの起動後、
  かつ init が止まる前に入る。
- ベンダはここで既に「UART 経由でシェルを出す」意図のループを書いている
  （コメントにそう書いてある）が、`cttyhack` が掴むのは制御端末＝`/dev/console`。
  製品用 bootargs には `console=` が無い（§8）ため実体が無く、シェルは出ない。
- ∴ シリアルに入りたければ、`rcS` から **ポートを明示して** getty を立てるか、
  ネットワーク側（`telnetd`）を使う。

> ネットワークについて: `/etc/network/interfaces` には `lo` しか無く、eth0 は
> ベンダアプリ側（`/app/bin` の udhcpc スクリプト群）が設定する。∴ `telnetd` を
> 起動しても、アドレスが付くのはアプリが立ち上がってから。
