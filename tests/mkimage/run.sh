#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# openird-mkimage の自己完結テスト（ボード定義・v1b ヘッダ層・RDHB 容器）。
#   使い方: tests/mkimage/run.sh
#   前提:   uv のみ。Linux/WSL もベンダ資材も不要＝どの環境でも回る。
#   任意:   OPENIRD_V1B_SAMPLES=<dir> を渡すと純正コンテナへのオラクル検証も走る。
set -eu

usage() { sed -n '4,8p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
case "${1:-}" in -h|--help) usage 0 ;; esac

T="$(cd "$(dirname "$0")" && pwd)"
exec uv run --no-project python "$T/test_mkimage.py"
