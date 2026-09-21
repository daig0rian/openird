# yb1-mxl

POE-BR100-YB1 の復調器（MaxLinear MXL683）を**ベンダアプリ抜きで**立ち上げる。
段階 C のフロントエンド実装の最初の一歩。

```
scripts/build-yb1-mxl.sh          # 固定した MIPS BE クロスで建てる
→ tools/yb1-mxl/yb1-mxl           （静的・libc 非依存・約 9.6KB）
```

## 出自（clean-room）

**仕様だけを見て書いた。** 根拠は

- [docs/specs/poe-br100-yb1-reception-stack.md](../../docs/specs/poe-br100-yb1-reception-stack.md) §29
  （コールドスタートの列 A / C / D）
- [docs/specs/mbin-firmware-container.md](../../docs/specs/mbin-firmware-container.md) §2,§3,§5,§7
  （容器の構造と I2C 転送）

ベンダバイナリの逆アセンブルはしていない。仕様の元になった事実は
**バス観察**（GPL の i2c ドライバにログを足して採ったバイト列）。

## ファームウェアは同梱しない

MXL683 はチップ内 RAM にファームを入れないと動かない。そのファームは
**所有者の機器上のベンダアプリ `/app/bin/blur` の中**にあり、このツールは
**実行時にそこを走査して取り出して流す**。OpenIRD の配布物にもこの repo にも
ファームは入らない（CLAUDE.md §1-5）。

## 使い方

```
yb1-mxl <bus> probe                  素性レジスタ 0x3001 を読む（0x1012 が返る）
yb1-mxl <bus> rd <reg>               16bit 読み
yb1-mxl <bus> rd32 <reg>             32bit 読み（0xfffd 経由）
yb1-mxl <bus> wr <reg> <val>         16bit 書き
yb1-mxl <bus> scan <file>            MBIN を探して形を表示する
yb1-mxl <bus> load <file>            列 A → ファーム転送 → 起動確認 → 列 C
yb1-mxl <bus> tune <MHz>             列 D（地上波の整数呼び値）
yb1-mxl <bus> bringup <file> [MHz]   load してから tune
yb1-mxl <bus> status                 ロック・品質のレジスタ
```

`<bus>` は `/dev/i2c-N` の N。**本機は 1**（バス 0 は何も載っておらず、転送が
アダプタ内でブロックする）。

## 前提

**ベンダアプリが動いていない状態で使うこと。** 動いていると資源を握られていて
デマルチプレクサ側が取れない（受信スタック仕様 §25）。`/app/setting/openird-noblur`
を置いて再起動する（§24.1）。**その状態では `blur` が起動する `udhcpc` が無いので、
起動フックの側で IP を振っておかないと telnet に入れない。**

## 実機で確認済み（2026-09-22）

コールドスタート → ファーム転送（13,181 バイト / 301 ブロック）→ `0xa8cc = 0xabcd`
→ 選局 → チャンネルごとの品質測定まで。詳細は受信スタック仕様 §30。
