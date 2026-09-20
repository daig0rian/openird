# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""区画イメージ（squashfs / uImage）の形式検査。

ボードのカーネルが読めない像を作ってしまう事故は、実機で mount / boot に失敗して
初めて分かる。ここで生成時に弾く（OpenWrt の check-size と同じ役割）。
"""
import struct
import sys

SQUASHFS_MAGIC = b"hsqs"          # squashfs LE (0x73717368)
SQUASHFS_MAGIC_BE = b"sqsh"       # 同 BE。3.x は両エンディアンの像が存在する
SQUASHFS_COMP = {1: "gzip", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}
SQUASHFS_INVALID_BLK = 0xFFFFFFFFFFFFFFFF
SQUASHFS_FLAG_COMP_OPT = 1 << 10

UIMAGE_MAGIC = 0x27051956
UIMAGE_HDR_LEN = 64
UIMAGE_OS = {5: "linux"}
UIMAGE_ARCH = {2: "arm", 5: "mips", 6: "mips64"}
UIMAGE_TYPE = {2: "kernel"}
UIMAGE_COMP = {0: "none", 1: "gzip", 2: "bzip2", 3: "lzma", 4: "lzo"}


def identify_squashfs(data):
    """先頭を見て squashfs の版とエンディアンを返す。読めなければ None。

    4.0 は LE 専用（mainline）。3.x は BE/LE 両方の像がある（SQUASHFS_SWAP_*）。
    """
    if len(data) < 96:
        return None
    if data[0:4] == SQUASHFS_MAGIC:
        major, minor = struct.unpack_from("<HH", data, 28)
        return {"endian": "le", "major": major, "minor": minor}
    if data[0:4] == SQUASHFS_MAGIC_BE:
        major, minor = struct.unpack_from(">HH", data, 28)
        return {"endian": "be", "major": major, "minor": minor}
    return None


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


def parse_uimage(data):
    """Legacy uImage ヘッダ 64B（常に BE）を読む。マジックが違えば None。"""
    if len(data) < UIMAGE_HDR_LEN:
        return None
    magic, hcrc, mtime, size, load, ep, dcrc = struct.unpack_from(">IIIIIII", data, 0)
    if magic != UIMAGE_MAGIC:
        return None
    os_, arch, typ, comp = data[28], data[29], data[30], data[31]
    return {
        "hcrc": hcrc, "mtime": mtime, "size": size, "load": load, "entry": ep, "dcrc": dcrc,
        "os": os_, "arch": arch, "type": typ, "comp": comp,
        "name": data[32:64].split(b"\x00", 1)[0].decode("ascii", "replace"),
    }
