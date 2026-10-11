# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""区画イメージ（squashfs / uImage）の形式検査。

ボードのカーネルが読めないイメージを作ってしまう事故は、実機で mount / boot に失敗して
初めて分かる。ここで生成時に弾く（OpenWrt の check-size と同じ役割）。
"""
import struct
import sys
import zlib

SQUASHFS_MAGIC = b"hsqs"         # squashfs LE (0x73717368)
SQUASHFS_MAGIC_BE = b"sqsh"       # 同 BE。3.x は両エンディアンのイメージが存在する
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

    4.0 は LE 専用（mainline）。3.x は BE/LE 両方のイメージがある（SQUASHFS_SWAP_*）。
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


def gzip_deterministic(raw):
    """gzip -9 相当。ヘッダの時刻・ファイル名を持たせない（同じ入力から同じイメージ）。"""
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    body = co.compress(raw) + co.flush()
    hdr = b"\x1f\x8b\x08\x00" + b"\x00\x00\x00\x00" + b"\x02\x03"    # mtime 0 / XFL=最大圧縮 / OS=Unix
    return hdr + body + struct.pack("<II", zlib.crc32(raw) & 0xFFFFFFFF, len(raw) & 0xFFFFFFFF)


def build_uimage(data, spec, load, entry, mtime=0):
    """Legacy uImage（ヘッダ 64B・常に BE ＋ データ）を作る。

    spec = board["uimage"]: os / arch / type / comp / name（ブートローダが食う契約）。
    data は comp で指定した形式に圧縮済みのもの。"""
    name = spec["name"].encode("ascii")
    if len(name) > 32:
        sys.exit("✗ uImage の名前が 32B を超えます: %r" % spec["name"])
    hdr = bytearray(UIMAGE_HDR_LEN)
    struct.pack_into(">IIIIIII", hdr, 0, UIMAGE_MAGIC, 0, mtime, len(data), load, entry,
                     zlib.crc32(data) & 0xFFFFFFFF)
    hdr[28:32] = bytes([spec["os"], spec["arch"], spec["type"], spec["comp"]])
    hdr[32:32 + len(name)] = name
    struct.pack_into(">I", hdr, 4, zlib.crc32(bytes(hdr)) & 0xFFFFFFFF)   # hcrc は 0 を置いて計算
    return bytes(hdr) + data


def check_uimage(img):
    """ヘッダ CRC とデータ CRC を検算して parse_uimage の結果を返す。合わなければ None。"""
    u = parse_uimage(img)
    if u is None or len(img) < UIMAGE_HDR_LEN + u["size"]:
        return None
    hdr = bytearray(img[:UIMAGE_HDR_LEN])
    hdr[4:8] = b"\x00\x00\x00\x00"
    if zlib.crc32(bytes(hdr)) & 0xFFFFFFFF != u["hcrc"]:
        return None
    if zlib.crc32(img[UIMAGE_HDR_LEN:UIMAGE_HDR_LEN + u["size"]]) & 0xFFFFFFFF != u["dcrc"]:
        return None
    return u


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
