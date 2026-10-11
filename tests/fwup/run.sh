#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# openird-fwup の自己完結テスト（偽の機器を 127.0.0.1 に立てて、投入の全経路を通す）。
#   使い方: tests/fwup/run.sh
#   前提:   Python 3.9+ のみ（標準ライブラリだけを使う）。実機もベンダ資材も不要。
#           待ち受けはループバックだけで、外には何も出ない。
set -eu

usage() { sed -n '4,7p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case "${1:-}" in -h|--help) usage 0 ;; esac

T="$(cd "$(dirname "$0")" && pwd)"

# uv があればそれで（Windows の python stub を避ける）。無ければ素の python3。
if command -v uv >/dev/null 2>&1; then
	exec uv run "$T/test_fwup.py"
elif command -v python3 >/dev/null 2>&1; then
	exec python3 "$T/test_fwup.py"
else
	echo "✗ Python 3.9+ が見つかりません（uv か python3 を入れてください）"; exit 1
fi
