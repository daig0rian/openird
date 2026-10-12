# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""OpenIRD の更新イメージ（sysupgrade）を作る・読む。形式は docs/sysupgrade.md。

    sysupgrade-<board>/CONTROL   誰のための何か（key=value）と、各ファイルの SHA-256
    sysupgrade-<board>/<name>    機種が書くファイル（emma3sep なら kernel と root）

無圧縮の tar（中のイメージが既に圧縮されている）。同じ入力からは同じバイト列になる。
機器の sysupgrade（br-ext-common/package/ird-base-files）が読む。鍵は関わらない。
"""
import hashlib
import io
import re
import tarfile

PREFIX = "sysupgrade-"
_NAME = re.compile(r"^[A-Za-z0-9_.-]+$")


def control(board, version, compat, files):
    """CONTROL の中身。files は [(名前, バイト列)]（書く順）。"""
    lines = ["BOARD=%s" % board, "VERSION=%s" % version, "COMPAT=%d" % compat,
             "FILES=%s" % " ".join(n for n, _ in files)]
    lines += ["SHA256_%s=%s" % (n, hashlib.sha256(d).hexdigest()) for n, d in files]
    return ("\n".join(lines) + "\n").encode("ascii")


def build(board, version, compat, files):
    """更新イメージ（tar）を返す。"""
    for s in (board, version):
        if not _NAME.match(s):
            raise ValueError("使えない文字がある: %r" % s)
    for n, _ in files:
        if not _NAME.match(n) or n == "CONTROL":
            raise ValueError("使えないファイル名: %r" % n)
    top = PREFIX + board
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
        d = tarfile.TarInfo(top)
        d.type, d.mode = tarfile.DIRTYPE, 0o755
        t.addfile(d)
        for n, data in [("CONTROL", control(board, version, compat, files))] + list(files):
            ti = tarfile.TarInfo("%s/%s" % (top, n))
            ti.size, ti.mode = len(data), 0o644
            t.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def read(data):
    """更新イメージを読み、{board, control, files, problems} を返す。

    problems が空なら、機器の sysupgrade が形として受け取るもの（機種ごとの検査は別）。
    """
    out = {"board": None, "control": {}, "files": {}, "problems": []}
    bad = out["problems"].append
    try:
        t = tarfile.open(fileobj=io.BytesIO(data), mode="r:")
    except tarfile.TarError as e:
        bad("tar として読めない: %s" % e)
        return out
    tops = set()
    with t:
        for m in t.getmembers():
            name = m.name.rstrip("/")
            top, _, rest = name.partition("/")
            tops.add(top)
            if not top.startswith(PREFIX) or "/" in rest or (rest and not _NAME.match(rest)):
                bad("想定外のメンバ: %s" % m.name)
            elif not rest:
                if not m.isdir():
                    bad("%s がディレクトリでない" % m.name)
            elif not m.isreg():
                bad("%s が通常のファイルでない" % m.name)
            else:
                out["files"][rest] = t.extractfile(m).read()
    if len(tops) != 1:
        bad("sysupgrade-<board> のディレクトリが 1 つでない: %s" % sorted(tops))
        return out
    top = tops.pop()
    ctl = out["files"].pop("CONTROL", None)
    if ctl is None:
        bad("CONTROL が無い")
        return out
    for line in ctl.decode("ascii", "replace").splitlines():
        k, sep, v = line.partition("=")
        if sep and k not in out["control"]:
            out["control"][k] = v
    c = out["control"]
    out["board"] = c.get("BOARD")
    if top != PREFIX + (c.get("BOARD") or ""):
        bad("CONTROL の BOARD とディレクトリ名が合わない")
    listed = (c.get("FILES") or "").split()
    if not listed:
        bad("CONTROL に FILES が無い")
    for n in listed:
        if n not in out["files"]:
            bad("%s が無い" % n)
        elif c.get("SHA256_" + n) != hashlib.sha256(out["files"][n]).hexdigest():
            bad("%s の SHA-256 が合わない" % n)
    for n in out["files"]:
        if n not in listed:
            bad("FILES に無いファイル: %s" % n)
    return out
