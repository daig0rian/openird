# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""更新コンテナ <FAMILY>_UPDATE_v1b（emma3sep 系・UPnP factory 経路）の外側ヘッダ層。

仕様: docs/specs/update-v1b-container.md §1 / §2
実装はこの仕様だけを根拠にする（clean-room）。鍵・パスワードは持たない。

  0x000..0x14b  ヘッダ 332B（平文）
  0x14c..EOF    暗号本体（AES-128-ECB・長さは常に 16B の倍数）
"""
import hashlib
import io
import struct
import zipfile

from . import zipcrypto

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


# --- 暗号層（仕様 §3: AES-128-ECB） -----------------------------------------

def _ecb(key):
    try:
        from Crypto.Cipher import AES
    except ImportError:
        raise SystemExit(
            "✗ AES に pycryptodome が要ります。\n"
            "  `uv run openird_mkimage.py ...` で実行してください\n"
            "  （--no-project だと PEP 723 の依存が入りません）。")
    if len(key) != 16:
        raise ValueError("AES-128 の鍵は 16 バイトです（得: %d）" % len(key))
    return AES.new(key, AES.MODE_ECB)


def decrypt_body(body, key):
    """暗号本体 → 平文。ECB・パディング無し＝長さはそのまま（仕様 §3）。"""
    if len(body) % BODY_ALIGN:
        raise ValueError("本体長 %d が %dB 境界にありません" % (len(body), BODY_ALIGN))
    return _ecb(key).decrypt(body)


def encrypt_body(plain, key):
    """平文 → 暗号本体。16B 境界に合わせてから呼ぶこと（仕様 §6-5）。"""
    if len(plain) % BODY_ALIGN:
        raise ValueError("平文長 %d が %dB 境界にありません" % (len(plain), BODY_ALIGN))
    return _ecb(key).encrypt(plain)


# --- ZIP 層（仕様 §4 / §5） -------------------------------------------------

ZIP_MAGIC = b"PK\x03\x04"
CSUM_NAME = "csum"


def parse_csum(text):
    """`sha1sum -c` 互換 1 行 '<40hex>␠␠<name>' を (hexdigest, name) に分解する。"""
    line = text.strip().splitlines()[0] if text.strip() else ""
    parts = line.split(None, 1)
    if len(parts) != 2 or len(parts[0]) != 40:
        raise ValueError("csum の形式が読めません: %r" % line)
    return parts[0].lower(), parts[1].lstrip("*").strip()


EOCD_MAGIC = b"PK\x05\x06"
EOCD_LEN = 22


def zip_end(data):
    """ZIP 構造の末尾位置を返す（EOCD の終わり）。見つからなければ None。

    本体長は 16B 境界に揃える必要がある（仕様 §3）ので、ZIP の後ろに詰め物が
    入りうる。pack 側で何をどれだけ足すかを決めるための観察点。
    """
    i = data.rfind(EOCD_MAGIC)
    if i < 0 or len(data) < i + EOCD_LEN:
        return None
    comment_len = struct.unpack_from("<H", data, i + 20)[0]
    return i + EOCD_LEN + comment_len


def open_inner_zip(plain, password):
    """AES 復号直後の平文（= ZipCrypto 暗号化 ZIP）を開く。仕様 §4。"""
    if plain[:4] != ZIP_MAGIC:
        raise ValueError(
            "復号結果が ZIP ではありません（先頭 %r）。鍵が違う可能性があります。" % plain[:4])
    zf = zipfile.ZipFile(io.BytesIO(plain))
    zf.setpassword(password)
    return zf


def unpack(data, key, password):
    """コンテナを 3 段ほどいて、各段の成果物と検算結果を返す。

    外側ヘッダ → AES-128-ECB → ZipCrypto ZIP（<family>.zip と csum）
                                → 無暗号 ZIP（区画イメージとスクリプト）
    """
    hdr = parse(data)
    plain = decrypt_body(data[HDR_LEN:], key)
    zf = open_inner_zip(plain, password)

    names = zf.namelist()
    inner_names = [n for n in names if n != CSUM_NAME]
    if len(inner_names) != 1:
        raise ValueError("中間 ZIP のエントリが想定外です: %r（<family>.zip と csum の 2 つのはず）" % names)
    payload_name = inner_names[0]

    try:
        payload_zip = zf.read(payload_name)
    except RuntimeError as e:                      # 伝統的暗号のパスワード誤り
        raise ValueError("中間 ZIP を開けません（ZIP パスワードが違う可能性）: %s" % e)

    csum_text, csum_expect, csum_target = None, None, None
    if CSUM_NAME in names:
        csum_text = zf.read(CSUM_NAME).decode("ascii", "replace")
        csum_expect, csum_target = parse_csum(csum_text)

    payload_sha1 = hashlib.sha1(payload_zip).hexdigest()

    entries = []
    if payload_zip[:4] == ZIP_MAGIC:
        with zipfile.ZipFile(io.BytesIO(payload_zip)) as pz:
            for info in pz.infolist():
                entries.append({
                    "name": info.filename,
                    "size": info.file_size,
                    "compressed": info.compress_size,
                    "method": info.compress_type,
                    "crc": info.CRC,
                    "times": zipcrypto.dos_time(info.date_time),
                    "ext_attr": info.external_attr,
                    "mode": info.external_attr >> 16,
                    "data": pz.read(info.filename),
                })

    zend = zip_end(plain)
    return {
        "header": hdr,
        "plain": plain,
        "zip_end": zend,
        "trailing": None if zend is None else plain[zend:],
        "inner_names": names,
        "payload_name": payload_name,
        "payload_zip": payload_zip,
        "payload_sha1": payload_sha1,
        "csum_text": csum_text,
        "csum_expect": csum_expect,
        "csum_target": csum_target,
        "csum_ok": csum_expect is not None and csum_expect == payload_sha1,
        "csum_name_ok": csum_target == payload_name,
        "entries": entries,
    }


# --- 組み立て（仕様 §6） ----------------------------------------------------

def default_ext_attr(name):
    """機器上で `./write.sh` のように **実行される**スクリプトには実行ビットが要る。

    純正も .sh が 0755、区画イメージが 0644。ここを落とすと機器側の update.sh が
    `./write.sh` で Permission denied になり、更新が静かに止まる。
    """
    mode = zipcrypto.MODE_EXEC if name.endswith(".sh") else zipcrypto.MODE_FILE
    return zipcrypto.ext_attr_for(mode)


def build_payload_zip(entries, times=zipcrypto.DOS_EPOCH):
    """区画イメージとスクリプトを無暗号 ZIP にまとめる（仕様 §5）。

    純正は全エントリ deflate で、ディレクトリ・エントリを持たない。entries は
    unpack が返す形（name / data / method / times / ext_attr を持つ dict）でも、
    (name, data) の組でも良い。
    """
    items = []
    for e in entries:
        if isinstance(e, dict):
            items.append((e["name"], e["data"],
                          {"method": e.get("method", zipcrypto.DEFLATED),
                           "times": e.get("times", times),
                           "ext_attr": e.get("ext_attr") or default_ext_attr(e["name"])}))
        else:
            items.append((e[0], e[1], {"method": zipcrypto.DEFLATED, "times": times,
                                       "ext_attr": default_ext_attr(e[0])}))
    return zipcrypto.write_zip(items, password=None)


def build_csum(payload_name, payload_zip):
    """`sha1sum -c` 互換の 1 行（仕様 §4）。"""
    return "%s  %s\n" % (hashlib.sha1(payload_zip).hexdigest(), payload_name)


def build_middle_zip(payload_name, payload_zip, password,
                     times=zipcrypto.DOS_EPOCH, salt=None, vendor_style=True):
    """ペイロード ZIP と csum を ZipCrypto ZIP にまとめる（仕様 §4）。

    vendor_style=True は純正の形に合わせる: 無圧縮(store) ＋ データ記述子(bit 3)。
    純正の中間 ZIP は Info-ZIP の streaming 出力で、UT/Ux の extra field も持つが、
    あれは情報用で読み側は無視するため再現しない。
    """
    return zipcrypto.write_zip(
        [(payload_name, payload_zip), (CSUM_NAME, build_csum(payload_name, payload_zip).encode("ascii"))],
        password=password, method=zipcrypto.STORED, times=times, salt=salt,
        data_descriptor=vendor_style)


def pad_to_block(plain):
    """暗号本体は 16B の倍数でなければならない（仕様 §3）。0x00 を最小限だけ足す。"""
    return plain + b"\x00" * ((-len(plain)) % BODY_ALIGN)


def pack(family, version, plain, key, h20=b"\x00" * LEN_H20, h30=b"\x00" * LEN_H30):
    """中間 ZIP の平文からコンテナ 1 本を組む。plain は呼び出し側で詰め物済みにしておく。"""
    body = encrypt_body(plain, key)
    return build_header(family, version, body, h20, h30) + body


# --- OpenIRD 自身の factory イメージ（書き込み機構の仕様 poe-br100-yb1-nand-update §3.2） ---

FACTORY_MARKER = "openird-factory"      # ペイロードに入れる目印（導入後の片付けが見る）
UBOOT_CMD_MAX = 2048                    # ブートローダが 1 つの変数を実行できる上限


def factory_update_commands(images, staging, ymount, workdir):
    """ブートローダに実行させるコマンド列（`;` 区切りの前の並び）。

    images: [{"name", "write_offset", "write_len", "erase_len"}]（整数）。区画ごとに
    「RAM を 0xff で固定長ぶん埋める → 更新ファイルを読む → 消去 → 固定長を書く」。
    最後に更新用 env を消して再起動する（消さないと起動のたびに書き込みを繰り返す）。
    """
    cmds = ["echo ###### OpenIRD factory install #####", "ymount %x %x" % ymount]
    for im in images:
        cmds += ["mw.b %x ff %x" % (staging, im["write_len"]),
                 "yrdm %s/%s %x" % (workdir, im["name"], staging),
                 "nand erase %x %x" % (im["write_offset"], im["erase_len"]),
                 "nand write %x %x %x" % (staging, im["write_offset"], im["write_len"])]
    cmds += ["delenvu", "reset"]
    if len("update=" + ";".join(cmds)) >= UBOOT_CMD_MAX:
        raise ValueError("ブートローダのコマンド列が %dB の上限を超えます" % UBOOT_CMD_MAX)
    return cmds


def factory_write_script(model, images, staging, ymount, workdir, env_tool, min_blocks):
    """ペイロードの `write.sh` を生成する（純正からは流用しない）。

    機器の更新スクリプトが展開先を cwd にして `./write.sh` を実行する。ここでは区画を
    書かない: ブートローダのコマンド列を更新用 env として mtd1 に書くだけで、実際の
    消去と書き込みは次の起動でブートローダが行う。

    ymount: (開始, 終端)＝更新ファイルの置き場（yaffs2 区画）
    min_blocks: この区画表が成り立つ NAND の総量（512B 単位）。満たなければ何もしない。
    機器に出る文字列は ASCII のみ。
    """
    cmds = factory_update_commands(images, staging, ymount, workdir)
    lines = [
        "#!/bin/sh",
        "# OpenIRD factory image for %s." % model,
        "#",
        "# Run by the vendor updater in the directory the images were unpacked to.",
        "# Nothing is written to the flash partitions here: this stores a list of",
        "# boot loader commands in the update environment (mtd1), and the boot",
        "# loader erases and writes the kernel and root partitions on the next",
        "# start.  No other partition is touched.",
        "",
        "E=/tmp/u-boot_update_env.bin",
        "",
        "# The offsets below are those of the 128MB NAND layout.",
        "n=0",
        "for b in 0 1 2 3 4 5 6 7 8 9 10; do",
        "\tn=`expr $n + \\`cat /sys/block/mtdblock$b/size\\``",
        "done",
        "[ $n -ge %d ] || { echo 'OpenIRD: this image is for 128MB NAND devices'; exit 1; }" % min_blocks,
        "for f in %s; do" % " ".join(im["name"] for im in images),
        "\t[ -e $f ] || { echo \"OpenIRD: $f is missing\"; exit 1; }",
        "done",
        "",
        "rm -f $E",
        "echo -ne 'bootdelay=0\\0000' > $E",
        "echo -ne 'baudrate=115200\\0000' >> $E",
        "echo -ne 'bootcmd=run update\\0000' >> $E",
        "echo -ne 'update=' >> $E",
    ]
    for i, c in enumerate(cmds):
        end = "\\0000\\0000" if i == len(cmds) - 1 else ";"
        lines.append("echo -ne '%s%s' >> $E" % (c, end))
    lines += ["", "%s $E" % env_tool, "exit $?", ""]
    return "\n".join(lines).encode("ascii")
