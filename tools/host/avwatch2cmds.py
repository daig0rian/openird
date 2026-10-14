#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""avwatch2cmds - list the sub-CPU commands recorded in an avwatch log.

  avwatch2cmds.py <avwatch.log> [--t0 <uptime>] [--cmd <hex prefix>] [--max <words>]

avwatch (tools/yb1-fifodrain/avwatch.c, loaded with cmdlog=1) logs every change
of the command stack's entry table and copies each command's request and
response from the data ring.  Replaying the log gives the memory at any time;
a command is a change of an entry's word 1 (command ID << 16 | sequence), and
its request and response are read just before the entry is used again, 31
commands later, or at the end of the log.

For each command: the time (plus --t0, the uptime avwatch prints as its time
0), entry, command ID, sequence number, status, lengths and the words.  A
request the sub-CPU rewrote in place is shown twice: "req" as first seen and
"req'" as left behind.  --cmd 11 keeps the IDs starting with 11.  The last
line counts the commands by ID.

The entry layout is in docs/specs/poe-br100-yb1-av-command.md §2.
"""
import re
import struct
import sys

SHM = 0x10000
ENT = 0x10400
NENT = 32
LINE = re.compile(r"\s*(\d+) us\s+([0-9a-f]{8})\s+([0-9a-f]{8}) -> ([0-9a-f]{8})")


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    path = args.pop(0)
    t0 = 0.0
    prefix = None
    maxw = 24
    while args:
        a = args.pop(0)
        if a == "--t0":
            t0 = float(args.pop(0))
        elif a == "--cmd":
            prefix = args.pop(0).lower()
        elif a == "--max":
            maxw = int(args.pop(0))
        else:
            sys.exit("unknown option " + a)

    evs = []
    for ln in open(path, encoding="ascii", errors="replace"):
        m = LINE.match(ln)
        if not m:
            continue
        us, addr, new = int(m[1]), int(m[2], 16), int(m[4], 16)
        if SHM <= addr < SHM + 0x10000:
            evs.append((us, addr, new))

    # the commands, and the event at which each entry is used again
    cmds = []  # (us, entry, word 1, index of the event)
    for k, (us, addr, new) in enumerate(evs):
        if us and ENT <= addr < ENT + 32 * NENT and (addr - ENT) % 32 == 4 and new:
            cmds.append((us, (addr - ENT) // 32, new, k))
    at = {}
    for i, c in enumerate(cmds):
        nxt = len(evs)
        for d in cmds[i + 1:]:
            if d[1] == c[1]:
                nxt = d[3]
                break
        at.setdefault(nxt, []).append(i)

    writes = {}  # address -> [(event index, value)]
    for k, (_, addr, new) in enumerate(evs):
        writes.setdefault(addr, []).append((k, new))

    def first_after(a, k0):
        for k, v in writes.get(a & 0x1fffffff, ()):
            if k > k0:
                return v
        return None

    mem = {}

    def rd(a):
        return mem.get(a & 0x1fffffff, 0)

    out = {}
    for k in range(len(evs) + 1):
        for i in at.get(k, ()):
            us, ent, w1, k0 = cmds[i]
            e = ENT + 32 * ent
            w2, w3, st, w6 = rd(e + 8), rd(e + 12), rd(e + 20), rd(e + 24)
            req = req0 = rs = None
            if 0 <= w3 - w2 <= 4096:
                req = [rd(a) for a in range(w2, w3, 4)]
                req0 = [first_after(a, k0) for a in range(w2, w3, 4)]
                req0 = [r if r is not None else q for r, q in zip(req0, req)]
            if 0 <= w2 + w6 - w3 <= 4096:
                rs = [rd(a) for a in range(w3, w2 + w6, 4)]
            out[i] = (w2, w3, st, w6, req0, req, rs)
        if k < len(evs):
            mem[evs[k][1]] = evs[k][2]

    prev = None
    count = {}
    for i, (us, ent, w1, _) in enumerate(cmds):
        w2, w3, st, w6, req0, req, rs = out[i]
        cmd, seq = w1 >> 16, w1 & 0xffff
        count[cmd] = count.get(cmd, 0) + 1
        gap = "" if prev is None or seq == ((prev + 1) & 0xffff) else "  [not the next sequence number]"
        prev = seq
        if prefix and not ("%04x" % cmd).startswith(prefix):
            continue
        print("%9.3f  e%-2d cmd %04x seq %04x st %-3d req %3d resp %3d @%08x%s" % (
            t0 + us / 1e6, ent, cmd, seq, struct.unpack(">i", struct.pack(">I", st))[0],
            w3 - w2, w6 - (w3 - w2), w2, gap))
        rows = [("req ", req0), ("req'", req if req != req0 else None), ("resp", rs)]
        for label, ws in rows:
            if not ws:
                continue
            for j in range(0, min(len(ws), maxw), 8):
                print("      %s %s" % (label if j == 0 else "    ",
                                       " ".join("%08x" % w for w in ws[j:j + 8])))
            if len(ws) > maxw:
                print("           ... (%d words)" % len(ws))
    print("# %d commands; by ID: %s" % (
        len(cmds), " ".join("%04x:%d" % kv for kv in sorted(count.items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
