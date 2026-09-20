# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""ボード定義（boards/<board>.json）の読み込みと区画の参照。"""
import json
import os
import sys

BOARDS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "boards")


def load_board(name):
    path = os.path.join(BOARDS_DIR, name + ".json")
    if not os.path.exists(path):
        sys.exit("✗ 未知のボード: %s（boards/ に %s.json が無い）" % (name, name))
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def h(x):
    """JSON 中の "0x..." 文字列 / 数値 / None を int または None にする。"""
    if x is None:
        return None
    return int(x, 16) if isinstance(x, str) else int(x)


def part_by_mtd(board, mtd):
    for p in board["partitions"]:
        if p["mtd"] == mtd:
            return p
    return None
