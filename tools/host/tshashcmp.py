#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tshashcmp - line up two per-packet hash lists of the same broadcast.

  tshashcmp.py <reference.hash[.gz]> <device.hash[.gz]> [PID ...]

Each list has one 'pid cc crc32' line per TS packet (tsref.py on the reference
host, `yb1-mem @file 0 <len> hashes` on the device).  Only hashes are compared
-- no payload is needed.  (docs/specs/poe-br100-yb1-reception-stack.md §76)

Per PID (default: every PID the device list has), the device's packets are
anchored at the first hash that occurs once in the reference, then walked in
order: a packet equal to the next reference packet is a bit-exact match; one
found a little further on means the reference packets in between are missing
from the device; one found nowhere is a packet the device got wrong.

Coverage: the time window is taken from the PID the device has most of (its
first and last matched positions in the reference); every PID the reference
carries inside that window is counted there and set against what the device
delivered, so PIDs the device never had show up too.
"""
import collections
import gzip
import sys


def load(path):
    op = gzip.open if path.endswith('.gz') else open
    out = []
    with op(path, 'rt') as f:
        for ln in f:
            p = ln.split()
            if len(p) == 3:
                out.append((int(p[0], 16), p[2]))
    return out


def walk(ref, dev_hashes, pid):
    """Returns (anchor, first, last, match, missing, wrong) or None; first/last
    are global indices into ref."""
    idx = [i for i, (p, _) in enumerate(ref) if p == pid]
    r = [ref[i][1] for i in idx]
    pos = collections.defaultdict(list)
    for i, h in enumerate(r):
        pos[h].append(i)
    a = next((k for k, h in enumerate(dev_hashes) if len(pos[h]) == 1), None)
    if a is None:
        return None
    j = first = pos[dev_hashes[a]][0]
    match = missing = wrong = 0
    last = j
    for h in dev_hashes[a:]:
        if j < len(r) and r[j] == h:
            match += 1
            last = j
            j += 1
            continue
        later = [q for q in pos.get(h, []) if j < q < j + 200]
        if later:
            missing += later[0] - j
            j = later[0] + 1
            last = later[0]
            match += 1
        else:
            wrong += 1
    return a, idx[first], idx[last], match, missing, wrong


def main():
    ref, dev = load(sys.argv[1]), load(sys.argv[2])
    by_pid = collections.defaultdict(list)
    for p, h in dev:
        by_pid[p].append(h)
    pids = [int(a, 0) for a in sys.argv[3:]] or sorted(by_pid)
    print('reference packets %d, device packets %d, device PIDs %d'
          % (len(ref), len(dev), len(by_pid)))

    res = {}
    for pid in pids:
        res[pid] = walk(ref, by_pid[pid], pid)
    print('\n  PID     device  bit-exact  missing  wrong   (anchor)')
    for pid in pids:
        w = res[pid]
        if w is None:
            print('  0x%04x %7d   -- no unique anchor (repeating identical packets) --'
                  % (pid, len(by_pid[pid])))
            continue
        a, _, _, m, miss, wrong = w
        print('  0x%04x %7d  %9d  %7d  %5d   #%d' % (pid, len(by_pid[pid]), m, miss, wrong, a))

    # coverage over the window of the busiest anchored PID
    anchored = [p for p in pids if res[p] is not None]
    if not anchored:
        return
    main_pid = max(anchored, key=lambda p: len(by_pid[p]))
    _, lo, hi, _, _, _ = res[main_pid]
    win = collections.Counter(p for p, _ in ref[lo:hi + 1])
    print('\ncoverage in the reference window of PID 0x%04x (%d reference packets):'
          % (main_pid, hi - lo + 1))
    print('  PID     reference  device   device/reference')
    tot_ref = tot_dev = 0
    for pid in sorted(set(win) | set(by_pid)):
        rc, dc = win.get(pid, 0), len(by_pid.get(pid, []))
        if pid != 0x1fff:
            tot_ref += rc
            tot_dev += dc
        note = '' if rc and dc else ('  <- device only' if dc else '  <- NOT DELIVERED')
        print('  0x%04x %9d %7d   %s%s' % (pid, rc, dc, ('%.3f' % (dc / rc)) if rc else '-', note))
    print('  non-null total: reference %d, device %d (%.3f); null in reference %d'
          % (tot_ref, tot_dev, tot_dev / tot_ref if tot_ref else 0, win.get(0x1fff, 0)))
    print('  (device counts cover its whole capture; the window is set by 0x%04x,'
          ' so PIDs can differ by the edges)' % main_pid)


if __name__ == '__main__':
    main()
