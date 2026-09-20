# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""更新コンテナ <FAMILY>_UPDATE_v1b（emma3sep 系・UPnP factory 経路）の外側ヘッダ層。

仕様: docs/specs/update-v1b-container.md §1 / §2
実装はこの仕様だけを根拠にする（clean-room）。鍵・パスワードは持たない。

  0x000..0x14b  ヘッダ 332B（平文）
  0x14c..EOF    暗号本体（AES-128-ECB・長さは常に 16B の倍数）
"""
import hashlib
import struct

HDR_LEN = 0x14C           # 332B
TAG_LEN = 0x20            # 0x00: 族タグ 32B
TAG_SUFFIX = "_UPDATE_v1b"
OFF_H20, LEN_H20 = 0x20, 16
OFF_H30, LEN_H30 = 0x30, 4
OFF_VER, LEN_VER = 0x34, 4
OFF_H38, LEN_H38 = 0x38, 20
OFF_PAD, LEN_PAD = 0x4C, 0x100   # 0x4c..0x14b が 0x00 パディング
BODY_ALIGN = 16


class NotV1b(ValueError):
    pass


def _tag_text(raw):
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def parse(data):
    """コンテナ先頭を読み、ヘッダの各フィールドと自己検算の結果を返す。

    鍵は不要。h38（本体の SHA-1）は完全性用で署名ではないため、鍵なしで検算できる。
    """
    if len(data) < HDR_LEN + BODY_ALIGN:
        raise NotV1b("短すぎます（%dB）" % len(data))
    tag_raw = data[0:TAG_LEN]
    tag = _tag_text(tag_raw)
    if not tag.endswith(TAG_SUFFIX):
        raise NotV1b("族タグが %r 形式ではありません（得: %r）" % ("<FAMILY>" + TAG_SUFFIX, tag))
    body = data[HDR_LEN:]
    ver = data[OFF_VER:OFF_VER + LEN_VER]
    return {
        "tag": tag,
        "family": tag[:-len(TAG_SUFFIX)],
        "tag_nul_padded": tag_raw[len(tag):] == b"\x00" * (TAG_LEN - len(tag)),
        "h20": data[OFF_H20:OFF_H20 + LEN_H20],
        "h30": data[OFF_H30:OFF_H30 + LEN_H30],
        "version_raw": ver,
        "version": "%d.%d.%d" % (ver[0], ver[1], ver[2]),
        "version_tail_zero": ver[3] == 0,
        "h38": data[OFF_H38:OFF_H38 + LEN_H38],
        "pad_zero": data[OFF_PAD:OFF_PAD + LEN_PAD] == b"\x00" * LEN_PAD,
        "body_len": len(body),
        "body_aligned": len(body) % BODY_ALIGN == 0,
        "body_sha1": hashlib.sha1(body).digest(),
    }


def sha1_ok(hdr):
    """0x38 の h38 が本体の SHA-1 と一致するか（仕様 §2: 確定・27 本全数で検算一致）。"""
    return hdr["h38"] == hdr["body_sha1"]


def checks(hdr):
    """仕様どおりかの検査項目。(名前, 真偽) の並びで返す。"""
    return [
        ("h38 = SHA-1(body)", sha1_ok(hdr)),
        ("body が 16B の倍数", hdr["body_aligned"]),
        ("0x4c..0x14b が 0 埋め", hdr["pad_zero"]),
        ("族タグが NUL 詰め", hdr["tag_nul_padded"]),
        ("version の 4 バイト目が 0", hdr["version_tail_zero"]),
    ]


def build_header(family, version, body, h20=b"\x00" * LEN_H20, h30=b"\x00" * LEN_H30):
    """ヘッダ 332B を組む（仕様 §6-6）。h38 は本体から計算するので最後で良い。

    version は (MM, mm, pp) の三つ組。h20/h30 の算出式は未解明（仕様 §7 U1/U2）なので
    呼び出し側が明示的に渡す。既定のゼロは「不明なまま置く」という意思表示。
    """
    if len(body) % BODY_ALIGN:
        raise ValueError("本体長 %d が %dB 境界にありません" % (len(body), BODY_ALIGN))
    if len(h20) != LEN_H20 or len(h30) != LEN_H30:
        raise ValueError("h20/h30 の長さが違います")
    mm, mi, pp = version
    tag = (family + TAG_SUFFIX).encode("ascii")
    if len(tag) >= TAG_LEN:
        raise ValueError("族タグが長すぎます: %r" % tag)
    hdr = bytearray(HDR_LEN)
    hdr[0:len(tag)] = tag
    hdr[OFF_H20:OFF_H20 + LEN_H20] = h20
    hdr[OFF_H30:OFF_H30 + LEN_H30] = h30
    hdr[OFF_VER:OFF_VER + LEN_VER] = struct.pack("BBBB", mm, mi, pp, 0)
    hdr[OFF_H38:OFF_H38 + LEN_H38] = hashlib.sha1(body).digest()
    return bytes(hdr)
