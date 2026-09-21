# yb1-trace

POE-BR100-YB1（BMMPX1301 / Renesas EMMA3SEP・MIPS BE）のベンダアプリ `blur` に
被せる `LD_PRELOAD` の ioctl トレーサ。段階 B の観察専用で、**配布物には入らない**。

```
scripts/build-yb1-trace.sh          # dl/ にツールチェーンを取得してビルド
→ tools/yb1-trace/yb1_trace.so
```

## 何を出すか

- `/dev/` 以下の `open` / `close`（fd に名前を付けるため）
- その fd への **すべての ioctl**。MIPS の `_IOC` で dir/type/nr/size に分解し、
  `_IOC_WRITE` なら呼び出し前の入力、`_IOC_READ` なら呼び出し後の出力をバイト列で出す
- **4 バイト引数がユーザ空間アドレスだったときはその先 64 バイト**。この基板の
  `/dev/io_controller` はコマンド／イベントのバッファをこの形で渡す
- 任意で `/dev/*` への `read()` / `write()`（既定は無効。TS 経路でログが溢れるため）

## libc に依存しない

ベンダのユーザランドは uClibc 0.9.31。このライブラリは raw syscall だけで動き、
`__errno_location` のみを取り込む（相手の libc が解決する）。∴ uClibc の
ツールチェーンは要らず、カーネルと同じ kernel.org の MIPS クロス gcc で作れる。
ビルドスクリプトが「未定義シンボルが `__errno_location` だけ」であることを検査する。

## 設定（`/app/setting` に置く。mtd8＝更新で上書きされない区画）

| ファイル | 効果 |
|---|---|
| `openird-trace` | トレースを有効にする（`rcS` がこれを見て `LD_PRELOAD` を立てる） |
| `openird-trace-rw` | `/dev/*` への `read()`/`write()` も記録する |
| `openird-trace-noderef` | 4 バイト引数のポインタ展開を止める |

**設定はプロセス起動時に 1 度だけ読まれる。** 変更を効かせるには再起動が要る。
また **`blur` は kill できない**（WDT でボードごと落ちる）ので、別の `LD_PRELOAD` で
起動し直す形の観察はできない。詳しくは
[docs/specs/poe-br100-yb1-reception-stack.md](../../docs/specs/poe-br100-yb1-reception-stack.md) §8。

ログは `/tmp/yb1-trace.log`。1 行 = 1 回の `write()` なのでスレッド間で混ざらない。

## clean-room 上の位置づけ

ioctl の分解はアーキテクチャの `_IOC` マクロ（上流カーネルの uapi）に従うだけで、
ベンダのドライバを読んで得た知識は入っていない。各コマンドの**意味**は観察で
決める。出力は解析素材であって配布素材ではない。
