# openird-mkimage

OpenIRD の install-image ツール（ボード別バックエンド）。`boards/<board>.json` に
確定した区画・ロード・install 情報を持ち、CLI がそれを使う。**配布物・ツールとも
ベンダバイナリ/鍵を 0 バイトに保つ**。鍵が要る経路では鍵を埋め込まず、所有者が
自分の機器から回収した値を実行時に渡す設計にする。鍵の回収そのものは本ツールの
スコープ外で、将来もサブコマンド化しない（CONTRIBUTING.md §0）。

実行は uv 経由（Windows の python stub を避ける）:

```
uv run openird_mkimage.py <cmd> ...                       # 依存も解決する
uv run --no-project python openird_mkimage.py <cmd> ...   # 依存不要のコマンドのみ
```

依存は v1b コンテナの AES-128-ECB だけが必要とする（PEP 723 のインラインメタデータで
`pycryptodome` を宣言）。他のコマンドは標準ライブラリのみで動く。

構成は **1 容器形式 = 1 モジュール**（OpenWrt firmware-utils の作法）:

| ファイル | 役割 |
|---|---|
| `openird_mkimage.py` | CLI（引数定義とディスパッチ） |
| `ird/board.py` | `boards/<board>.json` の読み込み |
| `ird/images.py` | squashfs / uImage の形式検査 |
| `ird/rdhb.py` | mb86h61 のカーネル容器 RDHB |
| `ird/v1b.py` | emma3sep の更新コンテナ `<FAMILY>_UPDATE_v1b` |

自己完結テストは `make test-mkimage`（`tests/mkimage/run.sh`）。秘密もベンダ資材も
要求しない。純正コンテナを持っているなら `OPENIRD_V1B_SAMPLES=<dir>` でオラクル検証も走る。

## ボード別の「factory」経路

- **sb-tv02-wfpl (Fujitsu MB86H61)** — 開発者向けは **booter-root から MTD を直接書く**
  経路（暗号ゲート無し）。一般ユーザ向けの USB 経路は未解明点が残る（NOTES 参照）。
- **poe-br100-yb1 (Renesas EMMA3SEP)** — 純正の更新コンテナ `BLUR_UPDATE_v1b` を UPnP で
  流し込む経路。この機種には開発者向けの直接書き込み経路が無い（u-boot がロック）ので
  **これが本線**。現状は `probe-factory`（鍵不要の観察）まで実装済み。

## サブコマンド（sb-tv02-wfpl）

| cmd | 内容 |
|---|---|
| `partitions <board>` | 区画表（writable / preserve）を表示 |
| `pack-rootfs <board> <in.squashfs> <out> [--mtd N]` | squashfs をカーネルが読める形式か（4.0・gzip・xattr 無し）と対象区画の容量で検査して出力。既定は mtd6、救助用は `--mtd 8` |
| `pack-kernel <board> <Image> <out>` | 生カーネル Image を RDHB 梱包（実機 round-trip 検証済） |
| `unpack-kernel <RDHB> [out]` | RDHB 像を検証・展開（生 Image 取り出し） |
| `flash-script <board> [--rootfs][--kernel][--firstaid] [--via mtd\|dione] [-o]` | 機器上で実行する書き込み手順を生成（preserve 区画はガードで拒否）。既定は OpenIRD の `mtd` ツール、`--via dione` は区画切り直し前のベンダ状態向け |
| `probe-kernel-header <dump>` | 任意ダンプ先頭を観察（RDHB/PIX-/hsqs 探索） |
| `full <board>` | 未解明のため refuse（NOTES の H2/H3） |

## サブコマンド（poe-br100-yb1）

| cmd | 鍵 | 内容 |
|---|---|---|
| `partitions poe-br100-yb1` | 不要 | 区画表（GPL の区画表由来・実機ダンプと一致を確認済み） |
| `probe-factory <file>...` | **不要** | v1b コンテナのヘッダ 332B を読む。族タグ・版数・**h38 = SHA-1(本体)** の検算・本体長の 16B 整合。複数渡すと表で出る |
| `unpack-factory <file> <outdir>` | 要 | 3 段復号（AES-128-ECB → ZipCrypto → 無暗号 ZIP）。**未実装（P1）** |
| `pack-factory <board> ...` | 要 | factory コンテナ生成。**未実装（P2）** |

形式の仕様は [update-v1b-container.md](../../docs/specs/update-v1b-container.md)、機器側の
書き込み機構は [poe-br100-yb1-nand-update.md](../../docs/specs/poe-br100-yb1-nand-update.md)。
実装の段取りと残っている不明点は **NOTES-poe-br100-yb1-factory.md**。

**鍵と ZIP パスワードはツールに埋め込まない。** 所有者が自機から回収した値を、値ではなく
**ファイルのパス**で渡す設計にする（`ps` と shell history に残さないため）。ツールは鍵を
出力せず、どの鍵を使ったかはフィンガープリントだけを示す。

RDHB 形式の仕様は [docs/specs/rdhb-kernel-container.md](../../docs/specs/rdhb-kernel-container.md)。
区画レイアウトと実機への導入順は
[board/mb86h61/README.md](../../br-ext-chip-fujitsu/board/mb86h61/README.md)。
残っている不明点は **NOTES-sb-tv02-factory.md**。

### pack-kernel メモ

入力は **非圧縮の生カーネル Image**（`arch/arm/boot/Image`。zImage/gzip ではない）。
libooter は zlib を inflate して load=entry=0x42e08000 へ展開しジャンプする。
ヘッダ `0x08` のチェックサムは **libooter が検証しない**ことを実機で確認済みなので、
既定の 0 で起動する像が作れる（`--cksum 0x..` で上書き可）。mtd5 は単一面なので、
書き込み前に全ダンプでバックアップを取ること。

### pack-rootfs メモ

2.6.31 の squashfs は zlib のみ（lzo は 2.6.36、xz は 2.6.38 から）、xattr 入り inode は
2.6.35 から。これらは実機で mount に失敗して初めて分かるので、ボード定義
（`boards/<board>.json` の `squashfs`）と突き合わせてビルド時に弾く。
