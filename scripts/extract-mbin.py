#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""MaxLinear の MBIN ファームウェア容器を走査して取り出す。

  使い方:
    scripts/extract-mbin.py <file> --list          見つけた容器を一覧する
    scripts/extract-mbin.py <file> -o fw.mbin      取り出す（1 個のときだけ）

対象はベンダバイナリ（POE-BR100-YB1 なら /app/bin/blur）。**所有者が自分の機器から
自分のファームを取り出すための道具**で、取り出した中身は OpenIRD の配布物には
入れない（CLAUDE.md §1-5）。

オフセットは決め打ちせず、走査してチェックサムとセグメントの歩きで自己検証する
（ベンダ FW の版が変わると位置が動くため）。

仕様: docs/specs/mbin-firmware-container.md
"""
import argparse
import hashlib
import sys

HDR_LEN = 16
SEG_HDR_LEN = 8
MAX_SEGMENTS = 32


def be(b):
    return int.from_bytes(b, "big")


def parse_at(buf, off):
    """off に妥当な MBIN があれば (header, segments, blob) を返す。無ければ None。"""
    h = buf[off:off + HDR_LEN]
    if len(h) < HDR_LEN or h[0:2] != b"M1" or h[2] != HDR_LEN:
        return None
    nseg = h[3]
    if not 1 <= nseg <= MAX_SEGMENTS:
        return None
    image_size = be(h[8:11])
    if image_size < 0x100 or off + HDR_LEN + image_size > len(buf):
        return None
    body = buf[off + HDR_LEN:off + HDR_LEN + image_size]
    if (sum(body) & 0xFF) != h[11]:
        return None

    segs, p = [], 0
    for _ in range(nseg):
        s = body[p:p + SEG_HDR_LEN]
        if len(s) < SEG_HDR_LEN or s[0:1] != b"S":
            return None
        seg_len, seg_addr = be(s[1:4]), be(s[4:8])
        segs.append((seg_addr, seg_len))
        p += SEG_HDR_LEN + ((seg_len + 3) // 4) * 4
    if p != image_size:
        return None

    hdr = {
        "num_segments": nseg,
        "entry_address": be(h[4:8]),
        "image_size": image_size,
        "checksum": h[11],
    }
    return hdr, segs, h + body


def scan(buf):
    out, off = [], -1
    while True:
        off = buf.find(b"M1", off + 1)
        if off < 0:
            return out
        got = parse_at(buf, off)
        if got:
            out.append((off,) + got)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", help="走査するベンダバイナリ")
    ap.add_argument("-o", "--output", help="取り出し先（見つかった容器が 1 個のときだけ）")
    ap.add_argument("--list", action="store_true", help="一覧するだけ")
    ap.add_argument("--index", type=int, default=None,
                    help="複数見つかったとき、取り出す番号（0 始まり）")
    args = ap.parse_args()

    with open(args.file, "rb") as f:
        buf = f.read()
    found = scan(buf)

    print(f"{args.file}: {len(buf)} バイト / 妥当な MBIN = {len(found)} 個")
    for i, (off, hdr, segs, blob) in enumerate(found):
        print(f"  [{i}] off=0x{off:08x} 全長={len(blob)} "
              f"segments={hdr['num_segments']} "
              f"entry=0x{hdr['entry_address']:08x} "
              f"checksum=0x{hdr['checksum']:02x}")
        print(f"      sha256={hashlib.sha256(blob).hexdigest()}")
        for j, (addr, ln) in enumerate(segs):
            print(f"      seg[{j}] addr=0x{addr:08x} len={ln} "
                  f"(4B 整列後 {((ln + 3) // 4) * 4})")

    if not found:
        print("見つかりませんでした。圧縮された squashfs 像や生の NAND ダンプでは"
              "なく、展開後のバイナリを対象にしてください。", file=sys.stderr)
        return 1
    if args.list or not args.output:
        return 0

    if len(found) > 1 and args.index is None:
        print("複数見つかりました。--index で選んでください。", file=sys.stderr)
        return 1
    blob = found[args.index or 0][3]
    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"→ {args.output} に {len(blob)} バイト書きました")
    return 0


if __name__ == "__main__":
    sys.exit(main())
