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
                             probe / unpack / repack まで実装済み。OpenIRD 自身の
                             image を作る pack-factory は未実装。

不明点（HOLD）は推測で埋めない。refuse して理由を示す。詳細は
NOTES-sb-tv02-factory.md / NOTES-poe-br100-yb1-factory.md を参照。
配布物にベンダバイナリ/鍵を含めない。

実行:
  uv run openird_mkimage.py <cmd> ...                       # 依存を解決する（v1b の復号に必要）
  uv run --no-project python openird_mkimage.py <cmd> ...   # 依存不要のコマンドのみ
"""
import sys
import os
import json
import zlib
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ird import images, rdhb, secrets, v1b              # noqa: E402
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
    cap = h(part["size"])
    want = b["squashfs"].get("vendor_format")
    if want:
        # ベンダカーネルの squashfs が mainline 以前の版（emma3sep: 3.1 BE）。
        # 中身の検査は版とバイト順まで。上限は区画ではなく factory の書き込み固定長
        # （それを超えた分は更新時に黙って切り捨てられる）。
        got = images.identify_squashfs(data)
        if got is None:
            sys.exit("✗ squashfs のマジックがありません（得: %r）。" % data[:4])
        if (got["major"], got["minor"], got["endian"]) != (want["major"], want["minor"], want["endian"]):
            sys.exit("✗ squashfs %d.%d %s。このボードのカーネルが読むのは %d.%d %s だけです。"
                     % (got["major"], got["minor"], got["endian"].upper(),
                        want["major"], want["minor"], want["endian"].upper()))
        print("  squashfs %d.%d %s" % (got["major"], got["minor"], got["endian"].upper()))
        im = next((i for i in b.get("factory", {}).get("images", []) if i["mtd"] == a.mtd), None)
        if im:
            cap = h(im["write_len"])
    else:
        if data[:4] != images.SQUASHFS_MAGIC:
            sys.exit("✗ squashfs マジック 'hsqs' がありません（得: %r）。" % data[:4])
        images.check_squashfs(data, b["squashfs"])
    if len(data) > cap:
        sys.exit("✗ %dB > mtd%d に書ける %dB。" % (len(data), a.mtd, cap))
    with open(a.outfile, "wb") as f:
        f.write(data)
    print("✓ rootfs OK: %dB / %dB (%d%%) mtd%d(%s) → %s"
          % (len(data), cap, 100 * len(data) // cap, a.mtd, part["name"], a.outfile))
    if not want:
        print("  → 書き込み: mtd write <file> /dev/mtd%d（区画を切り直した後）" % a.mtd)


def _check_uimage_fits_decompress(spec, img, load):
    """このボードのブートローダは NAND の uImage を nboot_load に置き、gunzip で load へ
    展開する。uImage が load までに収まらないと、展開の出力が自分の未読の圧縮ソースを
    上書きして起動しない（CRC は通るので気づきにくい・実機でブリックを踏んだ）。
    ここで生成時に弾く。"""
    nboot = spec.get("nboot_load")
    if not nboot:
        return
    budget = load - h(nboot)
    if len(img) > budget:
        sys.exit(
            "✗ uImage %d B > ブートローダの展開余裕 %d B（load 0x%08x - nboot_load %s）。\n"
            "  この境界を超えると gunzip が自分の未読ソースを潰して起動しません（CRC は通る）。\n"
            "  カーネルを小さく（埋め込み initramfs を外す等）してください。"
            % (len(img), budget, load, nboot))


def _factory_image(b, name):
    for im in b.get("factory", {}).get("images", []):
        if im["name"] == name:
            return im
    return None


def _pack_kernel_uimage(a, b):
    """生の vmlinux.bin → gzip → Legacy uImage（emma3sep。契約は board["uimage"]）。"""
    spec = b["uimage"]
    if not a.entry:
        sys.exit("✗ --entry が要ります（System.map の kernel_entry。ビルドごとに変わる）。")
    # System.map は 32bit カーネルでも符号拡張した 16 桁で出ることがある（ffffffff8228...）
    load = (h(a.load) if a.load else h(spec["load"])) & 0xFFFFFFFF
    entry = h(a.entry) & 0xFFFFFFFF
    with open(a.infile, "rb") as f:
        raw = f.read()
    if raw[:2] == b"\x1f\x8b" or raw[:4] == b"\x7fELF":
        sys.exit("✗ 入力は objcopy -O binary した *非圧縮* の vmlinux.bin を渡してください。")
    if not (load <= entry < load + len(raw)):
        sys.exit("✗ entry 0x%08x が像の範囲 0x%08x+%dB の外です。" % (entry, load, len(raw)))
    img = images.build_uimage(images.gzip_deterministic(raw), spec, load, entry)
    im = _factory_image(b, "vmlinux.img")
    cap = h(im["write_len"]) if im else h(part_by_mtd(b, 2)["size"])
    if len(img) > cap:
        sys.exit("✗ uImage %dB > 書き込み固定長 %dB（超過分は黙って切り捨てられる）。" % (len(img), cap))
    _check_uimage_fits_decompress(spec, img, load)
    u = images.check_uimage(img)
    if u is None or zlib.decompress(img[images.UIMAGE_HDR_LEN:], 31) != raw:
        sys.exit("✗ 内部エラー: 生成した uImage を読み直せません。")
    with open(a.outfile, "wb") as f:
        f.write(img)
    print("✓ uImage: %dB (%d%% of %dB) → %s" % (len(img), 100 * len(img) // cap, cap, a.outfile))
    print("  name=%r load=0x%08x entry=0x%08x  展開後=%dB" % (u["name"], load, entry, len(raw)))


def cmd_pack_kernel(a):
    b = load_board(a.board)
    if "uimage" in b:
        return _pack_kernel_uimage(a, b)
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


ZIP_METHOD = {0: "store", 8: "deflate", 9: "deflate64", 12: "bzip2", 14: "lzma"}
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IGNORED_DIRS = ("temp", "output", "build")


def _guard_outdir(outdir):
    """復号結果はベンダ由来の素材。リポジトリの追跡対象に落とさせない。"""
    full = os.path.abspath(outdir)
    try:
        rel = os.path.relpath(full, REPO_ROOT)
    except ValueError:
        return                                   # 別ドライブ＝リポジトリ外
    if rel.startswith(os.pardir):
        return                                   # リポジトリ外
    top = rel.replace("\\", "/").split("/", 1)[0]
    if top not in IGNORED_DIRS:
        sys.exit(
            "✗ 出力先がリポジトリの中です: %s\n"
            "  復号結果はベンダ由来の解析素材で、commit してはいけません（CONTRIBUTING.md §0）。\n"
            "  リポジトリ外か、git 管理外の %s のどれかに出してください。"
            % (full, " / ".join(d + "/" for d in IGNORED_DIRS)))


def cmd_unpack_factory(a):
    _guard_outdir(a.outdir)
    key, key_path = secrets.load_key(a.key_file)
    pw, pw_path = secrets.load_zip_password(a.zip_pw_file)
    with open(a.infile, "rb") as f:
        data = f.read()

    print("# %s  (%d B)" % (a.infile, len(data)))
    secrets.report("AES 鍵       ", key, key_path)
    secrets.report("ZIP パスワード", pw, pw_path)

    try:
        r = v1b.unpack(data, key, pw)
    except (ValueError, v1b.NotV1b) as e:
        sys.exit("✗ %s" % e)

    hdr = r["header"]
    print("\n## 段 0: 外側ヘッダ")
    print("  族 %s / version %s / 本体 %d B" % (hdr["family"], hdr["version"], hdr["body_len"]))
    for name, ok in v1b.checks(hdr):
        print("  %s %s" % ("✓" if ok else "✗", name))

    print("\n## 段 1: AES-128-ECB 復号 → 中間 ZIP（ZipCrypto）")
    print("  平文 %d B  先頭 %r" % (len(r["plain"]), r["plain"][:4]))
    print("  エントリ: %s" % ", ".join(r["inner_names"]))
    tr = r["trailing"]
    if tr is None:
        print("  ⚠ EOCD が見つかりません（ZIP 構造の末尾を特定できない）")
    else:
        kinds = sorted(set(tr))
        print("  ZIP 末尾 0x%x → 後ろに %d B の詰め物%s  (pack 側で再現する対象)"
              % (r["zip_end"], len(tr),
                 ("・中身 " + " ".join("%02x" % b for b in kinds[:4])) if tr else ""))

    print("\n## 段 2: csum 検算")
    if r["csum_text"] is None:
        print("  ✗ csum がありません（仕様 §4 ではエントリは正確に 2 つ）")
    else:
        print("  csum: %r" % r["csum_text"].strip())
        print("  %s SHA-1(%s) = %s" % ("✓" if r["csum_ok"] else "✗",
                                       r["payload_name"], r["payload_sha1"]))
        print("  %s csum のファイル名がペイロードと一致" % ("✓" if r["csum_name_ok"] else "✗"))

    print("\n## 段 3: ペイロード ZIP（無暗号）")
    print("  %-20s %10s %10s %-9s %s" % ("name", "size", "stored", "method", "crc"))
    for e in r["entries"]:
        print("  %-20s %10d %10d %-9s %08x"
              % (e["name"], e["size"], e["compressed"],
                 ZIP_METHOD.get(e["method"], "id%d" % e["method"]), e["crc"]))
    methods = sorted({ZIP_METHOD.get(e["method"], "id%d" % e["method"]) for e in r["entries"]})
    print("  → 圧縮方式: %s  (仕様の V2＝pack 側で合わせる対象)" % ", ".join(methods) if methods else "")

    # 書き出し
    os.makedirs(a.outdir, exist_ok=True)
    stage1 = os.path.join(a.outdir, "stage1-inner.zip")
    with open(stage1, "wb") as f:
        f.write(r["plain"])
    s2 = os.path.join(a.outdir, "stage2")
    os.makedirs(s2, exist_ok=True)
    with open(os.path.join(s2, r["payload_name"]), "wb") as f:
        f.write(r["payload_zip"])
    if r["csum_text"] is not None:
        with open(os.path.join(s2, v1b.CSUM_NAME), "w", encoding="ascii", newline="") as f:
            f.write(r["csum_text"])
    pdir = os.path.join(a.outdir, "payload")
    os.makedirs(pdir, exist_ok=True)
    for e in r["entries"]:
        safe = os.path.basename(e["name"])       # ZIP のパスは信用しない
        with open(os.path.join(pdir, safe), "wb") as f:
            f.write(e["data"])
    print("\n✓ 出力: %s" % os.path.abspath(a.outdir))
    print("    stage1-inner.zip   AES 復号結果（ZipCrypto ZIP）")
    print("    stage2/            中間 ZIP の中身（%s と csum）" % r["payload_name"])
    print("    payload/           区画イメージとスクリプト %d 点" % len(r["entries"]))

    bad = [n for n, ok in v1b.checks(hdr) if not ok]
    if bad or (r["csum_text"] is not None and not r["csum_ok"]):
        sys.exit(1)


def _hexarg(text, want, what):
    """16 進文字列を指定バイト数で読む。`~` 前置で原本の値のビット反転（実験用）。"""
    t = text.replace(" ", "").replace(":", "")
    try:
        b = bytes.fromhex(t)
    except ValueError:
        sys.exit("✗ %s は 16 進で指定してください: %r" % (what, text))
    if len(b) != want:
        sys.exit("✗ %s は %d バイト（16 進 %d 文字）です（得: %d）"
                 % (what, want, want * 2, len(b)))
    return b


def _board_for_family(family):
    """族タグから boards/*.json を引く（書き込み固定長の検査に使う）。"""
    import glob as _glob
    for path in _glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), "boards", "*.json")):
        try:
            with open(path, encoding="utf-8") as f:
                b = json.load(f)
        except (OSError, ValueError):
            continue
        if b.get("factory", {}).get("family_tag") == family:
            return b
    return None


def _apply_replacements(r, specs, family):
    """--replace name=path をペイロードに反映する。OpenWrt の check-size 相当を掛ける。"""
    if not specs:
        return []
    board = _board_for_family(family)
    limits = {}
    if board:
        limits = {im["name"]: (h(im["write_len"]), im["mtd"])
                  for im in board["factory"]["images"]}
        print("\n## 差し替え（ボード定義 %s で上限を検査）" % board["board"])
    else:
        print("\n## 差し替え（族 %r に対応するボード定義が無く、上限を検査できません）" % family)
    by_name = {e["name"]: e for e in r["entries"]}
    done = []
    for spec in specs:
        if "=" not in spec:
            sys.exit("✗ --replace は name=path の形で指定してください: %r" % spec)
        name, path = spec.split("=", 1)
        if name not in by_name:
            sys.exit("✗ ペイロードに %r がありません（あるのは %s）"
                     % (name, ", ".join(by_name)))
        with open(path, "rb") as f:
            data = f.read()
        old = len(by_name[name]["data"])
        if name in limits:
            cap, mtd = limits[name]
            if len(data) > cap:
                sys.exit("✗ %s が mtd%d の書き込み長を超えています: %d B > %d B。\n"
                         "  超過分は機器側で黙って切り捨てられ、区画後半が壊れます。"
                         % (name, mtd, len(data), cap))
            print("  %-18s %d B → %d B  (mtd%d 上限 %d B の %d%%)"
                  % (name, old, len(data), mtd, cap, 100 * len(data) // cap))
        else:
            print("  %-18s %d B → %d B" % (name, old, len(data)))
        by_name[name]["data"] = data
        done.append(name)
    return done


def cmd_repack_factory(a):
    """純正コンテナをほどいて、中身を変えずに組み直す（V8 の初弾）。

    --rebuild で「どこまで自前で組み直すか」を選ぶ。段を 1 つずつ増やせるので、
    受理されなかったときにどの層が原因かを切り分けられる。
    """
    key, key_path = secrets.load_key(a.key_file)
    pw, pw_path = secrets.load_zip_password(a.zip_pw_file)
    with open(a.infile, "rb") as f:
        orig = f.read()
    print("# %s  (%d B)" % (a.infile, len(orig)))
    secrets.report("AES 鍵       ", key, key_path)
    secrets.report("ZIP パスワード", pw, pw_path)

    try:
        r = v1b.unpack(orig, key, pw)
    except (ValueError, v1b.NotV1b) as e:
        sys.exit("✗ %s" % e)
    hdr = r["header"]
    if not v1b.sha1_ok(hdr):
        sys.exit("✗ 元コンテナの h38 が合いません。壊れた入力は組み直さない。")

    before = {e["name"]: e["data"] for e in r["entries"]}     # 差し替え前の原本
    replaced = _apply_replacements(r, a.replace, hdr["family"])
    if replaced and a.rebuild != "all":
        sys.exit("✗ --replace はペイロードを変えるので --rebuild all でしか使えません。")

    # ZipCrypto の 12B ヘッダは通常ランダム。再現ビルドのため既定では固定値を使う。
    salt = None if a.random_salt else bytes.fromhex(a.salt)

    if a.rebuild == "none":
        plain = r["plain"]
        note = "平文をそのまま再暗号化（ヘッダと AES 層だけを自前で組む）"
    elif a.rebuild == "zipcrypto":
        mid = v1b.build_middle_zip(r["payload_name"], r["payload_zip"], pw,
                                   times=a_times(r), salt=salt,
                                   vendor_style=not a.plain_zip)
        plain = v1b.pad_to_block(mid)
        note = "ペイロード ZIP は原本のまま、中間 ZIP から自前で組む"
    else:
        payload = v1b.build_payload_zip(r["entries"])
        mid = v1b.build_middle_zip(r["payload_name"], payload, pw,
                                   times=a_times(r), salt=salt,
                                   vendor_style=not a.plain_zip)
        plain = v1b.pad_to_block(mid)
        note = "ペイロード ZIP から全段を自前で組む（本番と同じ経路）"

    # h20/h30 は既定で原本から複写する（算出式が未解明のため・仕様 §7 U1/U2）。
    # 明示指定は仕様 §7 U3 の実験用＝「判定に使われているのか」を実機で決めるため。
    h20 = _hexarg(a.h20, v1b.LEN_H20, "--h20") if a.h20 else hdr["h20"]
    h30 = _hexarg(a.h30, v1b.LEN_H30, "--h30") if a.h30 else hdr["h30"]
    out = v1b.pack(hdr["family"], tuple(hdr["version_raw"][:3]), plain, key,
                   h20=h20, h30=h30)

    print("\n## 組み直し: %s" % note)
    print("  平文 %d B（原本 %d B / 差 %+d）" % (len(plain), len(r["plain"]), len(plain) - len(r["plain"])))
    print("  出力 %d B（原本 %d B / 差 %+d）" % (len(out), len(orig), len(out) - len(orig)))
    if a.h20 or a.h30:
        print("  ⚠ h20/h30 を原本から変えています（仕様 §7 U3 の実験）")
        print("     h20: %s → %s" % (hdr["h20"].hex(), h20.hex()))
        print("     h30: %s → %s" % (hdr["h30"].hex(), h30.hex()))
    else:
        print("  h20/h30 は原本から複写（算出式は未解明・仕様 §7 U1/U2）")

    identical = out == orig
    if a.rebuild == "none":
        print("  %s 原本とバイト一致" % ("✓" if identical else "✗"))
        if not identical:
            n = sum(1 for x, y in zip(out, orig) if x != y)
            print("     先頭の相違位置: %s / 相違バイト数 %d"
                  % (next((hex(i) for i, (x, y) in enumerate(zip(out, orig)) if x != y), "-"), n))
    elif identical:
        print("  ✓ 原本とバイト一致（ZIP の組み方まで純正と同じだった）")

    # 自己検証: 出したものを読み直す
    try:
        v = v1b.unpack(out, key, pw)
    except (ValueError, v1b.NotV1b) as e:
        sys.exit("✗ 生成物を読み直せません: %s" % e)
    got = {e["name"]: e["data"] for e in v["entries"]}
    want = {e["name"]: e["data"] for e in r["entries"]}
    untouched = [n for n in before if n not in replaced]
    checks = [
        ("生成物の h38 が合う", v1b.sha1_ok(v["header"])),
        ("生成物の csum が合う", v["csum_ok"]),
        ("族タグ・版数が原本と同じ",
         v["header"]["family"] == hdr["family"] and v["header"]["version"] == hdr["version"]),
        ("h20/h30 が指定どおり" if (a.h20 or a.h30) else "h20/h30 が原本と同じ",
         v["header"]["h20"] == h20 and v["header"]["h30"] == h30),
        ("エントリの名前と順序が原本と同じ",
         [e["name"] for e in v["entries"]] == [e["name"] for e in r["entries"]]),
        ("差し替えなかった %d 点が原本とバイト一致" % len(untouched),
         all(got.get(n) == before[n] for n in untouched)),
    ]
    if replaced:
        checks.append(("差し替えた %d 点が渡したファイルと一致（%s）"
                       % (len(replaced), ", ".join(replaced)),
                       all(got.get(n) == want[n] for n in replaced)))
    print("\n## 自己検証（生成物をほどき直す）")
    for name, ok in checks:
        print("  %s %s" % ("✓" if ok else "✗", name))

    with open(a.outfile, "wb") as f:
        f.write(out)
    print("\n✓ %s" % os.path.abspath(a.outfile))
    if not all(ok for _, ok in checks):
        sys.exit(1)


def a_times(r):
    """中間 ZIP の日時は原本のペイロード先頭エントリに合わせる（純正もそうなっている）。"""
    return r["entries"][0]["times"] if r["entries"] else None


def _factory_entries(b, kernel, rootfs, label):
    """factory のペイロードに入れるもの: 渡された像・生成した write.sh・目印。

    像は機種の契約（uImage の形・squashfs の版・書き込み固定長）に合うものだけを通す。
    同梱しなかった区画は機器側で消去も書き込みもされない。
    """
    fac = b["factory"]
    entries, used = [], []
    for name, path in (("vmlinux.img", kernel), ("rootfs.img", rootfs)):
        if not path:
            continue
        im = _factory_image(b, name)
        if not im or not im.get("openird"):
            sys.exit("✗ %s はこの機種で OpenIRD が書く像ではありません。" % name)
        with open(path, "rb") as f:
            data = f.read()
        if len(data) > h(im["write_len"]):
            sys.exit("✗ %s: %dB > 書き込み固定長 %dB（超過分は黙って切り捨てられる）。"
                     % (name, len(data), h(im["write_len"])))
        if name == "vmlinux.img":
            u, spec = images.check_uimage(data), b["uimage"]
            if (u is None or u["name"] != spec["name"] or u["load"] != h(spec["load"])
                    or (u["os"], u["arch"], u["type"], u["comp"])
                    != (spec["os"], spec["arch"], spec["type"], spec["comp"])):
                sys.exit("✗ %s はこの機種のブートローダが食う uImage ではありません。" % path)
            _check_uimage_fits_decompress(spec, data, h(spec["load"]))
        else:
            got, want = images.identify_squashfs(data), b["squashfs"]["vendor_format"]
            if got is None or (got["major"], got["minor"], got["endian"]) != (
                    want["major"], want["minor"], want["endian"]):
                sys.exit("✗ %s はこの機種のカーネルが読む squashfs %d.%d %s ではありません。"
                         % (path, want["major"], want["minor"], want["endian"].upper()))
        entries.append((name, data))
        used.append({"name": name, "write_offset": h(im["write_offset"]),
                     "write_len": h(im["write_len"]), "erase_len": h(im["erase_len"])})
        print("  %-12s %8d B / %8d B (%d%%)" % (name, len(data), h(im["write_len"]),
                                              100 * len(data) // h(im["write_len"])))
    store = part_by_mtd(b, fac["update_store_mtd"])
    script = v1b.factory_write_script(
        b["model"], used, h(fac["staging"]),
        (h(store["offset"]), h(store["offset"]) + h(store["size"])),
        fac["update_dir"], fac["env_tool"], h(b["nvm"]["size"]) // 512)
    entries.append(("write.sh", script))
    entries.append((v1b.FACTORY_MARKER, ("OpenIRD %s for %s\n" % (label, b["model"])).encode("ascii")))
    return entries


def cmd_pack_factory(a):
    """OpenIRD 自身の kernel / rootfs から factory コンテナを作る。"""
    b = load_board(a.board)
    fac = b.get("factory", {})
    if fac.get("container") != "v1b" or "payload_name" not in fac:
        sys.exit("✗ %s の factory 像はまだ作れません（ボード定義に書き込み側の値が無い）。" % a.board)
    if not a.kernel and not a.rootfs:
        sys.exit("✗ --kernel / --rootfs の少なくとも一方が要ります。")
    try:
        version = tuple(int(x) for x in a.fw_version.split("."))
        if len(version) != 3 or not all(0 <= x <= 255 for x in version):
            raise ValueError
    except ValueError:
        sys.exit("✗ --fw-version は 0〜255 の三つ組（例 26.10.0）で渡してください。")
    entries = _factory_entries(b, a.kernel, a.rootfs, a.fw_version)
    payload = v1b.build_payload_zip(entries)
    if a.payload_only:
        # 鍵の要らない形: 機器の展開先（更新ファイルの置き場）にほどいて write.sh を
        # 実行すれば、コンテナ経由と同じ書き込みになる（開発・復旧用）
        with open(a.outfile, "wb") as f:
            f.write(payload)
        print("✓ payload ZIP（無暗号）: %d B → %s" % (len(payload), os.path.abspath(a.outfile)))
        return

    key, key_path = secrets.load_key(a.key_file)
    pw, pw_path = secrets.load_zip_password(a.zip_pw_file)
    secrets.report("AES 鍵       ", key, key_path)
    secrets.report("ZIP パスワード", pw, pw_path)
    mid = v1b.build_middle_zip(fac["payload_name"], payload, pw, salt=bytes(11))
    out = v1b.pack(fac["family_tag"], version, v1b.pad_to_block(mid), key)
    try:
        v = v1b.unpack(out, key, pw)
    except (ValueError, v1b.NotV1b) as e:
        sys.exit("✗ 生成物を読み直せません: %s" % e)
    got = {e["name"]: e["data"] for e in v["entries"]}
    ok = (v1b.sha1_ok(v["header"]) and v["csum_ok"] and v["header"]["family"] == fac["family_tag"]
          and [e["name"] for e in v["entries"]] == [n for n, _ in entries]
          and all(got[n] == d for n, d in entries))
    if not ok:
        sys.exit("✗ 内部エラー: 生成したコンテナが渡した中身と一致しません。")
    with open(a.outfile, "wb") as f:
        f.write(out)
    print("✓ factory: %s_UPDATE_v1b %s / %d B → %s"
          % (fac["family_tag"], a.fw_version, len(out), os.path.abspath(a.outfile)))


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

    p = sub.add_parser("pack-kernel",
                       help="生カーネルをボードの容器で包む（mb86h61: RDHB / emma3sep: uImage）")
    p.add_argument("board")
    p.add_argument("infile", help="非圧縮の生カーネル（arch/arm/boot/Image / vmlinux.bin）")
    p.add_argument("outfile")
    p.add_argument("--name", default=rdhb.NAME_DEFAULT, help="RDHB 名(既定=観察値)")
    p.add_argument("--load", help="load アドレス(既定=board の値)")
    p.add_argument("--entry", help="uImage の entry（System.map の kernel_entry。uImage のボードで必須）")
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

    p = sub.add_parser("unpack-factory", help="v1b コンテナを 3 段復号（要 鍵・ZIP パスワード）")
    p.add_argument("infile")
    p.add_argument("outdir", help="リポジトリ外か temp/ output/ build/ のどれか")
    p.add_argument("--key-file", help="AES 鍵ファイル（既定 ~/.openird/v1b/%s）" % secrets.KEY_FILE)
    p.add_argument("--zip-pw-file", help="ZIP パスワードファイル（既定 ~/.openird/v1b/%s）" % secrets.PW_FILE)
    p.set_defaults(fn=cmd_unpack_factory)

    p = sub.add_parser("repack-factory", help="純正コンテナを中身を変えずに組み直す")
    p.add_argument("infile")
    p.add_argument("outfile")
    p.add_argument("--rebuild", choices=["none", "zipcrypto", "all"], default="all",
                   help="どこまで自前で組むか: none=ヘッダと AES のみ / zipcrypto=中間 ZIP から / all=全段（既定）")
    p.add_argument("--plain-zip", action="store_true",
                   help="中間 ZIP をデータ記述子なしで作る（既定は純正と同じ bit3 あり）")
    p.add_argument("--salt", default="00" * 11,
                   help="ZipCrypto の 12B ヘッダに使う 11B（既定は固定＝再現ビルド）")
    p.add_argument("--random-salt", action="store_true", help="salt を毎回ランダムにする")
    p.add_argument("--replace", action="append", metavar="NAME=PATH",
                   help="ペイロードの 1 点を差し替える（繰り返し可。--rebuild all のときだけ）")
    p.add_argument("--h20", metavar="HEX32",
                   help="0x20 の 16B を明示指定（既定は原本から複写。仕様 §7 U3 の実験用）")
    p.add_argument("--h30", metavar="HEX8",
                   help="0x30 の 4B を明示指定（同上）")
    p.add_argument("--key-file", help="AES 鍵ファイル（既定 ~/.openird/v1b/%s）" % secrets.KEY_FILE)
    p.add_argument("--zip-pw-file", help="ZIP パスワードファイル（既定 ~/.openird/v1b/%s）" % secrets.PW_FILE)
    p.set_defaults(fn=cmd_repack_factory)

    p = sub.add_parser("pack-factory", help="OpenIRD の kernel / rootfs から v1b factory コンテナを作る")
    p.add_argument("board")
    p.add_argument("outfile")
    p.add_argument("--kernel", help="vmlinux.img（uImage）")
    p.add_argument("--rootfs", help="rootfs.img（squashfs）")
    p.add_argument("--fw-version", default="0.0.0",
                   help="ヘッダに入れる版数の三つ組（既定 0.0.0。機器は版数の大小を見ない）")
    p.add_argument("--payload-only", action="store_true",
                   help="暗号層を被せず、ペイロード ZIP だけを出す（鍵不要）")
    p.add_argument("--key-file", help="AES 鍵ファイル（既定 ~/.openird/v1b/%s）" % secrets.KEY_FILE)
    p.add_argument("--zip-pw-file", help="ZIP パスワードファイル（既定 ~/.openird/v1b/%s）" % secrets.PW_FILE)
    p.set_defaults(fn=cmd_pack_factory)

    p = sub.add_parser("full", help=".full コンテナ（HOLD）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_full)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
