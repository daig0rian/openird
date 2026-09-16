#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Daigorian
"""構造体・共用体のレイアウト抽出と比較（tests/kabi/run.sh から呼ぶ）。

  layout.py dump <readelf --debug-dump=info の出力>
      名前の付いた（または typedef された）構造体・共用体ごとに大きさとメンバ位置を出す。
      無名の入れ子は親に展開する。行形式: "S\t<kind name>\t<size>" / "M\t<kind name>\t<member>\t<offset>"
  layout.py compare <ref.layout> <new.layout> <allowed-additions.txt>
      ref にあるメンバが new で消えた・動いたら失敗。new だけにあるメンバは許可リストに
      あれば通し、無ければ失敗。終了コード 0 = 互換。
"""
import re
import sys

DIE = re.compile(r'^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: (\d+)(?: \((DW_TAG_\w+)\))?')
ATTR = re.compile(r'^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$')
AGG = {'DW_TAG_structure_type': 'struct', 'DW_TAG_union_type': 'union'}


def parse(path):
    dies, stack, cur = {}, [], None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = DIE.match(line)
            if m:
                level, off, abbrev = int(m.group(1)), int(m.group(2), 16), int(m.group(3))
                if abbrev == 0:
                    cur = None
                    continue
                while stack and stack[-1][0] >= level:
                    stack.pop()
                parent = stack[-1][1] if stack else None
                cur = {'tag': m.group(4), 'attrs': {}, 'children': []}
                dies[off] = cur
                if parent is not None:
                    dies[parent]['children'].append(off)
                stack.append((level, off))
                continue
            m = ATTR.match(line)
            if m and cur is not None:
                cur['attrs'][m.group(1)] = m.group(2).strip()
    return dies


def name(d):
    v = d['attrs'].get('DW_AT_name')
    return re.sub(r'^(?:\([^)]*\)\s*)+:\s*', '', v) if v else None


def num(v):
    m = re.search(r'DW_OP_plus_uconst: (\d+)', v)
    return int(m.group(1)) if m else int(v.split()[0], 0)


def ref(v):
    m = re.match(r'<0x([0-9a-f]+)>', v or '')
    return int(m.group(1), 16) if m else None


def dump(path):
    dies = parse(path)
    typedef_of = {}
    for d in dies.values():
        if d['tag'] == 'DW_TAG_typedef':
            t = ref(d['attrs'].get('DW_AT_type'))
            if t in dies and dies[t]['tag'] in AGG and not name(dies[t]):
                typedef_of.setdefault(t, name(d))

    def members(off, base, prefix, out):
        anon = 0
        for c in dies[off]['children']:
            cd = dies[c]
            if cd['tag'] != 'DW_TAG_member':
                continue
            a = cd['attrs']
            loc = base + num(a.get('DW_AT_data_member_location', '0'))
            nm = name(cd)
            t = ref(a.get('DW_AT_type'))
            td = dies.get(t)
            nested = td is not None and td['tag'] in AGG and not name(td) and t not in typedef_of
            if nm is None:
                anon += 1
                nm = '(anon%d)' % anon
            if 'DW_AT_bit_size' in a:
                bit = a.get('DW_AT_data_bit_offset', a.get('DW_AT_bit_offset', '?'))
                out.append((prefix + nm, '%d:%s/%s' % (loc, bit, a['DW_AT_bit_size'])))
            else:
                out.append((prefix + nm, str(loc)))
            if nested:
                members(t, loc, prefix + nm + '.', out)

    seen = {}
    for off, d in dies.items():
        kind = AGG.get(d['tag'])
        if not kind or 'DW_AT_declaration' in d['attrs'] or 'DW_AT_byte_size' not in d['attrs']:
            continue
        nm = name(d) or typedef_of.get(off)
        if not nm:
            continue
        key = '%s %s' % (kind, nm)
        seen[key] = seen.get(key, 0) + 1
        if seen[key] > 1:
            key += '#%d' % seen[key]
        print('S\t%s\t%d' % (key, num(d['attrs']['DW_AT_byte_size'])))
        out = []
        members(off, 0, '', out)
        for m, loc in out:
            print('M\t%s\t%s\t%s' % (key, m, loc))


def load(path):
    types = {}
    with open(path, encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[0] == 'S':
                types[p[1]] = {'size': int(p[2]), 'members': {}}
            elif p[0] == 'M':
                types[p[1]]['members'][p[2]] = p[3]
    return types


def compare(ref_path, new_path, allow_path):
    ref_t, new_t = load(ref_path), load(new_path)
    allowed = set()
    with open(allow_path, encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].split()
            if len(line) == 3:
                allowed.add(('%s %s' % (line[0], line[1]), line[2]))
    fail, note = [], []
    for key, r in sorted(ref_t.items()):
        n = new_t.get(key)
        if n is None:
            note.append('only in ref: %s' % key)
            continue
        for m, loc in r['members'].items():
            if m not in n['members']:
                fail.append('REMOVED  %s.%s (was @%s)' % (key, m, loc))
            elif n['members'][m] != loc:
                fail.append('MOVED    %s.%s @%s -> @%s' % (key, m, loc, n['members'][m]))
        added = [m for m in n['members'] if m not in r['members']]
        for m in added:
            line = '%s.%s @%s (size %d -> %d)' % (key, m, n['members'][m], r['size'], n['size'])
            (note if (key, m) in allowed else fail).append(
                ('ADDED    ' if (key, m) in allowed else 'ADDED(!) ') + line)
        if n['size'] != r['size'] and not added:
            fail.append('RESIZED  %s %d -> %d' % (key, r['size'], n['size']))
    new_only = [k for k in new_t if k not in ref_t]
    print('compared %d types (ref %d / new %d, new-only %d)'
          % (len(set(ref_t) & set(new_t)), len(ref_t), len(new_t), len(new_only)))
    for x in note:
        print('  note: ' + x)
    for x in fail:
        print('  FAIL: ' + x)
    return 1 if fail else 0


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'dump':
        dump(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == 'compare':
        sys.exit(compare(*sys.argv[2:]))
    else:
        sys.exit(__doc__)
