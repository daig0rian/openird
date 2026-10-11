# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""RDHB カーネル容器（Fujitsu MB86H61 / SB-TV02-WFPL）。

仕様: docs/specs/rdhb-kernel-container.md
"""
import struct
import zlib

MAGIC = b"RDHB"
HDR_LEN = 0x4c            # 76B 固定
NAME_DEFAULT = "PIX-DT270-FW-Linux"   # 観察値。libooter が照合する可能性があり既定で複製
CONST = {0x04: 0xfffffffe, 0x20: 0, 0x24: 1, 0x28: 1, 0x2c: 1, 0x34: 1, 0x38: 0x301}


def build(image, load, name, cksum):
    """生カーネル Image → RDHB ラッパー。仕様どおり。"""
    payload = zlib.compress(image, 6)          # 78 9c + deflate + adler4
    total = HDR_LEN + len(payload)
    hdr = bytearray(b"\x00" * HDR_LEN)
    hdr[0x00:0x04] = MAGIC
    for off, v in CONST.items():
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


def inflate(img):
    """RDHB イメージのペイロードを展開して生 Image を返す（先頭 2B の 78 9c を飛ばし raw-inflate）。"""
    d = zlib.decompressobj(wbits=-15)
    return d.decompress(img[HDR_LEN + 2:]) + d.flush()


def fields(img):
    """ヘッダの数値フィールドを読む。"""
    g = lambda o: struct.unpack_from("<I", img, o)[0]
    return {
        "name": img[0x0c:0x24].split(b"\x00", 1)[0],
        "cksum": g(0x08), "load": g(0x44), "entry": g(0x48),
        "total_div4": g(0x30), "payload_div4": g(0x3c), "uncomp_div4": g(0x40),
    }
