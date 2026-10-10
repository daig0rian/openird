#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""ird-mkaddon - OpenIRD のアドオン（.ird）と、アドオンリポジトリの索引（Packages）を作る。

形式の定義は docs/addon.md（§2 アドオン、§3 索引）。標準ライブラリだけで動く単一
ファイルで、SDK（make sdk）には bin/ird-mkaddon として入る。

  ird-mkaddon build <staging> [-o <outdir>] [--arch <id>]
      staging ディレクトリ（manifest・root/・任意で postinstall / preremove）から
      <Package>_<Version>_<Arch>.ird を作る。
  ird-mkaddon index <dir>
      <dir> の *.ird を調べて、索引 <dir>/Packages を書く。

同じ入力からは同じバイト列が出る（並びは名前順、所有者は root、時刻は
SOURCE_DATE_EPOCH か 0）。sha256 を pin する仕組みなので、作り直すたびに
ハッシュが変わらないようにしてある。
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import stat
import sys
import tarfile

for _s in (sys.stdout, sys.stderr):            # Windows cp932 既定を回避
    if hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8")

# manifest のキー（この順で書き出す）
MANIFEST_KEYS = ("Package", "Version", "Arch", "Requires-OpenIRD", "Depends", "Description")
REQUIRED_KEYS = ("Package", "Version", "Arch")
# 場所だけ決めてある予約キー。意味が決まるまで、値を持つアドオンは作らせない
# （機器側の ird-addon も入れない＝あとで意味を与えても古い base が黙って無視しない）。
RESERVED_KEYS = ("Requires-OpenIRD", "Depends")
SCRIPTS = ("postinstall", "preremove")

# アドオンが置けない場所。機器側（ird-addon の DENY）と同じ表:
# 揮発する／仮想のツリー、union の 2 層そのもの、インストーラ自身の記録。
FORBIDDEN_DIRS = ("dev", "proc", "sys", "tmp", "run", "rom", "overlay", "var/lib/ird-addon")

# ファイル名 <Package>_<Version>_<Arch>.ird の区切りが "_" なので、3 つとも "_" を含めない
_PACKAGE = re.compile(r"[a-z0-9][a-z0-9.+-]*\Z")
_VERSION = re.compile(r"[A-Za-z0-9][A-Za-z0-9.+~-]*\Z")
_ARCH = re.compile(r"[a-z0-9][a-z0-9-]*\Z")


class Error(Exception):
    pass


def _printable(s):
    return all(0x20 <= ord(c) <= 0x7e for c in s)


def parse_manifest(text, where):
    """`Key: value` の並びを読む。知らないキー・重複・ASCII 以外は誤りにする。"""
    m = {}
    for n, line in enumerate(text.replace("\r", "").split("\n"), 1):
        if not line.strip():
            continue
        if ":" not in line:
            raise Error("%s:%d: 'Key: value' の形でない: %r" % (where, n, line))
        k, v = line.split(":", 1)
        k, v = k.strip(), v.strip()
        if k not in MANIFEST_KEYS:
            raise Error("%s:%d: 知らないキー %r（使えるのは %s）"
                        % (where, n, k, ", ".join(MANIFEST_KEYS)))
        if k in m:
            raise Error("%s:%d: %s が 2 回ある" % (where, n, k))
        if not _printable(v):
            raise Error("%s:%d: %s の値に ASCII の印字文字でないものがある"
                        "（機器のシリアルコンソールに出る文字列なので ASCII に限る）" % (where, n, k))
        m[k] = v
    return m


def validate_manifest(m, where):
    for k in REQUIRED_KEYS:
        if not m.get(k):
            raise Error("%s: %s が無い" % (where, k))
    for k, rx, hint in (("Package", _PACKAGE, "英小文字・数字で始まり、英小文字・数字・ . + - だけ"),
                        ("Version", _VERSION, "英数字で始まり、英数字・ . + ~ - だけ"),
                        ("Arch", _ARCH, "英小文字・数字で始まり、英小文字・数字・ - だけ")):
        if not rx.match(m[k]):
            raise Error("%s: %s=%r は使えない（%s）" % (where, k, m[k], hint))
    for k in RESERVED_KEYS:
        if m.get(k):
            raise Error("%s: %s は予約キーで、まだ意味が決まっていない（docs/addon.md §2）。"
                        "アドオンは自己完結にして、この行を外すこと" % (where, k))


def format_manifest(m):
    return "".join("%s: %s\n" % (k, m[k]) for k in MANIFEST_KEYS if m.get(k)).encode("ascii")


def check_target_path(rel, where):
    """root/ より下の相対パス（例 usr/bin/recpt1）が、機器に置いてよい名前か。"""
    if not _printable(rel):
        raise Error("%s: パスに ASCII の印字文字でないものがある: %r" % (where, rel))
    if any(p in ("", ".", "..") for p in rel.split("/")):
        raise Error("%s: パスが正規の形でない: %r" % (where, rel))
    for d in FORBIDDEN_DIRS:
        if rel == d or rel.startswith(d + "/"):
            raise Error("%s: /%s の下にはアドオンを置けない: %s" % (where, d, rel))


def _walk_root(root):
    """root/ を名前順にたどり (相対パス, 種別, 実パス, lstat) を返す。"""
    out = []

    def rec(d, rel):
        for name in sorted(os.listdir(d)):
            full = os.path.join(d, name)
            r = rel + "/" + name if rel else name
            st = os.lstat(full)
            if stat.S_ISLNK(st.st_mode):
                out.append((r, "link", full, st))
            elif stat.S_ISDIR(st.st_mode):
                out.append((r, "dir", full, st))
                rec(full, r)
            elif stat.S_ISREG(st.st_mode):
                out.append((r, "file", full, st))
            else:
                raise Error("root/%s: ファイル・ディレクトリ・symlink 以外は入れられない" % r)

    rec(root, "")
    return out


def _tarinfo(name, kind, mode, mtime, size=0, linkname=""):
    ti = tarfile.TarInfo(name)
    ti.type = {"file": tarfile.REGTYPE, "dir": tarfile.DIRTYPE, "link": tarfile.SYMTYPE}[kind]
    ti.mode = mode
    ti.size = size
    ti.mtime = mtime
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    ti.linkname = linkname
    return ti


def build(staging, outdir, arch=None):
    """staging から .ird を作り (出力パス, sha256, サイズ, manifest) を返す。"""
    if os.name == "nt":
        raise Error("build は Linux/WSL で実行すること（Windows では実行ビットと symlink を"
                    "表せないので、機器で動かないアドオンができる）")
    if not os.path.isdir(staging):
        raise Error("%s: staging ディレクトリが無い" % staging)
    extra = sorted(set(os.listdir(staging)) - {"manifest", "root"} - set(SCRIPTS))
    if extra:
        raise Error("%s: staging に置けるのは manifest・root/・postinstall・preremove だけ"
                    "（余計なもの: %s）" % (staging, ", ".join(extra)))

    mpath = os.path.join(staging, "manifest")
    if not os.path.isfile(mpath):
        raise Error("%s: manifest が無い" % staging)
    with open(mpath, "rb") as f:
        try:
            text = f.read().decode("ascii")
        except UnicodeDecodeError:
            raise Error("%s: ASCII でない" % mpath)
    m = parse_manifest(text, mpath)
    if arch:
        if m.get("Arch") and m["Arch"] != arch:
            raise Error("%s: Arch=%s だが --arch %s が指定された" % (mpath, m["Arch"], arch))
        m["Arch"] = arch
    elif not m.get("Arch") and os.environ.get("IRD_ARCH"):
        m["Arch"] = os.environ["IRD_ARCH"]          # SDK の sdk.env が与える id
    validate_manifest(m, mpath)

    try:
        mtime = int(os.environ.get("SOURCE_DATE_EPOCH", "0"))
    except ValueError:
        raise Error("SOURCE_DATE_EPOCH が整数でない")

    members = [(_tarinfo("manifest", "file", 0o644, mtime), format_manifest(m))]
    for s in SCRIPTS:
        p = os.path.join(staging, s)
        if not os.path.lexists(p):
            continue
        if os.path.islink(p) or not os.path.isfile(p):
            raise Error("%s: 通常のファイルでない" % p)
        with open(p, "rb") as f:
            data = f.read()
        if b"\r" in data:
            raise Error("%s: 改行が CRLF（機器の sh が読めない）" % p)
        members.append((_tarinfo(s, "file", 0o755, mtime), data))

    root = os.path.join(staging, "root")
    if os.path.islink(root) or not os.path.isdir(root):
        raise Error("%s: root/ ディレクトリが無い" % staging)
    tree = _walk_root(root)
    if not any(kind != "dir" for _, kind, _, _ in tree):
        raise Error("%s: 中にファイルが 1 つも無い" % root)
    members.append((_tarinfo("root", "dir", 0o755, mtime), None))
    for rel, kind, full, st in tree:
        check_target_path(rel, "root/" + rel)
        name = "root/" + rel
        if kind == "dir":
            members.append((_tarinfo(name, "dir", 0o755, mtime), None))
        elif kind == "link":
            target = os.readlink(full)
            if not target or not _printable(target):
                raise Error("%s: symlink の先に ASCII の印字文字でないものがある" % name)
            members.append((_tarinfo(name, "link", 0o777, mtime, linkname=target), None))
        else:
            with open(full, "rb") as f:
                data = f.read()
            # モードは 2 通りに正規化する（umask や checkout 先のファイルシステムに左右されない）
            mode = 0o755 if st.st_mode & 0o100 else 0o644
            members.append((_tarinfo(name, "file", mode, mtime), data))

    # ustar: BusyBox の tar がそのまま読める最小の形。名前が長すぎれば ValueError になる
    raw = io.BytesIO()
    try:
        with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tf:
            for ti, data in members:
                ti.size = len(data) if data is not None else 0
                tf.addfile(ti, io.BytesIO(data) if data is not None else None)
    except ValueError as e:
        raise Error("tar に入らない名前がある（ustar の長さ制限）: %s" % e)
    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, compresslevel=9, mtime=0) as gz:
        gz.write(raw.getvalue())
    blob = out.getvalue()

    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, "%s_%s_%s.ird" % (m["Package"], m["Version"], m["Arch"]))
    with open(path, "wb") as f:
        f.write(blob)
    return path, hashlib.sha256(blob).hexdigest(), len(blob), m


def inspect(path):
    """.ird を開いて manifest を返す。機器側が拒む形（余計なメンバ・不正なパス）も調べる。"""
    manifest = None
    files = 0
    try:
        with tarfile.open(path, "r:gz") as tf:
            for ti in tf:
                name = ti.name[2:] if ti.name.startswith("./") else ti.name
                name = name.rstrip("/")
                if name in ("", "."):
                    continue
                if name == "manifest" or name in SCRIPTS:
                    if not ti.isreg():
                        raise Error("%s: %s が通常のファイルでない" % (path, name))
                    if name == "manifest":
                        manifest = tf.extractfile(ti).read()
                elif name == "root":
                    if not ti.isdir():
                        raise Error("%s: root がディレクトリでない" % path)
                elif name.startswith("root/"):
                    if not (ti.isreg() or ti.isdir() or ti.issym()):
                        raise Error("%s: %s はファイル・ディレクトリ・symlink のどれでもない"
                                    % (path, name))
                    check_target_path(name[5:], path)
                    files += 0 if ti.isdir() else 1
                else:
                    raise Error("%s: 余計なメンバがある: %s" % (path, ti.name))
    except (tarfile.TarError, OSError, EOFError) as e:
        raise Error("%s: gzip 圧縮の tar として読めない（%s）" % (path, e))
    if manifest is None:
        raise Error("%s: manifest が無い" % path)
    if not files:
        raise Error("%s: root/ にファイルが 1 つも無い" % path)
    try:
        m = parse_manifest(manifest.decode("ascii"), path + ":manifest")
    except UnicodeDecodeError:
        raise Error("%s: manifest が ASCII でない" % path)
    validate_manifest(m, path + ":manifest")
    return m


def index(repo):
    """repo の *.ird から索引 Packages を作り (索引のパス, スタンザ数) を返す。"""
    if not os.path.isdir(repo):
        raise Error("%s: ディレクトリが無い" % repo)
    names = sorted(n for n in os.listdir(repo) if n.endswith(".ird"))
    if not names:
        raise Error("%s: *.ird が 1 つも無い" % repo)
    entries = {}
    for n in names:
        path = os.path.join(repo, n)
        m = inspect(path)
        want = "%s_%s_%s.ird" % (m["Package"], m["Version"], m["Arch"])
        if n != want:
            raise Error("%s: ファイル名が manifest と合わない（%s のはず）" % (path, want))
        key = (m["Package"], m["Arch"])
        if key in entries:
            # 版に順序は無い（一意であればよい）ので、機器はどちらが新しいか決められない
            raise Error("%s: %s (%s) が 2 つある（%s と %s）。1 つのリポジトリに置けるのは "
                        "Package と Arch の組ごとに 1 版だけ"
                        % (repo, m["Package"], m["Arch"], entries[key][0], n))
        with open(path, "rb") as f:
            blob = f.read()
        entries[key] = (n, m, hashlib.sha256(blob).hexdigest(), len(blob))

    out = []
    for key in sorted(entries):
        n, m, sha, size = entries[key]
        lines = ["Package: " + m["Package"], "Version: " + m["Version"], "Arch: " + m["Arch"],
                 "Filename: " + n, "SHA256: " + sha, "Size: %d" % size]
        if m.get("Description"):
            lines.append("Description: " + m["Description"])
        out.append("\n".join(lines) + "\n")
    path = os.path.join(repo, "Packages")
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out))
    return path, len(out)


def main():
    ap = argparse.ArgumentParser(
        prog="ird-mkaddon",
        description="OpenIRD のアドオン（.ird）とアドオンリポジトリの索引を作る（docs/addon.md）")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="staging ディレクトリから .ird を作る")
    b.add_argument("staging", help="manifest と root/ を持つディレクトリ")
    b.add_argument("-o", "--outdir", default=".", help="出力先（既定: カレント）")
    b.add_argument("--arch", help="manifest の Arch（省くと manifest の値、無ければ $IRD_ARCH）")
    i = sub.add_parser("index", help="ディレクトリの *.ird から索引 Packages を作る")
    i.add_argument("dir", help="アドオンリポジトリにするディレクトリ")
    a = ap.parse_args()

    try:
        if a.cmd == "build":
            path, sha, size, m = build(a.staging, a.outdir, a.arch)
            print("✓ %s" % path)
            print("  Package: %s  Version: %s  Arch: %s" % (m["Package"], m["Version"], m["Arch"]))
            print("  Size:    %d" % size)
            print("  SHA256:  %s" % sha)
        else:
            path, n = index(a.dir)
            print("✓ %s（%d 個）" % (path, n))
    except Error as e:
        sys.exit("✗ %s" % e)


if __name__ == "__main__":
    main()
