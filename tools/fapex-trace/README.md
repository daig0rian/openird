# fapex-trace

段階 B の観察ツール。ベンダの受信プロセス（`dione_server`）に `LD_PRELOAD` で
差し込み、FAPex の chardev（`/dev/fapi/fapiNNN`）に対する **ioctl の引数バイト列**を
ログに落とす。strace は ioctl 番号と引数ポインタまでしか出さないが、選局で復調・
チューナに書く実バイト列は引数構造体の**中のポインタの先**にある。それを取るのが
目的（`docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §4）。

**配布イメージには入らない。** 所有者が自分の機器で観察するためだけのもの。

## 何を記録するか

- `/dev/*` の open / close（ファイルディスクリプタに名前を付ける）
- type=`0xFF`（FAPex）の ioctl すべて。`_IOC_WRITE` は呼び出し前の入力、
  `_IOC_READ` は呼び出し後の出力を、引数のバイト列ごと。
- I2C read/write（nr=0x62）は、引数構造体が指す送信・受信バッファの中身も。
  write は非 ACK slave でハングし得るので、**呼び出し前に入力側を `[pre-call]` 行で先出し**する
  （ハングしても復調器 init のレジスタ列が残る）。
- **h57fmb0（Dione の USB char dev, type=`0xae`）の ioctl も黒箱ログ**（h57fmb は GPL 非収録＝
  proprietary。struct 未知なので `_IOC_SIZE` 分をダンプ）。ファーム転送・GET_INFO/GET_FACTOR 等。
- `/dev/*` の mmap（`/dev/mem` 経由のレジスタ窓など）。

**bring-up シム（既定 off・観察専用ではない）**: 対象プロセスの **chroot 内**にフラグファイルを
置くと有効になる。
- `/tmp/fapex-h57-retry`: h57fmb0 の ioctl が `EPROTO(-71)` または `ENODEV(-19)` を返したら短い
  待ちを挟んで retry する。`-71` は Dione ファームがリセット解除後に起動するまでの吸収。
  `-19` は **Dione が FW 起動時に一度自己再列挙する**（USB disconnect addr N → reconnect N+1）
  ことで daemon の既存 fd が stale になるため、その fd のパスを開き直して `dup2` で同じ fd 番号に
  貼り替えてから retry する（＝再列挙後のデバイスに透過的に再束縛。§5.5）。
- `/tmp/fapex-h57-quiet`: h57fmb0 の ioctl のログ出力を抑制する（シム/passthrough は維持）。
  Dione FW 転送 ~1500 回のログ I/O を省いて観察ノイズと負荷を下げる（§5.5）。
- `/tmp/fapex-i2c-settle`（内容＝ms、空/0 は 3000）: 最初の復調器（slave `0x20`）への FAPex I2C
  write の直前に一度 sleep する（Dione の post-FW 再列挙を待つ汎用 knob）。**注（§5.5）**: これ単体
  では daemon の demod write ハングは解消しない。
- `/tmp/fapex-i2c-proxy`: daemon の FAPex I2C RW を、daemon 自身の fd でなく **tracer が open し直した
  fresh ハンドル**で代理実行する（daemon の I2C_OPEN param を捕捉して再適用。診断行 `[proxydiag]`/
  `[proxy]` を出す）。**結論（§5.5）**: これでも demod write はハングする＝**ハングは fd 固有でなく
  「daemon プロセス固有」**（別プロセスの fresh ハンドルなら通るのに、daemon プロセス内では fresh でも
  ハング）。in-process では解けず cross-process 委譲が要る、と判明させた診断用シム。
- `/tmp/fapex-i2c-deleg`: **cross-process I2C 委譲（§5.6）**。FAPex I2C RW を、in-process でなく **別プロセスの
  ヘルパ `fapex-i2cd`** へ abstract AF_UNIX datagram ソケット `@fapex-i2cd`（netns スコープ＝chroot を跨ぐ）で
  転送し、reply の rx を daemon のバッファに書き戻す。ヘルパは自プロセスで fresh ハンドルを開くのでハングしない
  ＝daemon が demod／tuner init を最後まで進み、その列を観察できる。使う前に別途 `fapex-i2cd`（`tools/fapex-i2c`）を
  実機で起動しておく。診断行 `[deleg] fd=.. rc=..`。ソケット未起動なら `DELEG_FAIL` で通常呼び出しにフォールバック。

1 行 = 1 回の `write()`。スレッドをまたいでも行が混ざらない。ログは
`/tmp/fapex-trace.log`（プロセスの chroot 内）。

## 設計（なぜ libc に依存しないか）

ベンダのユーザランドは **uClibc 0.9.31**。このライブラリは **どの libc にも
リンクしない**（`-nostdlib`）。カーネルへは raw syscall（`syscall.S`）で話し、
外から借りるのは `__errno_location` だけ（interpose した open/ioctl が呼び出し元の
errno を正しく立てるため。プロセスの uClibc が実行時に解決する）。だから uClibc
ツールチェーンを用意する必要がなく、カーネルと同じ ARM クロス gcc 単体で作れる。

- interpose する入口（`open`/`open64`/`openat`/`close`/`ioctl`/`mmap`/`mmap64`）は
  **default 可視性で export**（`.dynsym` に出す）。`-fvisibility=hidden` を付けると
  隠れて interpose できなくなるので付けない。内部ヘルパーは `static`。
- ioctl 番号の分解と I2C 引数のレイアウトは FAPex の GPL ヘッダ（FAPexK
  `abst_lib.h` / `abst_libi2c.h`）由来。ベンダバイナリの逆アセは見ていない。

## ビルド

```
scripts/build-fapex-trace.sh            # dl/ の gcc4.9.4 を使う（要 make kernel 済み）
```
→ `tools/fapex-trace/fapex_trace.so`。undefined シンボルが `__errno_location`
だけであることを確認して出す。

## 使い方（実機・観察）

`docs/specs/sb-tv02-wfpl-stage-b-fapex.md` §5 の RAM だけの chroot サンドボックスを
組み、FAPex `.ko` を insmod した上で:

```sh
cp fapex_trace.so /tmp/vendor/root/tmp/
chroot /tmp/vendor/root /bin/busybox \
  env LD_LIBRARY_PATH=/slot/lib LD_PRELOAD=/tmp/fapex_trace.so \
  /slot/app/dione_server <args>
cat /tmp/vendor/root/tmp/fapex-trace.log
```

`dione_server -s`（リセット SW 読み）での検証例（2026-09-17 実機）:

```
open /dev/fapi/fapi186 -> fd 3
ioctl /dev/fapi/fapi186 nr=0x40 dir=W size=8 in=[8]:44 00 00 00 00 00 01 01 = 0   # GPIO_OPEN pin=0x44
ioctl /dev/fapi/fapi186 nr=0x41 dir=R size=4 out=[4]:01 00 00 00 = 0              # GPIO_READBIT -> 1
```

## 限界

- interpose するのは、動的シンボルとして `open`/`ioctl` 等を呼ぶ呼び出しだけ。
  libc 内部から直接呼ぶものや、生 syscall を出すコードは捕まえない（ベンダの
  受信部品は libc の open/ioctl を PLT 経由で呼ぶので捕まる）。
- 引数バッファは先頭 512 バイトまで。ポインタの妥当性は長さでしか守っていない
  （観察対象は正しいユーザポインタを渡す前提）。
