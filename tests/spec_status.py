#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 OpenIRD contributors
"""docs/specs/*.md のヘッダ宣言を、テストの実測と突き合わせて検査する。

**縛るのは片側だけ。** 「仕様が新しく実装が古い」は clean-room（観察 → 仕様化 →
実装）の正常な途中経過なので妨げない。落とすのは次の 2 方向だけ:

  - 実装していないのに `implemented` / `verified` と宣言している（過大申告）
  - 実装してテストまで書いたのに `specified` のまま（宣言の更新漏れ）

∴ 仕様だけのコミット（`Origin: observed`）は何も妨げられない。一方で Status は
書き換えを忘れると落ちるので、「目安」ではなく検証される宣言になる。

ヘッダは散文ではなく厳密な機械可読ブロック。テストからの引用も文字列の解析では
なく `check(..., spec="<slug>#<節>")` という構造化された引数で受ける。
"""
import os
import re

STATUSES = {
    "observing":   "観察中。仕様は未完成",
    "specified":   "仕様化済み・未実装（clean-room の正常な途中経過）",
    "implemented": "実装済み・実機未検証",
    "verified":    "実装済み・実機検証済み",
    "reference":   "実装対象ではない観察記録",
    "frozen":      "対象が消えた／保留",
}
NEEDS_IMPL = ("implemented", "verified")     # 実装とテストの存在を要求する
FORBIDS_IMPL = ("specified",)                # テストに引かれていてはいけない
REQUIRED_KEYS = ("Status", "As-of", "Impl", "Tests")

_BLOCK = re.compile(r"<!--\s*openird-spec\s*\n(.*?)-->", re.S)
_SECTION = re.compile(r"^##+\s+(?:§\s*)?([0-9]+(?:\.[0-9]+)*)[.．]?\s+(.*)$", re.M)


def parse_header(text):
    """先頭の openird-spec ブロックを読む。無ければ None。"""
    m = _BLOCK.search(text)
    if not m:
        return None
    hdr = {}
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if ":" not in line:
            return {"_error": "key: value 形式でない行: %r" % line}
        k, v = line.split(":", 1)
        hdr[k.strip()] = v.strip()
    return hdr


def sections(text):
    """`## N. 見出し` を (番号, 見出し) で拾う。"""
    return _SECTION.findall(text)


def _paths(value):
    if not value or value in ("-", "なし", "none"):
        return []
    return [p.strip() for p in value.split(",") if p.strip()]


def check_specs(root, cited, report=print):
    """docs/specs を検査する。cited は "<slug>#<節>" の集合。

    戻り値は失敗した項目名の並び（空なら全通過）。
    """
    specs_dir = os.path.join(root, "docs", "specs")
    fails = []

    def ck(name, cond, detail=""):
        report("  %s %s%s" % ("✓" if cond else "✗", name,
                              ("  — " + detail) if detail and not cond else ""))
        if not cond:
            fails.append(name)
        return bool(cond)

    cited_slugs = {c.split("#", 1)[0] for c in cited}
    for fn in sorted(os.listdir(specs_dir)):
        if not fn.endswith(".md") or fn == "README.md":
            continue
        slug = fn[:-3]
        with open(os.path.join(specs_dir, fn), encoding="utf-8") as f:
            text = f.read()
        hdr = parse_header(text)
        report("[spec] %s" % slug)
        if hdr is None:
            ck("%s: openird-spec ヘッダがある" % slug, False,
               "付け忘れ。Status/As-of/Impl/Tests を宣言すること")
            continue
        if "_error" in hdr:
            ck("%s: ヘッダが読める" % slug, False, hdr["_error"])
            continue
        missing = [k for k in REQUIRED_KEYS if k not in hdr]
        if not ck("%s: 必須キーが揃っている" % slug, not missing, "欠け: %s" % ", ".join(missing)):
            continue
        st = hdr["Status"]
        if not ck("%s: Status が既知の値" % slug, st in STATUSES, "得: %r" % st):
            continue

        mine = sorted(c for c in cited if c.split("#", 1)[0] == slug)
        if st in NEEDS_IMPL:
            for p in _paths(hdr["Impl"]):
                ck("%s: Impl の %s が実在する" % (slug, p),
                   os.path.exists(os.path.join(root, p)))
            ck("%s: Status=%s を裏づけるテストがある" % (slug, st),
               slug in cited_slugs,
               "この仕様を spec= で引くテストが 1 つも無い＝過大申告の疑い")
        elif st in FORBIDS_IMPL:
            ck("%s: Status=specified と実測が矛盾しない" % slug,
               slug not in cited_slugs,
               "テストが引いている（%s）＝実装済みなのに Status が古い" % ", ".join(mine))
        else:
            report("    （%s。実装・テストは要求しない）" % STATUSES[st])

        # 節ごとの被引用は報告のみ。テストの書きようが無い節（鍵の扱い・未解明点など）が
        # あるため強制しない。埋めるべき穴は一覧で見える。
        secs = sections(text)
        if secs and st in NEEDS_IMPL:
            got = {c.split("#", 1)[1] for c in mine if "#" in c}
            have = [n for n, _ in secs if n in got]
            lack = [n for n, _ in secs if n not in got]
            report("    節の被引用: %d/%d  引かれている §%s"
                   % (len(have), len(secs), ",".join(have) if have else "-"))
            if lack:
                report("    引かれていない §%s（報告のみ）" % ",".join(lack))
    return fails
