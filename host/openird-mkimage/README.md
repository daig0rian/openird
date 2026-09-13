# openird-mkimage

OpenIRD の install-image ツール（ボード別バックエンド）。`boards/<board>.json` に
確定した区画・ロード・install 情報を持ち、CLI がそれを使う。**配布物・ツールとも
ベンダバイナリ/鍵を 0 バイトに保つ**（鍵が要る経路は鍵を埋めず外から渡す設計）。

実行は uv 経由（Windows の python stub を避ける）:

```
uv run --no-project python openird_mkimage.py <cmd> ...
```

## ボード別の「factory」経路

- **sb-tv02-wfpl (Fujitsu MB86H61)** — factory ＝ **booter-root + `dione_server -w` 生書き込み**
  （暗号ゲート無し＝確定・spec §8.6/§8.9）。AES コンテナは無い。
- **emma3sep / *-yb1 (Renesas)** — factory ＝ UPnP 経由の `BLUR_UPDATE_v1b`（AES-ECB・族共有鍵）。
  鍵は NAND から回収してビルド時に渡す。**鍵回収前なので未実装**（別途）。

## サブコマンド（sb-tv02-wfpl）

| cmd | 状態 | 内容 |
|---|---|---|
| `partitions <board>` | 確定 | 区画表（writable / preserve）を表示 |
| `pack-rootfs <board> <in.squashfs> <out>` | 確定 | squashfs4.0 マジック＋mtd6 容量を検証して出力（raw） |
| `pack-kernel <board> <Image> <out>` | **確定** | 生カーネル Image を RDHB 梱包（実機 mtd5 で round-trip 検証済）。`0x08` のみ HOLD H1b |
| `unpack-kernel <RDHB> [out]` | 確定 | RDHB 像を検証・展開（生 Image 取り出し・デバッグ/検証用） |
| `flash-script <board> [--rootfs][--kernel][--slot] [-o]` | 確定 | `dione_server -w` の手順を生成（preserve 区画はガードで拒否） |
| `probe-kernel-header <dump>` | 補助 | 任意ダンプ先頭を観察（RDHB/PIX-/hsqs 探索） |
| `full <board>` | HOLD H2/H3 | `.full` コンテナは本体変換未解明のため refuse（確定経路では不要） |

RDHB 形式の仕様は [docs/specs/rdhb-kernel-container.md](../../docs/specs/rdhb-kernel-container.md)。
不明点（HOLD）の一覧と解決手順は **NOTES-sb-tv02-factory.md** を参照。

### pack-kernel メモ

入力は **非圧縮の生カーネル Image**（`arch/arm/boot/Image`。zImage/gzip ではない）。
libooter は zlib を inflate して load=entry=0x42e08000 へ展開しジャンプする。
`0x08` チェックサムは未特定（標準 CRC/Adler/Fletcher/sum いずれも不一致）＝既定 0 で
生成し警告を出す。libooter は壊れた adler でも起動する実測があり検証は緩い公算だが、
初回は実機で起動可否を確認すること（`--cksum 0x..` で上書き可）。mtd5 は単一面なので
書き込み前に全ダンプでバックアップ。
