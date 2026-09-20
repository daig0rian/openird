#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.9"
# dependencies = ["pycryptodome>=3.20"]
# ///
"""openird-mkimage の自己完結テスト。

秘密（鍵・ZIP パスワード）もベンダコンテナも要求しない＝ CI で回せる。
純正コンテナを持っている人は OPENIRD_V1B_SAMPLES=<dir> を渡すと
オラクル検証（ヘッダの全数検査）が追加で走る。
"""
import glob
import hashlib
import io
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MKIMAGE = os.path.join(ROOT, "host", "openird-mkimage")
sys.path.insert(0, MKIMAGE)

from ird import rdhb, v1b, zipcrypto  # noqa: E402
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


# --- v1b 暗号層と 3 段ほどき（合成コンテナで往復） ---------------------------

TEST_KEY = b"OpenIRD-test-key"        # 16B。明らかにテスト用＝ベンダの鍵ではない
TEST_PW = b"openird-test-password"


def _zip_of(items, method=zipfile.ZIP_STORED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, data in items:
            z.writestr(name, data)
    return buf.getvalue()


def test_v1b_crypto():
    print("[v1b] 暗号層と 3 段ほどき")
    try:
        v1b._ecb(TEST_KEY)
    except SystemExit:
        print("  – pycryptodome 無し＝skip（uv run openird_mkimage.py で入る）")
        return

    # AES-128-ECB は決定的＝バイト一致で検査できる
    plain16 = bytes(range(16)) * 64
    check("ECB 復号→暗号化が往復", v1b.encrypt_body(v1b.decrypt_body(plain16, TEST_KEY), TEST_KEY) == plain16)
    check("ECB 暗号化→復号が往復", v1b.decrypt_body(v1b.encrypt_body(plain16, TEST_KEY), TEST_KEY) == plain16)
    check("ECB は位置独立（同じ平文ブロック → 同じ暗号文ブロック）",
          v1b.encrypt_body(b"A" * 32, TEST_KEY)[:16] == v1b.encrypt_body(b"A" * 32, TEST_KEY)[16:])
    for bad in (b"\x00" * 15, b"\x00" * 17):
        try:
            v1b.decrypt_body(bad, TEST_KEY)
            check("16B 境界でない本体を拒否", False, "例外が出なかった")
            break
        except ValueError:
            pass
    else:
        check("16B 境界でない本体を拒否", True)

    # 合成コンテナを 3 段そろえて組み、ほどいて往復を見る。中間 ZIP も本物の
    # ZipCrypto（ird/zipcrypto.py）で暗号化する＝代用なしで全段を通す。
    payload_files = [("vmlinux.img", b"\x27\x05\x19\x56" + b"K" * 500),
                     ("rootfs.img", b"sqsh" + b"R" * 900),
                     ("write.sh", b"#!/bin/sh\necho test\n")]
    payload_zip = _zip_of(payload_files)
    csum = "%s  blur.zip\n" % hashlib.sha1(payload_zip).hexdigest()
    inner = zipcrypto.write_zip([("blur.zip", payload_zip), ("csum", csum.encode())],
                                password=TEST_PW, salt=b"\x01" * 11)
    pad = (-len(inner)) % v1b.BODY_ALIGN
    body = v1b.encrypt_body(inner + b"\x00" * pad, TEST_KEY)
    container = v1b.build_header("BLUR", (2, 1, 17), body) + body

    r = v1b.unpack(container, TEST_KEY, TEST_PW)
    check("族と版数を復元", r["header"]["family"] == "BLUR" and r["header"]["version"] == "2.1.17")
    check("ペイロード名を復元", r["payload_name"] == "blur.zip")
    check("csum が一致", r["csum_ok"])
    check("csum のファイル名が一致", r["csum_name_ok"])
    check("ZIP 末尾の詰め物を検出", r["trailing"] == b"\x00" * pad,
          "%r" % (r["trailing"],))
    got = [(e["name"], e["data"]) for e in r["entries"]]
    check("ペイロードの全ファイルが往復", got == payload_files)

    # 鍵が違えば ZIP として開けない
    try:
        v1b.unpack(container, b"wrong-key-16byte", TEST_PW)
        check("誤った鍵を検出", False, "例外が出なかった")
    except ValueError:
        check("誤った鍵を検出", True)

    # ZIP パスワードが違えば中間 ZIP を読めない
    try:
        v1b.unpack(container, TEST_KEY, b"wrong-password")
        check("誤った ZIP パスワードを検出", False, "例外が出なかった")
    except ValueError:
        check("誤った ZIP パスワードを検出", True)


# --- 組み直し（repack-factory と同じ経路を合成コンテナで） -------------------

def _synth_container(key, pw):
    files = [("vmlinux.img", b"\x27\x05\x19\x56" + b"K" * 3000),
             ("rootfs.img", b"sqsh" + b"R" * 9000),
             ("write.sh", b"#!/bin/sh\nexit 0\n")]
    payload = v1b.build_payload_zip(files)
    mid = v1b.build_middle_zip("blur.zip", payload, pw, salt=b"\x02" * 11)
    plain = v1b.pad_to_block(mid)
    return v1b.pack("BLUR", (2, 1, 17), plain, key,
                    h20=bytes(range(16)), h30=b"\xde\xad\xbe\xef"), files


def test_repack():
    print("[repack] 組み直しの 3 段階")
    try:
        v1b._ecb(TEST_KEY)
    except SystemExit:
        print("  – pycryptodome 無し＝skip")
        return
    orig, files = _synth_container(TEST_KEY, TEST_PW)
    r = v1b.unpack(orig, TEST_KEY, TEST_PW)
    hdr = r["header"]

    # none: 平文をそのまま再暗号化 → バイト一致でなければならない（ECB は決定的）
    again = v1b.pack(hdr["family"], tuple(hdr["version_raw"][:3]), r["plain"], TEST_KEY,
                     h20=hdr["h20"], h30=hdr["h30"])
    check("rebuild=none は原本とバイト一致", again == orig)

    # zipcrypto: ペイロード ZIP は原本のまま、中間 ZIP から組み直す
    mid = v1b.build_middle_zip(r["payload_name"], r["payload_zip"], TEST_PW,
                               times=r["entries"][0]["times"], salt=b"\x02" * 11)
    out = v1b.pack(hdr["family"], tuple(hdr["version_raw"][:3]), v1b.pad_to_block(mid),
                   TEST_KEY, h20=hdr["h20"], h30=hdr["h30"])
    v = v1b.unpack(out, TEST_KEY, TEST_PW)
    check("rebuild=zipcrypto: csum が合う", v["csum_ok"])
    check("rebuild=zipcrypto: ペイロードが原本と一致",
          [(e["name"], e["data"]) for e in v["entries"]] == files)

    # all: ペイロード ZIP から組み直す
    payload = v1b.build_payload_zip(r["entries"])
    mid = v1b.build_middle_zip(r["payload_name"], payload, TEST_PW,
                               times=r["entries"][0]["times"], salt=b"\x02" * 11)
    out = v1b.pack(hdr["family"], tuple(hdr["version_raw"][:3]), v1b.pad_to_block(mid),
                   TEST_KEY, h20=hdr["h20"], h30=hdr["h30"])
    v = v1b.unpack(out, TEST_KEY, TEST_PW)
    check("rebuild=all: csum が合う", v["csum_ok"])
    check("rebuild=all: ペイロードが原本と一致",
          [(e["name"], e["data"]) for e in v["entries"]] == files)
    check("rebuild=all: 族/版数/h20/h30 を保つ",
          v["header"]["family"] == "BLUR" and v["header"]["version"] == "2.1.17"
          and v["header"]["h20"] == hdr["h20"] and v["header"]["h30"] == hdr["h30"])
    check("rebuild=all: エントリ順と圧縮方式を保つ",
          [(e["name"], e["method"]) for e in v["entries"]]
          == [(e["name"], e["method"]) for e in r["entries"]])
    check("本体が 16B 境界", v["header"]["body_aligned"])
    check("同じ salt なら決定的",
          v1b.pack(hdr["family"], (2, 1, 17), v1b.pad_to_block(
              v1b.build_middle_zip(r["payload_name"], payload, TEST_PW,
                                   times=r["entries"][0]["times"], salt=b"\x02" * 11)),
              TEST_KEY, h20=hdr["h20"], h30=hdr["h30"]) == out)

    # 純正の形（store + データ記述子 + 時刻由来の check byte）で組めているか
    plain = v1b.decrypt_body(out[v1b.HDR_LEN:], TEST_KEY)
    import struct as _s
    _sig, ver, flg, mth = _s.unpack_from("<IHHH", plain, 0)
    check("中間 ZIP が純正と同じ形（ver=10 / flag=0x0009 / store）",
          (ver, flg, mth) == (10, 0x0009, 0), "ver=%d flag=0x%04x mth=%d" % (ver, flg, mth))


# --- ZipCrypto の書き出し（stdlib の復号器と突き合わせる） -------------------

def test_zipcrypto():
    print("[zipcrypto] ZipCrypto ZIP の書き出し")
    items = [("blur.zip", b"PK\x03\x04" + bytes(range(256)) * 8),
             ("csum", b"0" * 40 + b"  blur.zip\n")]
    for method, mname in ((zipcrypto.STORED, "store"), (zipcrypto.DEFLATED, "deflate")):
        blob = zipcrypto.write_zip(items, password=TEST_PW, method=method, salt=b"\x01" * 11)
        # 独立実装（stdlib の ZipCrypto 復号器）で読み返せること＝相互運用の担保
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            z.setpassword(TEST_PW)
            check("%s: stdlib で復号して往復" % mname,
                  [(i, z.read(i)) for i in z.namelist()] == items)
            check("%s: CRC を通る（testzip）" % mname, z.testzip() is None)
        check("%s: 同じ salt なら同じバイト列（再現ビルド）" % mname,
              zipcrypto.write_zip(items, password=TEST_PW, method=method,
                                  salt=b"\x01" * 11) == blob)
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            z.setpassword(b"wrong-password")
            try:
                z.read("blur.zip")
                check("%s: 誤ったパスワードを拒否" % mname, False, "読めてしまった")
            except RuntimeError:
                # ヘッダ最終バイト（CRC 最上位）の早期チェックが効いている
                check("%s: 誤ったパスワードを拒否" % mname, True)

    # password=None なら素の（無暗号）ZIP＝ペイロード ZIP 側で使う
    plain = zipcrypto.write_zip(items, method=zipcrypto.STORED)
    with zipfile.ZipFile(io.BytesIO(plain)) as z:
        check("password なしなら無暗号 ZIP", [(i, z.read(i)) for i in z.namelist()] == items)

    check("自前 encrypt→decrypt が往復",
          zipcrypto.decrypt(zipcrypto.encrypt(b"OpenIRD" * 30, TEST_PW), TEST_PW)
          == b"OpenIRD" * 30)
    check("鍵ストリームはパスワード依存",
          zipcrypto.encrypt(b"A" * 16, b"pw1") != zipcrypto.encrypt(b"A" * 16, b"pw2"))


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
    test_v1b_crypto()
    test_repack()
    test_zipcrypto()
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
