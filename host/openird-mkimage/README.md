# openird-mkimage

OpenIRD の install-image ツール（ボード別バックエンド）。`boards/<board>.json` に
確定した区画・ロード・install 情報を持ち、CLI がそれを使う。**配布物・ツールとも
ベンダバイナリ/鍵を 0 バイトに保つ**。鍵が要る経路では鍵を埋め込まず、所有者が
自分の機器から回収した値を実行時に渡す設計にする。鍵の回収そのものは本ツールの
スコープ外で、将来もサブコマンド化しない（CONTRIBUTING.md §0）。

実行は uv 経由（Windows の python stub を避ける）:

```
uv run --no-project python openird_mkimage.py <cmd> ...
```

## ボード別の「factory」経路

- **sb-tv02-wfpl (Fujitsu MB86H61)** — 開発者向けは **booter-root から MTD を直接書く**
  経路（暗号ゲート無し）。一般ユーザ向けの USB 経路は未解明点が残る（NOTES 参照）。
- **emma3sep / *-yb1 (Renesas)** — 純正の更新コンテナ形式を使う経路。**未実装**（別途）。

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
