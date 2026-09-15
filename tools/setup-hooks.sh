#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# OpenIRD: git フックと commit テンプレートを有効化する（clone 後に一度だけ）。
set -e
root="$(git rev-parse --show-toplevel)"
cd "$root"
git config core.hooksPath .githooks
git config commit.template .gitmessage
chmod +x .githooks/* 2>/dev/null || true
echo "✓ core.hooksPath=.githooks / commit.template=.gitmessage を設定しました。"
echo "  以後 'git commit -s' で DCO 署名が付きます。"
