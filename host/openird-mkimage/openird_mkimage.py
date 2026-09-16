#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""openird-mkimage - OpenIRD install-image ツール（ボード別バックエンド）。

現状の実装範囲:
  mb86h61 / sb-tv02-wfpl : 確定経路（booter-root + dione_server -w 生書き込み）。
      raw 書き込みは暗号ゲート無し（spec §8.6/§8.9）＝確定。
  emma3sep / *-yb1       : BLUR_UPDATE_v1b (AES-ECB) 版は鍵回収後（未実装・別途）。

不明点（HOLD）は推測で埋めない。refuse して理由を示す。詳細は
NOTES-sb-tv02-factory.md を参照。配布物にベンダバイナリ/鍵を含めない。
"""
import sys
import os
import json
import struct
import zlib
import argparse

for _s in (sys.stdout, sys.stderr):            # Windows cp932 既定を回避
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8")

HERE = os.path.dirname(os.path.abspath(__file__))
SQUASHFS_MAGIC = b"hsqs"   # squashfs 4.0 LE (0x73717368)


def load_board(name):
    path = os.path.join(HERE, "boards", name + ".json")
    if not os.path.exists(path):
        sys.exit("✗ 未知のボード: %s（boards/ に %s.json が無い）" % (name, name))
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def h(x):
    return int(x, 16) if isinstance(x, str) else int(x)


def part_by_mtd(board, mtd):
    for p in board["partitions"]:
        if p["mtd"] == mtd:
            return p
    return None


def cmd_partitions(a):
    b = load_board(a.board)
    print("# %s  (%s)  NVM %s  erase %s"
          % (b["board"], b["soc"], b["nvm"]["size"], b["nvm"]["erasesize"]))
    print("%3s %-14s %10s %10s %-9s note" % ("mtd", "name", "offset", "size", "policy"))
    for p in b["partitions"]:
        print("%3d %-14s %10s %10s %-9s %s"
              % (p["mtd"], p["name"], p["offset"], p["size"], p["policy"], p.get("note", "")))


SQUASHFS_COMP = {1: "gzip", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}
SQUASHFS_INVALID_BLK = 0xFFFFFFFFFFFFFFFF
SQUASHFS_FLAG_COMP_OPT = 1 << 10


def check_squashfs(data, spec):
    """squashfs 4.0 スーパーブロック（先頭 96B・LE）をボードのカーネルが読める形か検査する。
    spec = board["squashfs"]: {"compressors": [...], "xattrs": bool}。"""
    if len(data) < 96:
        sys.exit("✗ squashfs が短すぎます（%dB）。" % len(data))
    (_magic, _inodes, _mtime, block_size, _frags, comp, _blog, flags, _nids,
     major, minor, _root, bytes_used, _idt, xattr_t) = struct.unpack_from("<IIIIIHHHHHHQQQQ", data, 0)
    if (major, minor) != (4, 0):
        sys.exit("✗ squashfs %d.%d（4.0 のみ対応）。" % (major, minor))
    name = SQUASHFS_COMP.get(comp, "id%d" % comp)
    if name not in spec.get("compressors", ["gzip"]):
        sys.exit("✗ 圧縮 %s はこのボードのカーネルで読めません（可: %s）。"
                 % (name, ", ".join(spec["compressors"])))
    if flags & SQUASHFS_FLAG_COMP_OPT and not spec.get("comp_options", False):
        sys.exit("✗ 圧縮オプション付き（flags 0x%x）。このボードのカーネルは解釈しません。" % flags)
    if xattr_t != SQUASHFS_INVALID_BLK and not spec.get("xattrs", False):
        sys.exit("✗ xattr テーブル付き。このボードのカーネルは xattr 入り inode を読めません（mksquashfs -no-xattrs）。")
    print("  squashfs 4.0 / %s / block %dK / bytes_used %dB / xattr %s"
          % (name, block_size // 1024, bytes_used, "有" if xattr_t != SQUASHFS_INVALID_BLK else "無"))


def cmd_pack_rootfs(a):
    b = load_board(a.board)
    part = part_by_mtd(b, a.mtd)
    if part is None:
        sys.exit("✗ mtd%d はこのボードに存在しない。" % a.mtd)
    if part["policy"] != "writable":
        sys.exit("✗ mtd%d(%s) は policy=%s＝書き込み禁止。" % (a.mtd, part["name"], part["policy"]))
    with open(a.infile, "rb") as f:
        data = f.read()
    if data[:4] != SQUASHFS_MAGIC:
        sys.exit("✗ squashfs マジック 'hsqs' が先頭にありません（得: %r）。" % data[:4])
    check_squashfs(data, b.get("squashfs", {}))
    cap = h(part["size"])
    if len(data) > cap:
        sys.exit("✗ rootfs %dB が mtd%d(%s) 容量 %dB を超過。" % (len(data), a.mtd, part["name"], cap))
    with open(a.outfile, "wb") as f:
        f.write(data)
    print("✓ rootfs OK: %dB / %dB (%d%%) mtd%d(%s) → %s"
          % (len(data), cap, 100 * len(data) // cap, a.mtd, part["name"], a.outfile))
    print("  → 書き込み: mtd write <file> /dev/mtd%d（区画を切り直した後）" % a.mtd)


# --- RDHB コンテナ（docs/specs/rdhb-kernel-container.md より。clean-room） ---
RDHB_MAGIC = b"RDHB"
RDHB_HDR_LEN = 0x4c            # 76B 固定
RDHB_NAME_DEFAULT = "PIX-DT270-FW-Linux"   # 観察値。libooter が照合する可能性があり既定で複製
RDHB_CONST = {0x04: 0xfffffffe, 0x20: 0, 0x24: 1, 0x28: 1, 0x2c: 1, 0x34: 1, 0x38: 0x301}


def build_rdhb(image, load, name, cksum):
    """生カーネル Image → RDHB ラッパー。docs/specs/rdhb-kernel-container.md の仕様どおり。"""
    payload = zlib.compress(image, 6)          # 78 9c + deflate + adler4
    total = RDHB_HDR_LEN + len(payload)
    hdr = bytearray(b"\x00" * RDHB_HDR_LEN)
    hdr[0x00:0x04] = RDHB_MAGIC
    for off, v in RDHB_CONST.items():
        struct.pack_into("<I", hdr, off, v)
    struct.pack_into("<I", hdr, 0x08, cksum)           # HOLD: 既定 0
    nm = name.encode("ascii")[:19]
    hdr[0x0c:0x0c + len(nm)] = nm                       # 0x0c..0x1f, NUL 詰め
    struct.pack_into("<I", hdr, 0x30, total // 4)       # 総長 ÷4
    struct.pack_into("<I", hdr, 0x3c, len(payload) // 4)  # ペイロード長 ÷4
    struct.pack_into("<I", hdr, 0x40, len(image) // 4)  # 展開後長 ÷4
    struct.pack_into("<I", hdr, 0x44, load)             # load
    struct.pack_into("<I", hdr, 0x48, load)             # entry = load
    return bytes(hdr) + payload


def cmd_pack_kernel(a):
    b = load_board(a.board)
    mtd5 = part_by_mtd(b, 5)
    cap = h(mtd5["size"])
    load = h(b["load"]["zreladdr"]) if not a.load else h(a.load)
    with open(a.infile, "rb") as f:
        image = f.read()
    if image[:2] == b"\x1f\x8b" or image[:2] == b"\x78\x9c":
        sys.exit("✗ 入力は *非圧縮* の生カーネル Image（arch/arm/boot/Image）を渡してください（gzip/zlib ではない）。")
    cksum = h(a.cksum) if a.cksum else 0
    img = build_rdhb(image, load, a.name, cksum)
    if len(img) > cap:
        sys.exit("✗ RDHB 像 %dB > mtd5 容量 %dB。" % (len(img), cap))
    # 自己検証: 生成物を展開し直して元 Image と一致するか
    d = zlib.decompressobj(wbits=-15)
    back = d.decompress(img[RDHB_HDR_LEN + 2:]) + d.flush()
    if back != image:
        sys.exit("✗ 内部エラー: 再展開が元 Image と不一致。")
    with open(a.outfile, "wb") as f:
        f.write(img)
    print("✓ RDHB 像: %dB (%d%% of mtd5 %dB) → %s"
          % (len(img), 100 * len(img) // cap, cap, a.outfile))
    print("  load=entry=0x%08x  name=%r  展開後=%dB" % (load, a.name, len(image)))
    if cksum == 0:
        print("  ⚠ HOLD(H1b): 0x08 チェックサム=0 で生成（アルゴリズム未特定）。libooter は")
        print("     adler 欠損でも起動＝検証が緩い公算だが、起動可否は実機確認が必要。")
        print("     判明したら --cksum 0xXXXXXXXX で指定可。mtd5 は単一面＝全ダンプでバックアップ必須。")


def cmd_unpack_kernel(a):
    with open(a.infile, "rb") as f:
        img = f.read()
    if img[:4] != RDHB_MAGIC:
        sys.exit("✗ RDHB マジックがありません（得: %r）。" % img[:4])
    g = lambda o: struct.unpack_from("<I", img, o)[0]
    name = img[0x0c:0x24].split(b"\x00", 1)[0]
    print("# RDHB ヘッダ")
    print("  name=%r  load=0x%08x entry=0x%08x  cksum(0x08)=0x%08x"
          % (name, g(0x44), g(0x48), g(0x08)))
    print("  size語: total/4=0x%x payload/4=0x%x uncomp/4=0x%x" % (g(0x30), g(0x3c), g(0x40)))
    d = zlib.decompressobj(wbits=-15)
    out = d.decompress(img[RDHB_HDR_LEN + 2:]) + d.flush()   # 78 9c を飛ばし raw-inflate
    print("  展開後: %dB (0x40*4=%dB, %s)"
          % (len(out), g(0x40) * 4, "一致" if len(out) == g(0x40) * 4 else "不一致"))
    i = out.find(b"Linux version")
    if i >= 0:
        print("  banner: %r" % out[i:i + 70])
    if a.outfile:
        with open(a.outfile, "wb") as f:
            f.write(out)
        print("  → 生 Image を %s に出力" % a.outfile)


def cmd_flash_script(a):
    b = load_board(a.board)

    def guard(mtd):
        p = part_by_mtd(b, mtd)
        if p is None:
            sys.exit("✗ mtd%d は存在しない" % mtd)
        if p["policy"] != "writable":
            sys.exit("✗ mtd%d(%s) は policy=%s＝書き込み禁止。" % (mtd, p["name"], p["policy"]))
        return p

    def write(mtd, path):
        p = guard(mtd)
        if a.via == "mtd":
            return ['echo "[+] %s -> mtd%d"' % (p["name"], mtd),
                    "mtd write %s /dev/mtd%d" % (path, mtd),
                    "mtd verify %s /dev/mtd%d" % (path, mtd)]
        return ['echo "[+] %s -> mtd%d"' % (p["name"], mtd),
                "dione_server -w /dev/mtd%d:%s" % (mtd, path)]

    if a.via == "mtd":
        pre = ["# 前提: OpenIRD の rootfs（mtd ツール入り）で起動している状態。",
               "#       mtd6 を書くときは firstaid(mtd8) から起動すること（自分自身は上書きできない）。"]
    else:
        pre = ["# 前提: 区画切り直し前のベンダ状態。libooter 'Wait for kbd input' → init=/bin/sh で起動し、",
               "#       slot(mtd7/8) を mount して dione_server を使える状態（spec §8.5/§8.6/§8.9）。"]
    lines = ["#!/bin/sh",
             "# openird-mkimage 生成: %s on-device フラッシュ手順" % b["board"]] + pre + [
        "# 温存: mtd0(libooter)/mtd1(env=MAC,cmdline)/nvram/slot_info は絶対に書かない。",
        "set -e",
    ]
    if a.rootfs:
        lines += write(6, a.rootfs)
    if a.kernel:
        lines += [
            "# kernel 像は RDHB 梱包済(pack-kernel)。mtd5 は単一面＝書く前に全ダンプでバックアップ。",
        ] + write(5, a.kernel)
    if a.firstaid:
        lines += write(8, a.firstaid)
    if not (a.rootfs or a.kernel or a.firstaid):
        sys.exit("✗ --rootfs / --kernel / --firstaid のいずれかを指定してください。")
    lines += [
        "# 復旧メモ: 失敗時は libooter の setparam+saveparam で cmdline を戻す",
        '#   (例: root=/dev/mtdblock8 で firstaid 起動)。mtd0/mtd1 は無傷。',
        'echo "[done] 書き込み完了。reboot で確認。"',
    ]
    out = "\n".join(lines) + "\n"
    if a.outfile:
        with open(a.outfile, "w", encoding="utf-8", newline="\n") as f:
            f.write(out)
        print("✓ フラッシュスクリプト → %s" % a.outfile)
    else:
        print(out)


def cmd_probe(a):
    with open(a.dumpfile, "rb") as f:
        data = f.read(a.bytes)
    print("# %s 先頭 %dB（H1: RDHB ヘッダ解明の観察補助。判断は人間が行う）" % (a.dumpfile, len(data)))
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexs = " ".join("%02x" % x for x in chunk)
        text = "".join(chr(x) if 32 <= x < 127 else "." for x in chunk)
        print("%04x  %-47s  %s" % (off, hexs, text))
    for needle in (b"RDHB", b"PIX-", SQUASHFS_MAGIC):
        i = data.find(needle)
        print("  find %r: %s" % (needle, ("offset " + hex(i)) if i >= 0 else "見つからず"))


def cmd_full(a):
    sys.exit(
        "✗ HOLD(H2/H3): .full (Updater_Header + 本体) 生成は未実装。\n"
        "  ヘッダのフィールド名は既知（magic %c%c%c%c / type / size / version 0x%08x / buildNumber, spec §8.5）だが、\n"
        "  本体 Updater_decode の変換（暗号/難読化/素通し）と各フィールドの byte レイアウトが未解明。\n"
        "  そもそも確定経路（dione_server -w 生書き込み）では .full は不要。まずはそちらを使う。")


def main():
    ap = argparse.ArgumentParser(
        prog="openird-mkimage", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("partitions", help="区画表を表示")
    p.add_argument("board")
    p.set_defaults(fn=cmd_partitions)

    p = sub.add_parser("pack-rootfs", help="squashfs を rootfs 区画の像として検証・出力")
    p.add_argument("board")
    p.add_argument("infile")
    p.add_argument("outfile")
    p.add_argument("--mtd", type=int, default=6, help="対象区画（既定 6=rootfs。復旧用は 8=firstaid）")
    p.set_defaults(fn=cmd_pack_rootfs)

    p = sub.add_parser("pack-kernel", help="生カーネル Image を RDHB で包み mtd5 像を作る")
    p.add_argument("board")
    p.add_argument("infile", help="arch/arm/boot/Image（非圧縮）")
    p.add_argument("outfile")
    p.add_argument("--name", default=RDHB_NAME_DEFAULT, help="RDHB 名(既定=観察値)")
    p.add_argument("--load", help="load/entry アドレス(既定=board の zreladdr)")
    p.add_argument("--cksum", help="0x08 チェックサム(既定 0=HOLD H1b)")
    p.set_defaults(fn=cmd_pack_kernel)

    p = sub.add_parser("unpack-kernel", help="RDHB 像を検証・展開（生 Image 取り出し）")
    p.add_argument("infile")
    p.add_argument("outfile", nargs="?")
    p.set_defaults(fn=cmd_unpack_kernel)

    p = sub.add_parser("flash-script", help="on-device フラッシュ手順を生成")
    p.add_argument("board")
    p.add_argument("--rootfs")
    p.add_argument("--kernel")
    p.add_argument("--firstaid")
    p.add_argument("--via", choices=["mtd", "dione"], default="mtd",
                   help="mtd=OpenIRD の mtd ツール(既定) / dione=区画切り直し前のベンダ dione_server")
    p.add_argument("-o", "--outfile")
    p.set_defaults(fn=cmd_flash_script)

    p = sub.add_parser("probe-kernel-header", help="mtd5 ダンプ先頭を観察（H1 補助）")
    p.add_argument("dumpfile")
    p.add_argument("--bytes", type=int, default=256)
    p.set_defaults(fn=cmd_probe)

    p = sub.add_parser("full", help=".full コンテナ（HOLD）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_full)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
