#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""tshashcmp - line up two per-packet hash lists of the same broadcast.

  tshashcmp.py <reference.hash[.gz]> <device.hash[.gz]> [PID ...]

Each list has one 'pid cc crc32' line per TS packet (tsref.py on the reference
host, `yb1-mem @file 0 <len> hashes` on the device).  For each PID (default
0x0100), the device's packets are anchored at the first hash that occurs once
in the reference, then walked in order: a packet equal to the next reference
packet is a bit-exact match; one found a little further on means the reference
packets in between are missing from the device; one found nowhere is a packet
the device got wrong.  Only hashes are compared -- no payload is needed.
(docs/specs/poe-br100-yb1-reception-stack.md §76)
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


def main():
    ref, dev = load(sys.argv[1]), load(sys.argv[2])
    pids = [int(a, 0) for a in sys.argv[3:]] or [0x0100]
    print('reference packets %d, device packets %d' % (len(ref), len(dev)))
    for pid in pids:
        r = [h for p, h in ref if p == pid]
        d = [h for p, h in dev if p == pid]
        pos = collections.defaultdict(list)
        for i, h in enumerate(r):
            pos[h].append(i)
        a = next((k for k, h in enumerate(d) if len(pos[h]) == 1), None)
        if a is None:
            print('PID 0x%04x: device %d packets, no unique anchor '
                  '(not in the reference, or identical packets repeat)' % (pid, len(d)))
            continue
        j = first = pos[d[a]][0]
        match = missing = wrong = 0
        for h in d[a:]:
            if j < len(r) and r[j] == h:
                match += 1
                j += 1
                continue
            later = [q for q in pos.get(h, []) if j < q < j + 200]
            if later:
                missing += later[0] - j
                j = later[0] + 1
                match += 1
            else:
                wrong += 1
        print('PID 0x%04x: device %d packets (anchored at #%d), reference window %d'
              % (pid, len(d), a, j - first))
        print('   bit-exact %d | missing from device %d | device-only (wrong) %d'
              % (match, missing, wrong))


if __name__ == '__main__':
    main()
