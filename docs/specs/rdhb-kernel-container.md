# RDHB カーネルコンテナ形式（SB-TV02-WFPL / mtd5）

<!-- Open 層: 実機ダンプの「データ構造」の静的読み（逆アセではない）。PII 無し。
     実装 (host/openird-mkimage pack-kernel) はこの仕様のみを根拠に書く（clean-room）。 -->

libooter（proprietary・GPL 非収録）が mtd5「kernel bslot」から読み込むカーネル像の
ラッパー。GPL には無いため、実機 mtd5 ダンプの構造観察から仕様化した（Open）。

## 全体

```
[ 0x00 .. 0x4b ]  RDHB ヘッダ (76 バイト固定)
[ 0x4c .. EOF  ]  ペイロード = zlib ストリーム (78 9c + deflate + 4B adler スロット)
[ 末尾         ]  0xff パディング（SPI-NOR 消去値）
```

- ペイロードを inflate した結果 = **生 ARM カーネル Image**（非 zImage。libooter が
  inflate して load アドレスへ展開し、entry へジャンプする）。実測で
  `Linux version 2.6.31.6-rt19 (airtuner@shiki) (gcc 4.5.1, Buildroot 2010.11)` を含む。
- zlib の末尾 adler32 は実機で **先頭2バイトのみ + 0xff** に欠損していたが起動する
  ＝**libooter は adler を検証しない（raw-inflate 相当）**。→ 自作像は正規の zlib
  ストリームで問題ない見込み。

## ヘッダ（76B, リトルエンディアン u32）

| off | 値(実測) | 意味 | 確度 |
|---|---|---|---|
| 0x00 | `"RDHB"` (52 44 48 42) | マジック | 確定 |
| 0x04 | 0xfffffffe | フラグ/型（意味未確認の定数） | 観察定数 |
| 0x08 | 0x92cf0c83 | **チェックサム（アルゴリズム未特定）** | **HOLD** |
| 0x0c | `"PIX-DT270-FW-Linux\0"` | 名前（0x0c..0x1f, NUL 詰め, 20B 枠） | 確定 |
| 0x20 | 0 | （未確認定数） | 観察定数 |
| 0x24 | 1 | （未確認定数） | 観察定数 |
| 0x28 | 1 | （未確認定数） | 観察定数 |
| 0x2c | 1 | （未確認定数） | 観察定数 |
| 0x30 | 0x48f4b | **総イメージ長 ÷ 4**（= (0x4c + payload) / 4, floor） | 確定(算出一致) |
| 0x34 | 1 | （未確認定数） | 観察定数 |
| 0x38 | 0x301 | （版/型らしき定数） | 観察定数 |
| 0x3c | 0x48f38 | **ペイロード長 ÷ 4**（payload = zlib 全体, floor） | 確定(算出一致) |
| 0x40 | 0x91ca8 | **展開後カーネル長 ÷ 4** | 確定(算出一致) |
| 0x44 | 0x42e08000 | ロードアドレス | 確定(GPL Makefile.boot と一致) |
| 0x48 | 0x42e08000 | エントリアドレス（=load） | 確定 |

実測での算出一致（本機の1サンプル）:
- 展開後 = 2,388,640 B → ÷4 = 597,160 = 0x40 ✓
- ペイロード = 1,195,234 B → ÷4 = 298,808 = 0x3c ✓
- 総長 = 1,195,310 B → ÷4 = 298,827 = 0x30 ✓
- load/entry = 0x42e08000 = GPL `Makefile.boot`（memory_start 0x40000000 + FIRM_AREA
  0x02e00000 + 0x8000）= booter ログ "ARM jumps to 0x42e08000" ✓

## 自作カーネル像の作り方（pack-kernel が実装）

1. カーネルをビルド → `arch/arm/boot/Image`（**非圧縮・生 Image**）。
2. zlib 圧縮（level 6 = `78 9c`）。
3. 76B ヘッダを組む: RDHB / 0x04・0x20-0x2c・0x34・0x38 は観察定数を複製 /
   名前 / サイズ3種を上式で算出 / load=entry=0x42e08000。
4. ヘッダ + zlib を連結 → mtd5 へ（1.5MB=0x180000 以内）。

## 未解決（HOLD・推測で埋めない）

- **0x08 チェックサム**: 標準 crc32/adler32/sum/xor（対象領域を複数試行）で一致せず＝
  独自。**libooter が検証するか不明**（adler 欠損でも起動＝検証が緩い公算）。
  解決: (a) 0x08=0 等で1本焼いて起動可否を実機確認（mtd5 は単一面だが、全16MB
  ダンプがある＝SPI クリップ/`dione_server -w` で復旧可）、(b) どうしても要れば
  libooter 逆アセ（Close）。→ 実装は 0x08 を可変にし、既定は「未検証」警告を出す。
- 0x04 / 0x38 等の定数の意味（版・型）。自作では観察値を複製して回避。
