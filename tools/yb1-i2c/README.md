# yb1-i2c

POE-BR100-YB1 の I2C バスをユーザランドから叩く単体ツール。段階 B の観察専用で、
**配布物には入らない**。

```
scripts/build-yb1-i2c.sh            # dl/ にツールチェーンを取得してビルド
→ tools/yb1-i2c/yb1-i2c            （静的・libc 非依存・約 4KB）
```

## なぜ作れるか

この基板の i2c アダプタは **Renesas が GPL v2 で書いた `i2c_emma` / `i2c_algo_emma`**
で、Linux 標準の i2c-dev がそのまま生える。∴ `/dev/i2c-N` に `I2C_RDWR` を投げる
だけでバスに届く＝**ベンダのインターフェースを一切介さない**。段階 C の足場になる。

## 使い方

```
yb1-i2c <bus> probe [first] [last]   応答するアドレスを走査（7bit）
yb1-i2c <bus> r <addr> <reg> [n]     reg を書いてリピートスタート、n バイト読む
yb1-i2c <bus> d <addr> <reg> <n>     n 個のレジスタを 1 個ずつ読んで並べる
yb1-i2c <bus> w <addr> <byte>...     バイト列を書く
```

数値は 10 進、`0x` 前置で 16 進。実機へは `uudecode` 経由で `/tmp` に置けば足りる
（`scp` も `base64` も無いが busybox の `uudecode` はある）。**焼き直しは不要。**

## 実機で踏んだ注意点

- **バス 0 を触らないこと。** 何も載っておらず、転送がエラーで返らずアダプタ内で
  ブロックする。出力は 16 アドレスごとに flush するので、殺しても途中までは残る。
- バス 1 はベンダドライバが使用中。i2c コアが転送を直列化するので波形は壊れないが、
  相手の内部レジスタポインタを乱す可能性はある。**選局していないときに走査し、
  あとで選局し直して健全性を確かめる。**
- 走査は `dmesg` に `-----> I2C force Stop bit` を残すが、これは NAK からの正常復帰。

測定結果は
[docs/specs/poe-br100-yb1-reception-stack.md](../../docs/specs/poe-br100-yb1-reception-stack.md) §4.1。

## libc に依存しない

kernel.org の MIPS クロスはターゲット libc を持たないので、`_start` と raw syscall を
自前で書いて静的リンクしている（`-mno-abicalls -fno-pic -G0`）。ビルドスクリプトが
「動的リンクでない」「未定義シンボルが無い」ことを検査する。
