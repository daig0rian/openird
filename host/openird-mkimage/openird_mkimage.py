#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.9"
# dependencies = ["pycryptodome>=3.20"]
# ///
"""openird-mkimage - OpenIRD install-image ツール（ボード別バックエンド）。

現状の実装範囲:
  mb86h61 / sb-tv02-wfpl   : 確定経路（booter-root から MTD を直接書く。暗号ゲート無し）。
  emma3sep / poe-br100-yb1 : 純正の更新コンテナ <FAMILY>_UPDATE_v1b を使う経路。
                             probe-factory（鍵不要の観察）まで実装済み。unpack/pack は未実装。

不明点（HOLD）は推測で埋めない。refuse して理由を示す。詳細は
NOTES-sb-tv02-factory.md / NOTES-poe-br100-yb1-factory.md を参照。
配布物にベンダバイナリ/鍵を含めない。

実行:
  uv run openird_mkimage.py <cmd> ...                       # 依存を解決する（v1b の復号に必要）
  uv run --no-project python openird_mkimage.py <cmd> ...   # 依存不要のコマンドのみ
"""
import sys
import os
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ird import images, rdhb, v1b                       # noqa: E402
from ird.board import load_board, part_by_mtd, h        # noqa: E402

for _s in (sys.stdout, sys.stderr):            # Windows cp932 既定を回避
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8")


def cmd_partitions(a):
    b = load_board(a.board)
    print("# %s  (%s)  NVM %s  erase %s"
          % (b["board"], b["soc"], b["nvm"]["size"], b["nvm"]["erasesize"]))
    print("%3s %-16s %10s %10s %-9s note" % ("mtd", "name", "offset", "size", "policy"))
    for p in b["partitions"]:
        print("%3d %-16s %10s %10s %-9s %s"
              % (p["mtd"], p["name"], p["offset"], p["size"], p["policy"], p.get("note", "")))


def cmd_pack_rootfs(a):
    b = load_board(a.board)
    part = part_by_mtd(b, a.mtd)
    if part is None:
        sys.exit("✗ mtd%d はこのボードに存在しない。" % a.mtd)
    if part["policy"] != "writable":
        sys.exit("✗ mtd%d(%s) は policy=%s＝書き込み禁止。" % (a.mtd, part["name"], part["policy"]))
    with open(a.infile, "rb") as f:
        data = f.read()
    if data[:4] != images.SQUASHFS_MAGIC:
        sys.exit("✗ squashfs マジック 'hsqs' がありません（得: %r）。" % data[:4])
    images.check_squashfs(data, b["squashfs"])
    cap = h(part["size"])
    if len(data) > cap:
        sys.exit("✗ %dB > mtd%d 容量 %dB。" % (len(data), a.mtd, cap))
    with open(a.outfile, "wb") as f:
        f.write(data)
    print("✓ rootfs OK: %dB / %dB (%d%%) mtd%d(%s) → %s"
          % (len(data), cap, 100 * len(data) // cap, a.mtd, part["name"], a.outfile))
    print("  → 書き込み: mtd write <file> /dev/mtd%d（区画を切り直した後）" % a.mtd)


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
    img = rdhb.build(image, load, a.name, cksum)
    if len(img) > cap:
        sys.exit("✗ RDHB 像 %dB > mtd5 容量 %dB。" % (len(img), cap))
    # 自己検証: 生成物を展開し直して元 Image と一致するか
    if rdhb.inflate(img) != image:
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
    if img[:4] != rdhb.MAGIC:
        sys.exit("✗ RDHB マジックがありません（得: %r）。" % img[:4])
    fl = rdhb.fields(img)
    print("# RDHB ヘッダ")
    print("  name=%r  load=0x%08x entry=0x%08x  cksum(0x08)=0x%08x"
          % (fl["name"], fl["load"], fl["entry"], fl["cksum"]))
    print("  size語: total/4=0x%x payload/4=0x%x uncomp/4=0x%x"
          % (fl["total_div4"], fl["payload_div4"], fl["uncomp_div4"]))
    out = rdhb.inflate(img)
    print("  展開後: %dB (0x40*4=%dB, %s)"
          % (len(out), fl["uncomp_div4"] * 4,
             "一致" if len(out) == fl["uncomp_div4"] * 4 else "不一致"))
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
    for needle in (b"RDHB", b"PIX-", images.SQUASHFS_MAGIC):
        i = data.find(needle)
        print("  find %r: %s" % (needle, ("offset " + hex(i)) if i >= 0 else "見つからず"))


# --- emma3sep / v1b ---------------------------------------------------------

def _probe_one(path, verbose):
    with open(path, "rb") as f:
        data = f.read()
    try:
        hdr = v1b.parse(data)
    except v1b.NotV1b as e:
        print("✗ %s: %s" % (os.path.basename(path), e))
        return False
    ck = v1b.checks(hdr)
    ok = all(v for _, v in ck)
    if not verbose:
        print("%-42s %-10s %-8s %11d  %s"
              % (os.path.basename(path), hdr["family"], hdr["version"], len(data),
                 "OK" if ok else "✗ " + ", ".join(n for n, v in ck if not v)))
        return ok
    print("# %s  (%d B)" % (path, len(data)))
    print("  族タグ     : %-24s (族 %r)" % (hdr["tag"], hdr["family"]))
    print("  version    : %-24s (0x34: %s)"
          % (hdr["version"], " ".join("%02x" % x for x in hdr["version_raw"])))
    print("  h20 (0x20) : %s  ※推定: 平文 MD5・未検証 (仕様 §7 U1)" % hdr["h20"].hex())
    print("  h30 (0x30) : %s  ※推定: 平文 CRC32 系・未検証 (仕様 §7 U2)" % hdr["h30"].hex())
    print("  h38 (0x38) : %s" % hdr["h38"].hex())
    print("  本体       : %d B  (0x14c..EOF)" % hdr["body_len"])
    for name, v in ck:
        print("  %s %s" % ("✓" if v else "✗", name))
    if not v1b.sha1_ok(hdr):
        print("    実測 SHA-1(body) = %s" % hdr["body_sha1"].hex())
    return ok


def cmd_probe_factory(a):
    verbose = len(a.files) == 1 and not a.table
    if not verbose:
        print("%-42s %-10s %-8s %11s  %s" % ("file", "family", "version", "size", "checks"))
    bad = 0
    for p in a.files:
        if not _probe_one(p, verbose):
            bad += 1
    if not verbose:
        print("\n%d 本中 %d 本が仕様どおり。" % (len(a.files), len(a.files) - bad))
    if bad:
        sys.exit(1)


def cmd_unpack_factory(a):
    sys.exit(
        "✗ 未実装（P1）: v1b の 3 段復号（AES-128-ECB → ZipCrypto ZIP → 無暗号 ZIP）。\n"
        "  鍵とパスワードは所有者が自機から回収した値をファイルで渡す設計にする（仕様 §0）。\n"
        "  鍵不要の観察は probe-factory を使うこと。")


def cmd_pack_factory(a):
    sys.exit(
        "✗ 未実装（P2）: unpack が先。理由は NOTES-poe-br100-yb1-factory.md の V1。\n"
        "  ペイロードの write.sh 系は純正から流用せず仕様どおり自前生成する必要があり（仕様 §6-2）、\n"
        "  その呼び出し契約がまだ仕様化されていない。")


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
    p.add_argument("--name", default=rdhb.NAME_DEFAULT, help="RDHB 名(既定=観察値)")
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

    p = sub.add_parser("probe-factory", help="v1b コンテナのヘッダを読む（鍵不要）")
    p.add_argument("files", nargs="+")
    p.add_argument("--table", action="store_true", help="1 本でも表形式で出す")
    p.set_defaults(fn=cmd_probe_factory)

    p = sub.add_parser("unpack-factory", help="v1b コンテナを 3 段復号（未実装）")
    p.add_argument("infile")
    p.add_argument("outdir")
    p.set_defaults(fn=cmd_unpack_factory)

    p = sub.add_parser("pack-factory", help="v1b factory コンテナを作る（未実装）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_pack_factory)

    p = sub.add_parser("full", help=".full コンテナ（HOLD）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_full)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
