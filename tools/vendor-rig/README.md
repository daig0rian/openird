# vendor-rig

> **2026-09-29 に `tools/vendor-base/` へ置き換えた。** 実機はベンダイメージ（カーネル・区画表・
> slot をダンプのまま）＋rcS の入口 1 か所に戻してある。この rig（自前カーネル＋改造 slot0）は
> 経緯と再現のために残す。

段階 B の**観測 rig** — ベンダのユーザランドを自前カーネルの上でそのまま動かし、
**ベンダアプリ（`dione_server`）だけを自動起動しない**状態を作るための差分。

**配布イメージには入らない。** 所有者が自分の機器を観察するためだけのもの。ベンダ
バイナリは 1 バイトも改変せず、触るのは起動スクリプト 2 本だけ。仕様と根拠は
[docs/specs/sb-tv02-wfpl-vendor-boot.md](../../docs/specs/sb-tv02-wfpl-vendor-boot.md)。

## なぜ必要か

ベンダアプリは受信の中核ロジックを持つ観察対象であって、こちらの都合で起動してほしい。
`sb-tv02-wfpl-stage-b-fapex.md` §5 の chroot サンドボックスは環境を部分再現するため、
`fapex_fdlclkpwr` を外す・`/dev` から mtd を隠すといった差が入り、デーモンの挙動に
サンドボックス由来の副作用が出ていた。rig は**純正の環境をそのまま使い、最後の
2 行だけ抜く**ので、その副作用が無い。

## 中身

| ファイル | 置き場所 | 役割 |
|---|---|---|
| `rcS` | ベンダ rootfs の `/etc/init.d/rcS` を置換 | `slot_selector` を通さず `mtdblock7` を `/slot` に mount。`dmesg -n 7` |
| `slot-entry-tail.sh` | `slot_entry.sh` の末尾に連結 | ベンダアプリ起動（`insmod h57fmb.ko` ＋ `while : dione_server`）を除去し、静的 IP・`telnetd`・案内を追加 |
| `rig-daemon.sh` | slot0 に `/slot/rig-daemon.sh` として同梱 | ベンダアプリを「コンソールをシェルとして残したまま」起動する |

観察ツールは `tools/` にある成果物が `/slot` へ同梱される（`fapex_trace.so`・`fapex-ts`・
`fapex-i2c`・`fapex-gpio`・`fapex-poke`）。無いものは警告して飛ばす。毎回 wget で入れ直す
必要が無く、デーモンがネットワークを奪った後でも使える。

`slot_selector` を外すのは**安全上の理由**で、これは省略できない。同ツールは
Slot Duplication（`/dev/mtd8` と `/dev/mtd9` を erase して slot0 を複製）を持ち、
OpenIRD の区画表では `/dev/mtd8` が firstaid＝唯一の救助経路にあたる。

## ビルド

```sh
scripts/build-vendor-rig.sh --verify \
  --rootfs <所有者のダンプ>/mtd6_rootfs.img \
  --slot0  <所有者のダンプ>/mtd7_slot0.img
```

root 権限は要らない。`--verify` は「無改変で再梱包して元のマニフェストと完全一致するか」
の往復テストを先に走らせる（rootfs 910 / slot0 142 エントリ、setuid busybox と
451 個のデバイスノードの major/minor を含めて一致することを確認済み）。

出力は `temp/rig/rig-{rootfs,slot0}.sqfs`。書き込み手順はスクリプトが最後に表示する。

**ext4 上で実行すること。** `/mnt/c`（DrvFs）では `chmod` と特殊ファイルが期待どおりに
効かない。`--work` で作業領域を指定できる（既定 `~/openird-rig`）。

## 使い方（実機）

起動するとコンソールが root シェルになる。そこから:

```sh
/slot/rig-daemon.sh                        # ベンダアプリ起動（コンソールは残る）
echo "forceinit"                > /tmp/dbgin
echo "dpower 1 1"               > /tmp/dbgin
echo "settuner 0 557142857 0 0" > /tmp/dbgin   # 地上 ch27。選局＋TS 経路
echo "gsignal"                  > /tmp/dbgin
echo "chktunerts"               > /tmp/dbgin
```

工場テストシェルのコマンド 84 個・引数形式・実機の制約（ウォッチドッグ、`br0` と
eth0 の衝突、TSD の競合、偽 Factory Port）は仕様の §5・§6 にある。
