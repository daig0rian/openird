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
  **これが本線**。観察（`probe-factory`）・ほどき直し（`unpack-factory` / `repack-factory`）と、
  OpenIRD 自身のイメージを包む `pack-factory` がある。

## サブコマンド（sb-tv02-wfpl）

| cmd | 内容 |
|---|---|
| `partitions <board>` | 区画表（writable / preserve）を表示 |
| `pack-rootfs <board> <in.squashfs> <out> [--mtd N]` | squashfs をカーネルが読める形式か（4.0・gzip・xattr 無し）と対象区画の容量で検査して出力。既定は mtd6、救助用は `--mtd 8` |
| `pack-kernel <board> <Image> <out>` | 生カーネル Image を RDHB 梱包（実機 round-trip 検証済） |
| `unpack-kernel <RDHB> [out]` | RDHB イメージを検証・展開（生 Image 取り出し） |
| `flash-script <board> [--rootfs][--kernel][--firstaid] [--via mtd\|dione] [-o]` | 機器上で実行する書き込み手順を生成（preserve 区画はガードで拒否）。既定は OpenIRD の `mtd` ツール、`--via dione` は区画切り直し前のベンダ状態向け |
| `probe-kernel-header <dump>` | 任意ダンプ先頭を観察（RDHB/PIX-/hsqs 探索） |
| `full <board>` | 未解明のため refuse（NOTES の H2/H3） |

## サブコマンド（poe-br100-yb1）

| cmd | 鍵 | 内容 |
|---|---|---|
| `partitions poe-br100-yb1` | 不要 | 区画表（GPL の区画表由来・実機ダンプと一致を確認済み） |
| `probe-factory <file>...` | **不要** | v1b コンテナのヘッダ 332B を読む。族タグ・版数・**h38 = SHA-1(本体)** の検算・本体長の 16B 整合。複数渡すと表で出る |
| `unpack-factory <file> <outdir>` | 要 | 3 段復号（AES-128-ECB → ZipCrypto → 無暗号 ZIP）。段ごとに成果物を残し、csum を検算する |
| `repack-factory <in> <out>` | 要 | 純正コンテナを組み直す。`--rebuild none/zipcrypto/all` でどの層から自前にするかを選べる。`--replace NAME=PATH` でペイロードの 1 点を差し替え（書き込み固定長を超えたら hard error） |
| `pack-factory <board> <out> [--kernel K] [--rootfs R] [--fw-version X.Y.Z] [--payload-only]` | 要（`--payload-only` は不要） | OpenIRD の kernel / rootfs から factory コンテナを作る。渡されたイメージが機種の契約（uImage の形・squashfs の版・書き込み固定長・ブートローダの展開余裕）に合うかを検査し、`write.sh` を仕様どおり生成する。書くのは kernel と rootfs だけで、ほかの区画には触れない。`--payload-only` は暗号層を被せないペイロード ZIP だけを出す |

ビルド済みのカーネルと rootfs から factory イメージを作るのは `make factory BOARD=<board>`
（`scripts/make-factory.sh`。版と機種から出力名を決めて `pack-factory` を呼ぶ）。リリースでは
GitHub Actions が同じスクリプトを使う（[docs/release.md](../../docs/release.md)）。

`repack-factory --rebuild none` は原本とバイト一致する（ヘッダと AES 層の回帰テストになる）。
`--replace` で差し替えた場合、自己検証は「差し替えなかった点は原本とバイト一致」「差し替えた点は
渡したファイルと一致」を別々に確かめる。

`unpack-factory` の出力先は**リポジトリの外**か、git 管理外の `temp/` `output/` `build/` に
限る（復号結果はベンダ由来の解析素材で commit してはいけない）。ツールが機械的に拒否する。

形式の仕様は [update-v1b-container.md](../../docs/specs/update-v1b-container.md)、機器側の
書き込み機構は [poe-br100-yb1-nand-update.md](../../docs/specs/poe-br100-yb1-nand-update.md)。
できたコンテナを機器へ渡すのは [openird-fwup](../openird-fwup/)（鍵を扱わない別ツール）。
実装の段取りと残っている不明点は **NOTES-poe-br100-yb1-factory.md**。

### 鍵と ZIP パスワードの渡し方

**ツールに埋め込まない。既定値も持たない。** 所有者が自機から回収した値を、値ではなく
**ファイルのパス**で渡す（値をコマンドライン引数や環境変数に載せると `ps` と shell history
に残るため）。ツールは値を出力せず、指紋（SHA-256 先頭 4B）だけを示す。

既定の置き場は **リポジトリの外**:

| ファイル | 中身 |
|---|---|
| `~/.openird/v1b/aes-128-ecb.key` | 16 進 32 文字（空白・`:`・`0x`・`#` コメントは無視）または生 16B |
| `~/.openird/v1b/zip.pass` | パスワード 1 行。末尾の改行だけ落とす（行末スペースは残す） |

`--key-file` / `--zip-pw-file`、環境変数 `OPENIRD_V1B_KEY_FILE` / `OPENIRD_V1B_ZIPPW_FILE`
でも指せる（いずれも**パス**であって値ではない）。

RDHB 形式の仕様は [docs/specs/rdhb-kernel-container.md](../../docs/specs/rdhb-kernel-container.md)。
区画レイアウトと実機への導入順は
[board/mb86h61/README.md](../../br-ext-chip-fujitsu/board/mb86h61/README.md)。
残っている不明点は **NOTES-sb-tv02-factory.md**。

### pack-kernel メモ

入力は **非圧縮の生カーネル Image**（`arch/arm/boot/Image`。zImage/gzip ではない）。
libooter は zlib を inflate して load=entry=0x42e08000 へ展開しジャンプする。
ヘッダ `0x08` のチェックサムは **libooter が検証しない**ことを実機で確認済みなので、
既定の 0 で起動するイメージが作れる（`--cksum 0x..` で上書き可）。mtd5 は単一面なので、
書き込み前に全ダンプでバックアップを取ること。

### pack-rootfs メモ

2.6.31 の squashfs は zlib のみ（lzo は 2.6.36、xz は 2.6.38 から）、xattr 入り inode は
2.6.35 から。これらは実機で mount に失敗して初めて分かるので、ボード定義
（`boards/<board>.json` の `squashfs`）と突き合わせてビルド時に弾く。
