#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD SessionStart hook (matcher: compact): context 圧縮の直後に clean-room の
# 要点を再注入する。
#
# paths: を持たない .claude/rules/clean-room.md は圧縮後に disk から再注入される
# 仕様なので、これは冗長化。狙いは 2 つ: (1) 再注入の挙動に依存しない経路を残す、
# (2) 参照用の規範文書ではなく短い命令形として、圧縮直後の context に置く。
# matcher を compact だけにしているのは、起動時・/clear 時には rules が通常どおり
# ロードされて二重になるため。
cat <<'EOF'
[OpenIRD / clean-room] context 圧縮後の再確認:
- 順序は 観察 → 仕様化 → 実装 で固定。この順序を破る作業はしない。
- ベンダバイナリの逆アセンブル/逆コンパイルはしない。PreToolUse hook が実行を
  拒否する。データ・構造・語彙の観察は可、コード（命令列）を読む操作は不可。
- 逆アセ由来の内部ロジック・具体アドレス・コード由来の数式は、docs/specs/ にも
  コードにも書かない。置き場所も設けない。
- 限定受信（B-CAS / MULTI2 / ECM→Ks / ARIB スクランブル解除）は永久にスコープ外。
- commit は Origin: trailer が必須。clean-room の実装は Spec-Ref も必須。
- 逆アセが必要になったら、このリポジトリの外で別セッションを立てる。
詳細: .claude/rules/clean-room.md / docs/clean-room.md
EOF
