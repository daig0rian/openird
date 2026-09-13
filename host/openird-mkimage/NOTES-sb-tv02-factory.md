# SB-TV02-WFPL factory.bin 生成 — 確定事項と不足情報（更新: 2026-09-13 実ダンプ反映）

> 目的: SB-TV02-WFPL（Fujitsu MB86H61）で OpenIRD を初回導入する image を作る。
> 方針（ユーザ指示）: **不明点は推測で埋めず HOLD に書き出し、確定作業を先に進める**。
> 今回、実機全ダンプ `temp/sbtv02_fw.bin`（mtd0..10 連結 16MB）が得られ、**H1 を解決**した。

## 0. 結論（更新）

SB-TV02 の factory は AES コンテナではなく「**booter-root → `dione_server -w /dev/mtdN:<file>`
の暗号ゲート無し生書き込み**」（確定・spec §8.6/§8.9）。必要な成果物 2 つのうち:

- `rootfs.squashfs` → mtd6（raw・確定。`pack-rootfs`）
- kernel 像 → mtd5（**RDHB ラッパー＝今回解明**。`pack-kernel` 実装・実機 round-trip 検証済）

→ **factory パイプラインは一通り動く**。残る唯一の不確実性は RDHB の `0x08`
チェックサム（H1b）で、これは「libooter が検証するか」の実機確認に帰着する。

## 1. H1 解決の内容（実ダンプ検証・Open＝構造の静的読み）

mtd5(0x140000) のカーネルラッパー **RDHB** を完全に同定。仕様は
[docs/specs/rdhb-kernel-container.md](../../docs/specs/rdhb-kernel-container.md)。要点:

- 76B ヘッダ ＋ ペイロード(zlib `78 9c` + deflate)。inflate 結果＝**生 ARM カーネル Image**
  （`Linux version 2.6.31.6-rt19 (airtuner@shiki) gcc4.5.1 Buildroot2010.11`）。
- `0x44/0x48` = load=entry = **0x42e08000**（GPL Makefile.boot と一致）。
- サイズ 3 種は **÷4 の語数**: `0x40`=展開後/4, `0x3c`=payload/4, `0x30`=総長/4（実機値で算出一致）。
- libooter は **zlib 末尾 adler が欠損(2B+0xff)でも起動**＝checksum 検証は緩い公算。
- **検証**: 実機 mtd5 を `unpack-kernel` → 生 Image 取り出し → `pack-kernel` で再梱包 →
  再 unpack が **元 Image と byte 完全一致**。さらに再梱包ヘッダの size/load/name が
  **実機と同値**。圧縮 deflate も実機と byte 一致（= zlib lv6）。唯一 `0x08` のみ相違。

## 2. 不足情報（HOLD — 推測で埋めない）

| # | 不明点 | 状態 | 解決手順 | 影響 |
|---|---|---|---|---|
| ~~H1~~ | RDHB 構造 | **解決** | 実ダンプで同定・spec 化・pack-kernel 実装 | — |
| **H1b** | RDHB `0x08` チェックサムのアルゴリズムと、libooter が検証するか | **未特定** | 標準 CRC(各catalog)/Adler/Fletcher/sum いずれも不一致＝独自。→ **実機テスト**: `0x08=0` の自作 kernel を mtd5 に焼き起動可否を見る（全ダンプがある＝`dione_server -w` で即復旧）。どうしても要れば libooter 逆アセ(Close) | 中（起動可否の確証） |
| H2 | `.full`(`updater.full`) の `Updater_decode` 本体変換 | **昇格** | **end-user USB factory 経路の唯一の関門**（dev の `-w` 生書きには不要だが、ユーザ配布には必須）。純正 `.full` 入手→構造解析 / `dione_server -d` 素通しテスト。割れたら `pack-full` 実装 | 高(配布) |
| H3 | `Updater_Header` の byte レイアウト | 昇格 | H2 と一体で解明 | 高(配布) |
| H4 | kernel .config 確定版 | 進行可 | §C から menuconfig。実ビルドで確定 | 中 |
| H6 | userland libc（musl / uClibc-ng） | 未定 | 段階A=musl 基本。blob 併用なら uClibc 系。CLAUDE.md §10 | 中 |
| H7 | rootfs が 1MB(mtd6) に収まるか | 実ビルド待ち | ビルド後 `pack-rootfs` で実測。超過なら slot 併用/構成削減 | 中 |

### C 補足（H4 の出発点・GPL 由来の確定デルタ）

GPL `ubqmb86h61devkit-mtd_defconfig` を出発点に、実機へ合わせる差分:

- `CONFIG_MEM_SIZE_RAMIFS=0`（defconfig は 0x08000000）。0 でないと load が 0x22e08000 に
  なる。実機は 0x40000000 起動＝RAMIFS=0 分岐 → C3 の 0x42e08000 に一致。
- `CONFIG_CMDLINE` を `root=/dev/mtdblock6 ... squashfs` に（cmdline は libooter が渡す）。
- SQUASHFS を有効化（実機 rootfs は squashfs 4.0）。
- 区画は `mb86h61_devkit.c` の静的配列（Pixela #24244 2-slot 版）を使う。
- 出力は `arch/arm/boot/Image`（**非圧縮**）。これを `pack-kernel` に渡す。

## 3. 作った物（今夜・確定範囲）

- `boards/sb-tv02-wfpl.json` … 区画/ロード/install の機械可読データ（provenance 付き・MAC 非格納）。
- `docs/specs/rdhb-kernel-container.md` … RDHB 形式の Open 仕様（clean-room firewall）。
- `openird_mkimage.py` … CLI。**確定・動作確認済**:
  - `partitions` / `pack-rootfs`（squashfs 検証）/ `pack-kernel`（RDHB 梱包・実機 round-trip 検証）/
    `unpack-kernel`（展開・検証）/ `flash-script`（`dione_server -w`・preserve 区画ガード）/ `probe-kernel-header`。
  - HOLD refuse: `full`(H2/H3)。
- 検証ログ: 実機 mtd5 round-trip byte 一致、size/load/name 一致、factory flow（kernel+rootfs）生成。

### 確定経路での factory 手順（現状）

```
1. kernel をビルド → arch/arm/boot/Image（非圧縮, §C デルタ適用）
   openird_mkimage.py pack-kernel  sb-tv02-wfpl Image        mtd5.img
2. rootfs をビルド → squashfs4.0 (≤1MB)
   openird_mkimage.py pack-rootfs  sb-tv02-wfpl rootfs.sqfs  mtd6.img
3. 実機: libooter で init=/bin/sh root（可逆）→ slot_entry 相当を再現 → dione_server 起動
4. openird_mkimage.py flash-script sb-tv02-wfpl --kernel mtd5.img --rootfs mtd6.img -o flash.sh
   を実機で実行（dione_server -w /dev/mtd5:... , /dev/mtd6:...）
5. ★初回は mtd5=0x08 未検証＝起動可否を確認。失敗時は全ダンプから mtd5 を書き戻し
   （dione_server -w /dev/mtd5:<元mtd5>）。mtd0/1 は無傷＝ブリック回避。
```

## 4. 次に進められること（HOLD を待たず）

1. **kernel クリーンビルド**（GPL 2.6.31.6-rt19 + §C）→ `Image` → `pack-kernel`。
   → `0x08=0` で1本作り、**H1b の実機起動テスト**（最小の決定的実験）。
2. 段階A 最小 rootfs（musl/uClibc-ng）→ squashfs → `pack-rootfs` で mtd6 収まり実測（H7）。
3. `br-ext-chip-fujitsu` の linux.config に §C デルタを反映。
4. （任意）`0x08` 追い込み: deflate が実機と byte 一致＝既知バイト列上の検査値。
   標準アルゴリズムは外したので、独自（テーブル/16bit/加算系の変種）を広めに総当たりする
   余地はある。ただし優先は H1b 実機テスト（検証が緩ければ 0x08 はそもそも不問）。

> GPL tarball は `dl/` に置き commit しない。ダンプ `temp/sbtv02_fw.bin` は解析専用＝commit 禁止。
