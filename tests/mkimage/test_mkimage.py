#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""openird-mkimage の自己完結テスト。

秘密（鍵・ZIP パスワード）もベンダコンテナも要求しない＝ CI で回せる。
純正コンテナを持っている人は OPENIRD_V1B_SAMPLES=<dir> を渡すと
オラクル検証（ヘッダの全数検査）が追加で走る。
"""
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MKIMAGE = os.path.join(ROOT, "host", "openird-mkimage")
sys.path.insert(0, MKIMAGE)

from ird import rdhb, v1b            # noqa: E402
from ird.board import h              # noqa: E402

for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8")

_fail = []


def check(name, cond, detail=""):
    print("  %s %s%s" % ("✓" if cond else "✗", name, ("  — " + detail) if detail and not cond else ""))
    if not cond:
        _fail.append(name)


# --- ボード定義 -------------------------------------------------------------

def test_boards():
    for path in sorted(glob.glob(os.path.join(MKIMAGE, "boards", "*.json"))):
        name = os.path.basename(path)[:-5]
        print("[board] %s" % name)
        with open(path, encoding="utf-8") as f:
            b = json.load(f)
        total = h(b["nvm"]["size"])
        erase = h(b["nvm"]["erasesize"])

        pos, gap = 0, None
        for p in b["partitions"]:
            if h(p["offset"]) != pos:
                gap = "mtd%d offset 0x%x != 0x%x" % (p["mtd"], h(p["offset"]), pos)
                break
            pos = h(p["offset"]) + h(p["size"])
        check("区画が隙間なく連なる", gap is None, gap or "")
        check("区画の総和 = NVM 容量", pos == total, "0x%x != 0x%x" % (pos, total))
        check("全区画が消去ブロック境界",
              all(h(p["offset"]) % erase == 0 and h(p["size"]) % erase == 0
                  for p in b["partitions"]))
        check("policy は writable/preserve のみ",
              all(p["policy"] in ("writable", "preserve") for p in b["partitions"]))

        fac = b.get("factory")
        if not fac:
            continue
        # factory の書き込み範囲が区画の中に収まっているか（OpenWrt の check-size 相当を
        # ボード定義そのものに対して掛ける）
        parts = {p["mtd"]: p for p in b["partitions"]}
        prev_end = None
        for im in fac["images"]:
            p = parts[im["mtd"]]
            off, wlen, elen = h(im["write_offset"]), h(im["write_len"]), h(im["erase_len"])
            po, ps = h(p["offset"]), h(p["size"])
            check("%s: 書き込み先が mtd%d の中" % (im["name"], im["mtd"]),
                  off == po and wlen <= ps, "0x%x/0x%x vs 0x%x/0x%x" % (off, wlen, po, ps))
            check("%s: 消去長 >= 書き込み長" % im["name"], elen >= wlen)
            check("%s: 消去長が区画を越えない" % im["name"], elen <= ps)
            if prev_end is not None:
                check("%s: 消去範囲が前区画と連なる" % im["name"], off == prev_end,
                      "0x%x != 0x%x" % (off, prev_end))
            prev_end = off + elen
        check("OpenIRD が書くのは kernel と rootfs のみ",
              sorted(im["mtd"] for im in fac["images"] if im["openird"]) == [2, 3])
        check("OpenIRD が書く区画は policy=writable",
              all(parts[im["mtd"]]["policy"] == "writable"
                  for im in fac["images"] if im["openird"]))
        check("OpenIRD が書かない区画は policy=preserve",
              all(parts[im["mtd"]]["policy"] == "preserve"
                  for im in fac["images"] if not im["openird"]))


# --- v1b ヘッダ層 -----------------------------------------------------------

def test_v1b():
    print("[v1b] ヘッダ層")
    body = bytes(range(256)) * 64          # 16384B = 16B 境界
    hdr = v1b.build_header("BLUR", (2, 1, 17), body)
    check("ヘッダ長 332B", len(hdr) == v1b.HDR_LEN, str(len(hdr)))

    p = v1b.parse(hdr + body)
    check("族タグを復元", p["family"] == "BLUR" and p["tag"] == "BLUR_UPDATE_v1b")
    check("版数を復元", p["version"] == "2.1.17")
    check("本体長を復元", p["body_len"] == len(body))
    for nm, ok in v1b.checks(p):
        check("組み立て直後: " + nm, ok)

    # 本体を 1 バイト壊すと h38 が合わなくなる（＝検算が効いている）
    bad = bytearray(body)
    bad[1234] ^= 0x01
    check("本体の 1 バイト破損を検出", not v1b.sha1_ok(v1b.parse(hdr + bytes(bad))))

    # 16B 境界でない本体は組めない
    try:
        v1b.build_header("BLUR", (1, 0, 0), b"\x00" * 17)
        check("16B 境界でない本体を拒否", False, "例外が出なかった")
    except ValueError:
        check("16B 境界でない本体を拒否", True)

    # v1b でないものを拒否
    for junk, why in ((b"\x00" * 400, "全ゼロ"), (b"PK\x03\x04" + b"\x00" * 400, "素の ZIP")):
        try:
            v1b.parse(junk)
            check("v1b でない入力を拒否 (%s)" % why, False, "例外が出なかった")
        except v1b.NotV1b:
            check("v1b でない入力を拒否 (%s)" % why, True)


# --- RDHB（モジュール分割の回帰） -------------------------------------------

def test_rdhb():
    print("[rdhb] カーネル容器")
    image = (b"OpenIRD test image " * 1000)[:0x4000]
    img = rdhb.build(image, 0x42E08000, rdhb.NAME_DEFAULT, 0)
    check("RDHB マジック", img[:4] == rdhb.MAGIC)
    check("展開して元に戻る", rdhb.inflate(img) == image)
    f = rdhb.fields(img)
    check("load = entry", f["load"] == f["entry"] == 0x42E08000)
    check("展開後長の語が一致", f["uncomp_div4"] * 4 == len(image))
    # 長さは 4 バイト単位の語で入る（端数は切り捨て）。観察した純正像もそうなっている。
    check("総長の語が一致（語単位・端数切り捨て）", f["total_div4"] == len(img) // 4,
          "0x%x != 0x%x" % (f["total_div4"], len(img) // 4))
    check("ペイロード長の語が一致（同上）",
          f["payload_div4"] == (len(img) - rdhb.HDR_LEN) // 4)
    check("name を保持", f["name"] == rdhb.NAME_DEFAULT.encode())


# --- 任意: 純正コンテナに対するオラクル検証 ---------------------------------

def test_samples():
    d = os.environ.get("OPENIRD_V1B_SAMPLES")
    if not d:
        print("[oracle] OPENIRD_V1B_SAMPLES 未指定＝skip（純正コンテナは repo に置かない）")
        return
    files = sorted(glob.glob(os.path.join(d, "*.bin")))
    print("[oracle] %s: %d 本" % (d, len(files)))
    check("サンプルが 1 本以上", bool(files))
    fams = {}
    for path in files:
        with open(path, "rb") as f:
            data = f.read()
        try:
            p = v1b.parse(data)
        except v1b.NotV1b as e:
            check(os.path.basename(path), False, str(e))
            continue
        bad = [n for n, ok in v1b.checks(p) if not ok]
        check(os.path.basename(path), not bad, ", ".join(bad))
        fams[p["family"]] = fams.get(p["family"], 0) + 1
    print("  族: " + ", ".join("%s=%d" % kv for kv in sorted(fams.items())))


def main():
    test_boards()
    test_v1b()
    test_rdhb()
    test_samples()
    print()
    if _fail:
        print("✗ %d 件失敗: %s" % (len(_fail), "; ".join(_fail)))
        return 1
    print("✓ すべて通過")
    return 0


if __name__ == "__main__":
    sys.exit(main())
