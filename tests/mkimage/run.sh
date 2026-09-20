#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# openird-mkimage の自己完結テスト（ボード定義・v1b ヘッダ層・RDHB 容器）。
#   使い方: tests/mkimage/run.sh
#   前提:   Python 3.9+ のみ（標準ライブラリだけを使う）。Linux/WSL もベンダ資材も不要。
#   任意:   OPENIRD_V1B_SAMPLES=<dir> を渡すと純正コンテナへのオラクル検証も走る。
set -eu

usage() { sed -n '4,8p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case "${1:-}" in -h|--help) usage 0 ;; esac

T="$(cd "$(dirname "$0")" && pwd)"

# uv があればそれで（Windows の python stub を避ける）。無ければ素の python3。
# uv 経由だと PEP 723 の pycryptodome が入るので AES 層まで検査できる。
# 素の python3 では pycryptodome が無ければその項目だけ skip する。
if command -v uv >/dev/null 2>&1; then
	exec uv run "$T/test_mkimage.py"
elif command -v python3 >/dev/null 2>&1; then
	exec python3 "$T/test_mkimage.py"
else
	echo "✗ Python 3.9+ が見つかりません（uv か python3 を入れてください）"; exit 1
fi
