#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD PreToolUse hook: ベンダバイナリの逆アセンブル/逆コンパイルを拒否する。
#
# clean-room 規律（docs/clean-room.md, .claude/rules/clean-room.md）の機械強制。
# CLAUDE.md や rules は「context」であって強制力がないため、context 圧縮で規約が
# 薄れても効き続ける層をここに置く。設定は .claude/settings.json（プロジェクト
# スコープ）＝このフォルダで開いたセッションにだけ効き、他リポジトリには影響しない。
#
# 判定方針:
#   - 制御フロー（＝著作権が守る表現）を読む操作だけを拒否する。
#   - 構造・語彙の観察（strings / readelf / nm / objdump -h,-t,-s / strace）は通す。
#   - gdb 自体は通し、gdb の中でコードを見る操作だけを拒否する。
#   - マークダウンのインラインコード（`…`）は判定前に除去する。規約文書やコミット
#     メッセージでコマンド名に言及できるようにするため。
#   - どうしても通したいときは、コマンドに #cleanroom-allow を明示する。
#
# 想定する脅威は「規約が context から落ちた状態の Claude」であって、回避を試みる
# 攻撃者ではない。したがって難読化（base64 経由の実行など）は検出対象外。
set -e

input="$(cat)"
[ -n "$input" ] || exit 0

# 明示的な意図の宣言があれば通す（乱用しない前提）。
case "$input" in
  *'#cleanroom-allow'*) exit 0 ;;
esac

# インラインコード span を落としてから判定する。
scan="$(printf '%s' "$input" | sed 's/`[^`]*`//g')"

deny() {
  # exit 0 + permissionDecision: deny で拒否する（hooks リファレンス準拠）。
  printf '%s' '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"clean-room 違反: '
  printf '%s' "$1"
  printf '%s' '。OpenIRD は 観察 → 仕様化 → 実装 の順序を固定し、ベンダバイナリの逆アセンブル/逆コンパイルを行わない（.claude/rules/clean-room.md）。構造の観察なら strings / readelf / nm / objdump -h,-t,-s / strace を使う。逆アセが必要ならこのリポジトリの外で別セッションを立て、文章の仕様だけを持ち帰ること。規約自体への言及で誤検知した場合はコマンド名をバッククォートで囲むか #cleanroom-allow を添える。"}}'
  exit 0
}

# --- 1. objdump / otool に逆アセンブル系フラグ（-d / -D / -S）が付いている ---
# 観察用の -h / -t / -s / -x / -r は通す。パスや target triplet の前置は substring
# 一致で拾う（arm-linux-gnueabi-objdump など）。
if printf '%s' "$scan" | grep -qE '(objdump|otool)[^;&|]*[[:space:]]-{1,2}[A-Za-z-]*[dDS]'; then
  deny "objdump/otool による逆アセンブル"
fi

# --- 2. 逆アセンブラ / デコンパイラそのものの起動 ---
if printf '%s' "$scan" | grep -qiE '(radare2|rizin|rz-asm|rz-bin|ghidra|analyzehead|ida64|idapro|idat[[:space:]]|retdec|cstool|objconv|hopper|binaryninja|binja)'; then
  deny "逆アセンブラ/デコンパイラの起動"
fi
if printf '%s' "$scan" | grep -qE '(^|[^A-Za-z0-9_.-])r2[[:space:]]+[-A-Za-z0-9_./~$]'; then
  deny "radare2 (r2) の起動"
fi

# --- 3. gdb の中でコードを見る操作 ---
# データを見る x/32bx 等は通す。命令を見る x/i、disassemble、layout asm は拒否。
if printf '%s' "$scan" | grep -qE '(^|[^A-Za-z0-9_-])disas(semble)?([^A-Za-z0-9_-]|$)|x/[0-9]*i([^A-Za-z0-9]|$)|layout[[:space:]]+asm|disassemble-next-line'; then
  deny "gdb でのコード（命令列）の閲覧"
fi

exit 0
