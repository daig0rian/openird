# MBIN ファームウェア容器形式（MaxLinear 復調器）

<!-- openird-spec
Status: implemented
As-of:  POE-BR100-YB1 / ベンダ FW 2.1.17 r3 の `/app/bin/blur` (2026-09-22)
Impl:   scripts/extract-mbin.py
Tests:  tests/mkimage/
Note:   §1-§6 は実物の blur で確認済み。§7（I2C でどう流すか）は未観察＝段階 C の関門 A1
-->

<!-- Open 層: (1) メインライン Linux（GPL-2.0・公開）の MBIN 構造定義、
     (2) ベンダバイナリ中の該当バイト列の静的観察（走査・境界・チェックサム）、
     (3) ベンダバイナリ中の平文文字列。
     逆アセンブルは行っていない。ファーム本体・ベンダバイナリは置かない。 -->

MaxLinear の復調器・チューナが使う、チップ内 RAM へ転送するファームウェアの容器。
`MBIN` は MaxLinear 自身の呼び名で、**構造の定義はメインライン Linux に GPL で
入っている**（`drivers/media/dvb-frontends/mxl5xx_defs.h` の
`MBIN_FILE_HEADER_T` / `MBIN_SEGMENT_HEADER_T`、ダウンロード手順は
`mxl5xx.c` の `do_firmware_download()` / `check_fw()`）。本書はその公開定義を
**POE-BR100-YB1 の実物に突き合わせて確認した結果**と、**この機種固有の所在**を記録する。

本書は容器の話だけを扱う。**ファーム本体は所有者の機器上のものであって、OpenIRD の
配布物には 1 バイトも含めない**（CLAUDE.md §1-5・[§8](#8-配布方針)）。

## 0. なぜ要るか

POE-BR100-YB1 の復調器 MXL683 は、**チップ内 RAM へファームを転送しないと動かない**。
ベンダアプリ `blur` を外して起動すると誰もこれをやらないため、デマルチプレクサの
設定が全部通っても TS 入力そのものが無い
（[poe-br100-yb1-reception-stack.md](poe-br100-yb1-reception-stack.md) §24.5 / §25.3）。
∴ **段階 C の前提**。

## 1. 所在（POE-BR100-YB1）

**区画上のファイルではない。`blur` の中にある。**

| | |
|---|---|
| ファイル | `/app/bin/blur`（mtd4 `App_binary` の squashfs 内・10,247,076 バイト） |
| sha256 | `eca5748c34a962705cc9f27a0da288b5257330dacce08291d3c4dd2469854df0`（FW 2.1.17 r3） |
| ELF | ELF32 MSB EXEC / MIPS / o32 / mips32r2 |
| 収まっている節 | `.rodata`（vaddr `0x00b403d0`・file off `0x7403d0`・長さ `0xbe470`） |
| ファイル内オフセット | `0x7e1950`（**この版での値。固定値として扱わないこと**→§6） |
| 容器の全長 | 13,224 バイト（16 + 13,208） |

直前の `.rodata` には MxLWare68x の API 名が並ぶ（`MxLWare68x_API_CfgDevFirmwareDownload`、
`MxLWare68x_API_CfgDevXtal`、`MxL68x_Ctrl_Initialize`、`downloadFirmware` ほか）。
容器を検出した位置がこの文字列群の直後であることは、独立した傍証になる。

`blur` にはこの形式を扱う旨の平文文字列もある:

```
Ctrl_DownloadFirmwareInMbin: Invalid file header ID (%c)
Ctrl_DownloadFirmwareInMbin: Invalid segment header ID (%c)
Err : MxLWare68x_API_CfgDevFirmwareDownload %d
Download firmware success.
```

**どの区画にも独立したファームファイルは無い**（走査済み。mtd4 にある `.bin` は
`mb86h57_h58_aac_{trc,idle}.bin` ＝トランスコーダ MB86H58 のもの、
`spirom_writer_usb_pixela_102Dh.bin` ＝SPI-ROM ライタ）。

## 2. ファイルヘッダ（16 バイト・全フィールド big-endian）

```
off  size  field            POE-BR100-YB1 での実測値
---  ----  ---------------  ------------------------
0x00    1  id               'M'  (0x4d)
0x01    1  fmt_version      '1'  (0x31)
0x02    1  header_len       16   (0x10)
0x03    1  num_segments     3
0x04    4  entry_address    0x40010000
0x08    3  image_size24     13208 (0x003398)
0x0b    1  image_checksum   0xd5
0x0c    4  reserved         00 00 00 00
```

```
00000000: 4d31 1003 4001 0000 0033 98d5 0000 0000  M1..@....3......
```

- `image_size24` は**ヘッダを含まない本体の長さ**。容器全長は `16 + image_size24`。
- `entry_address` は**どのセグメントの範囲にも入らない**（§3 の実測値参照）。
  メインラインの `do_firmware_download()` もこのフィールドを読まない。
  **用途は未解明**。転送の再現に要るかどうかは §7 で確かめる。
- `reserved` は全ゼロ。

## 3. セグメント（ヘッダ 8 バイト・big-endian ＋ 4 バイト整列のデータ）

本体は `num_segments` 個のセグメントが**隙間なく並ぶ**。

```
off  size  field
---  ----  ----------
0x00    1  id       'S' (0x53)
0x01    3  len24    データ長（パディングを含まない）
0x04    4  address  転送先のチップ内アドレス
0x08  len   data    ★ 次のセグメントは 4 バイト境界まで進めた位置から
```

POE-BR100-YB1 の実測:

| # | address | len | 4B 整列後 | data の先頭 16 バイト |
|---|---|---|---|---|
| 0 | `0x3ffe0000` | 4784 | 4784 | `70 00 00 10 3f fe 10 64 70 00 01 68 70 00 01 6a` |
| 1 | `0x3ffe2900` | 601 | **604** | `70 00 00 00 3f fe 10 64 3f fe 19 b0 3f fe 19 90` |
| 2 | `0x40000010` | 7796 | 7796 | `04 14 00 00 00 00 00 00 00 00 00 00 00 00 00 00` |

**4 バイト整列は実測で裏が取れている。** セグメントヘッダ 3 個（24 バイト）と
データ長の合計は

```
24 + 4784 + 604 + 7796 = 13208   ← image_size24 と一致
24 + 4784 + 601 + 7796 = 13205   ← パディング無しだと合わない
```

∴ `len24` を `((len + 3) / 4) * 4` に丸めて次へ進む規則が、この実物で成立している。

転送先は 2 つの領域に分かれる（`0x3ffe0000` 台と `0x40000000` 台）。
`entry_address` の `0x40010000` はどちらにも属さない（§2）。

## 4. チェックサム

`image_checksum` は、**ヘッダ直後から `image_size24` バイト**の
**8 ビット総和**（桁あふれは捨てる）。

```
cs = 0
for b in body[0:image_size24]: cs = (cs + b) & 0xff
assert cs == image_checksum
```

POE-BR100-YB1 の実物で `0xd5` が一致した。∴ **容器の切り出しはこれで自己検証できる**
（オフセットを決め打ちしなくてよい理由・§6）。

## 5. 走査による同定

`blur` 全体（10,247,076 バイト）で `'M'`,`'1'` の 2 バイトは **13 か所**現れるが、
次の全部を満たすのは **1 か所だけ**だった:

1. `header_len == 16`
2. `1 <= num_segments <= 32`
3. `16 + image_size24` がファイル内に収まる
4. §4 のチェックサムが一致する
5. 全セグメントの `id == 'S'` で、§3 の整列規則で歩いた消費量が `image_size24` に一致する

∴ **このファームウェアの容器は `blur` 内に 1 個だけ**。分割・圧縮・難読化はされて
いない（`.rodata` に生で置かれている）。

取り出した容器（16 バイトヘッダ＋本体・計 13,224 バイト）の sha256 は
`db73329d63794258de351862690c077448bb8df2bb2ea03baca6dc445fb489c6`。
**この値は容器の同一性の目印であって、中身を置くものではない**（§8）。

## 6. 取り出し手順

**オフセットを決め打ちしない。走査して自己検証する。** ベンダ FW の版が変われば
`blur` の中の位置は動くため。

```
scripts/extract-mbin.py <blur> [-o out.mbin]
```

- 依存なしの Python 3。`--list` で見つけた容器の一覧（オフセット・セグメント・sha256）を出す。
- 所有者は自分の機器の `/app/bin/blur` に対して実行する。実機上の `/app/bin/blur`
  は squashfs がマウント済みなので**普通のファイルとして読める**。ホストで行うなら
  factory コンテナから取り出した `app_bin.img`（squashfs 3.1 BE）を
  `output/host/squashfs3/unsquashfs` で展開する（[BUILDING.md](../BUILDING.md)）。
- **圧縮された squashfs 像や生 NAND ダンプを直接走査しても見つからない**（実測で確認）。
  必ず展開後の `blur` を対象にすること。

## 7. 未解明＝I2C でどう流すか（段階 C の関門 A1）

**容器の構造はここまでで確定したが、バス上にどう出るかはまだ観察していない。**

分かっていること（[poe-br100-yb1-reception-stack.md](poe-br100-yb1-reception-stack.md) §4.3）:

- 復調器は I2C バス 1 のスレーブ `0x60`。通常のレジスタアクセスは
  **16bit アドレス・16bit 値**で、書きは 4 バイト 1 トランザクション
  `[reg_hi reg_lo val_hi val_lo]`、読みは「`0xfffb` に対象レジスタ番号を書く → 2 バイト読む」。
- **起動時（選局前）に `0x60` 宛ての 48 バイト書き込みが連続する。** 先頭が
  `e4 02 … 2c` `e5 02 … 2c` `e6 02 … 2c` と 1 ずつ進むブロック列。
  これがファーム転送だとみて整合するが、**まだ突き合わせていない**。

確かめること:

| # | 問い | 採り方 |
|---|---|---|
| A1-a | 48 バイトブロックの中身は本当に本節のセグメントデータか | 起動時バストレースを採り、**seg[0] の先頭バイト列 `70 00 00 10 3f fe 10 64` をトレース中に探す**。見つからなければ **32bit ワード単位で byte-swap した `10 00 00 70 64 10 fe 3f` も探す**（メインラインの `write_fw_segment()` は転送前に `convert_endian()` で 32bit ワードを入れ替える＝ホストが LE 前提。**本機のホストは MIPS BE** なので、どちらで出るかは実測でしか決まらない） |
| A1-b | 48 バイトの内訳（宛先アドレス・連番・データの割り当て） | 同じトレースから。ブロック数の見当: データ 13,208 バイトを 1 ブロック 44 バイトで運ぶなら 301 ブロック、46 バイトなら 288 ブロック |
| A1-c | 転送の前後に何が要るか（CPU リセット・クロック・完了の合図） | 同じトレース。`MxLWare68x_API_CfgDevXtal` / `CfgDevPowerMode` / `Ctrl_CfgDevSwitchClock` という API 名が `.rodata` にあるので、順序の当たりは付く |
| A1-d | `entry_address` は転送手順に要るか | 同じトレース中に `0x40010000` が現れるかどうか |

トレースの採り方は §4.2（GPL の `i2c-algo-emma` を自前ビルドして差し替え、
`/app/setting/openird-i2ctrace` を置いて再起動）。**この観察は blur を動かした状態で
行う**（blur 抜きでは誰もファームを流さない）。

## 8. 配布方針

- **ファーム本体は OpenIRD の配布物に含めない。** 所有者が自機の `blur` から実行時に
  取り出す（CLAUDE.md §1-5）。リポジトリにも置かない。
- 本書が置くのは**容器の構造と所在**だけで、ファームの中身についての記述は無い。
- `scripts/extract-mbin.py` は**所有者が自分の機器のファームを取り出す道具**であって、
  鍵の抽出ツールではない（clean-room.md の禁止事項に当たらない）。
