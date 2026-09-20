# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""伝統的 PKWARE 暗号（ZipCrypto）付き ZIP の書き出し。

仕様 update-v1b-container.md §4 の中間 ZIP がこれ。Python の `zipfile` は
ZipCrypto を**復号しかできない**ので、書く側をここに持つ。

アルゴリズムと ZIP のレコード配置は PKWARE が公開している APPNOTE.TXT が出典で、
ベンダのコードとは無関係（`unzip -P` / `zip -P` が読み書きするのと同じ旧方式。
AES-ZIP ではない）。暗号強度は無いに等しいが、ここでの目的は純正の更新機構が
受け取れる形を作ることであって、秘匿ではない。

決定性: 12B のランダムヘッダと更新時刻を明示的に渡せるようにしてある。同じ入力から
同じバイト列を作れる＝再現ビルドできる。
"""
import binascii
import os
import struct
import zlib

STORED, DEFLATED = 0, 8
FLAG_ENCRYPTED = 1 << 0
HEADER_LEN = 12                       # 暗号ストリーム先頭のランダムヘッダ
_INIT = (0x12345678, 0x23456789, 0x34567890)
_MULT = 134775813                     # APPNOTE の定数


def _crc_table():
    tab = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0xEDB88320 if c & 1 else 0)
        tab.append(c)
    return tab


_TAB = _crc_table()


def _crc32_step(crc, byte):
    return (crc >> 8) ^ _TAB[(crc ^ byte) & 0xFF]


class _Keys:
    """APPNOTE の 3 本の鍵レジスタ。"""

    __slots__ = ("k0", "k1", "k2")

    def __init__(self, password):
        self.k0, self.k1, self.k2 = _INIT
        for b in password:
            self.update(b)

    def update(self, byte):
        self.k0 = _crc32_step(self.k0, byte)
        self.k1 = (self.k1 + (self.k0 & 0xFF)) & 0xFFFFFFFF
        self.k1 = (self.k1 * _MULT + 1) & 0xFFFFFFFF
        self.k2 = _crc32_step(self.k2, self.k1 >> 24)

    def stream_byte(self):
        t = (self.k2 | 2) & 0xFFFF
        return ((t * (t ^ 1)) >> 8) & 0xFF


def encrypt(plain, password):
    """平文 → 暗号文（ランダムヘッダは含まない）。"""
    keys = _Keys(password)
    out = bytearray(len(plain))
    for i, p in enumerate(plain):
        out[i] = p ^ keys.stream_byte()
        keys.update(p)
    return bytes(out)


def decrypt(cipher, password):
    """暗号文 → 平文（ランダムヘッダは含まない）。自前実装の検算用。"""
    keys = _Keys(password)
    out = bytearray(len(cipher))
    for i, c in enumerate(cipher):
        p = c ^ keys.stream_byte()
        out[i] = p
        keys.update(p)
    return bytes(out)


def encrypt_entry(data, password, check_byte, salt=None):
    """1 エントリ分の暗号ストリーム（12B ヘッダ ＋ 本体）を作る。

    ヘッダの最終バイトは復号側の早期チェックに使われる。値の出どころは 2 通りで、
    データ記述子（フラグ bit 3）を使わないなら **CRC32 の最上位バイト**、使うなら
    **DOS 更新時刻の上位バイト**。呼び出し側が選んで渡す。
    salt を渡すと決定的になる（11B 使う）。
    """
    if salt is None:
        salt = os.urandom(HEADER_LEN - 1)
    if len(salt) != HEADER_LEN - 1:
        raise ValueError("salt は %dB です" % (HEADER_LEN - 1))
    return encrypt(bytes(salt) + bytes([check_byte & 0xFF]) + data, password)


# --- 最小の ZIP ライター -----------------------------------------------------

_LFH = b"PK\x03\x04"
_CDH = b"PK\x01\x02"
_DD = b"PK\x07\x08"
_EOCD = b"PK\x05\x06"
DOS_EPOCH = (0x0000, 0x0021)          # 1980-01-01 00:00:00（日付 0 は不正なので 1 日）
FLAG_DATA_DESCRIPTOR = 1 << 3


def dos_time(date_time):
    """(年,月,日,時,分,秒) → DOS の (時刻, 日付) 2 語。"""
    y, mo, d, h, mi, s = date_time
    return (((h & 31) << 11) | ((mi & 63) << 5) | ((s // 2) & 31),
            (((y - 1980) & 127) << 9) | ((mo & 15) << 5) | (d & 31))


def _compress(data, method, level):
    if method == STORED:
        return data
    if method == DEFLATED:
        co = zlib.compressobj(level, zlib.DEFLATED, -zlib.MAX_WBITS)
        return co.compress(data) + co.flush()
    raise ValueError("未対応の圧縮方式: %d" % method)


# 「作成した版」の上位バイトがファイル属性の解釈を決める。3 = Unix。
# **これが Unix でないと、展開側は external attributes の Unix モードを見ない**
# ＝ 実行ビットが落ちる。機器上では展開したスクリプトを `./write.sh` で実行するので、
# ここを外すと Permission denied で更新が止まる。低位バイトは仕様版（3.0 = 30）。
MADE_BY_UNIX30 = (3 << 8) | 30
MODE_FILE = 0o100644
MODE_EXEC = 0o100755


def ext_attr_for(mode):
    """Unix のモード（0o100644 等）を ZIP の external attributes に詰める。"""
    return (mode & 0xFFFF) << 16


def write_zip(entries, password=None, method=STORED, level=6,
              times=DOS_EPOCH, salt=None, data_descriptor=False,
              version_made=MADE_BY_UNIX30, version_needed=None,
              ext_attr=ext_attr_for(MODE_FILE)):
    """(name, data) または (name, data, opts) の並びから ZIP を組む。

    password を渡すと ZipCrypto で暗号化する。opts で "method" / "times" /
    "ext_attr" をエントリごとに上書きできる（純正の並びを忠実に再現するため）。

    data_descriptor=True でフラグ bit 3 を立て、各エントリの後ろに記述子を書く。
    純正の中間 ZIP がこの形（Info-ZIP の streaming 出力）なので、再梱包では
    こちらに合わせる。この場合、暗号ヘッダの check byte は CRC ではなく
    **DOS 時刻の上位バイト**になる。
    """
    out = bytearray()
    central = bytearray()
    count = 0
    for item in entries:
        name, data = item[0], item[1]
        opts = item[2] if len(item) > 2 else {}
        mth = opts.get("method", method)
        dtime, ddate = opts.get("times", times)
        ea = opts.get("ext_attr", ext_attr)
        need = version_needed if version_needed is not None else (20 if mth == DEFLATED else 10)
        nm = name.encode("ascii")
        crc = binascii.crc32(data) & 0xFFFFFFFF
        comp = _compress(data, mth, level)
        flags = 0
        if data_descriptor:
            flags |= FLAG_DATA_DESCRIPTOR
        if password is not None:
            check = (dtime >> 8) & 0xFF if data_descriptor else (crc >> 24) & 0xFF
            comp = encrypt_entry(comp, password, check, salt)
            flags |= FLAG_ENCRYPTED
        offset = len(out)
        out += struct.pack("<4sHHHHHIIIHH", _LFH, need, flags, mth, dtime, ddate,
                           crc, len(comp), len(data), len(nm), 0) + nm + comp
        if data_descriptor:
            out += struct.pack("<4sIII", _DD, crc, len(comp), len(data))
        central += struct.pack("<4sHHHHHHIIIHHHHHII", _CDH, version_made, need, flags, mth,
                               dtime, ddate, crc, len(comp), len(data),
                               len(nm), 0, 0, 0, 0, ea, offset) + nm
        count += 1
    cd_off = len(out)
    out += central
    out += struct.pack("<4sHHHHIIH", _EOCD, 0, 0, count, count,
                       len(central), cd_off, 0)
    return bytes(out)
