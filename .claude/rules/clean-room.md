<!-- SPDX-License-Identifier: GPL-2.0-only -->
# clean-room 規律（OpenIRD・常時適用）

順序は **観察 → 仕様化 → 実装** で固定。これを破る作業は行わない。規範の全文は
[docs/clean-room.md](../../docs/clean-room.md)、スコープ境界は
[CONTRIBUTING.md](../../CONTRIBUTING.md) §0。

## 禁止（例外なし）

- **ベンダバイナリの逆アセンブル・逆コンパイル。** ホスト・実機のどちらでも、
  `ssh openird '…'` 越しでも同じ。`.claude/hooks/no-disasm.sh` が PreToolUse で
  機械的に拒否する。
- **逆アセ由来の内部ロジック・具体アドレス・コード由来の数式**を `docs/specs/` や
  コードに書くこと。仕様にも書かないし、置き場所も設けない。
- 限定受信（B-CAS / MULTI2 / ECM→Ks / keyladder / ARIB スクランブル解除）の鍵・
  手順・回避技術。永久にスコープ外。
- ベンダバイナリ・ダンプ・ファーム・鍵の commit。鍵を機器から抽出するツールの commit。

## 観察として許されるもの

`strings` / usage / ログ / `/proc` / `strace` / `ltrace` / バス信号 / USB 列挙 /
`readelf -h,-l,-d,-s` / `nm` / `objdump -h,-t,-s`。事実・構造・語彙の抽出であって、
制御フロー（＝著作権が守る表現）には踏み込まない。

gdb / gdbserver 自体は観察手段として使ってよい。**データ**を見るメモリダンプ
（`x/32bx $r1` 等）は可、**コード**を見る `disassemble` / `x/i` / `layout asm` は
不可。ここが「制御フローに踏み込まない」の実務上の境界線。

## 逆アセが必要になったら

このリポジトリのセッションでは**やらない**。repo 外の別ディレクトリで別セッションを
立てる（hook は OpenIRD セッションにだけ効く＝意図した非対称）。成果は**文章の仕様
だけ**を人手で運び、コピペではなく書き直す。逆アセ出力を実装セッションに持ち込んだ
時点で、以後そのセッションで書いた物は `Origin: clean-room` を宣言できない。

事故で逆アセ出力が context に入ったら **`/compact` ではなく `/clear`**。圧縮は汚染を
要約として残したまま出自の記録を失う、最悪の組み合わせ。

## commit trailer（[.githooks/commit-msg](../../.githooks/commit-msg) が検査）

`Origin: clean-room | gpl-derived | observed | upstream`。clean-room の実装コードは
`Spec-Ref: docs/specs/<file>#<section>` が必須。

## hook の誤検知を外す

規約自体を書く・grep する等で禁止語が commit や検索文字列に現れる場合、コマンド内の
マークダウン インラインコード（`` ` `` で囲んだ部分）は判定前に除去されるので、
コマンド名はバッククォートで囲めば通る。それでも誤検知するときだけ、コマンドに
`#cleanroom-allow` を明示的に添える（意図の宣言。乱用しない）。
