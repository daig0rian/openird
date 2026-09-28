#!/usr/bin/env python3
"""Condense a fapex-trace log into one line per operation, split at ### MARK lines.

usage: trsum.py LOG [--skip-tid N ...]
Output: per segment, the ordered operations of every thread except skipped ones,
with identical consecutive lines folded into "xN".  The vendor application's
110 ms LED / reset-switch poll (GPIO 73 68 17 16 13 14 and CLKPWR nr=0x05) is
left out whatever thread runs it.
"""
import re, sys, collections

args = sys.argv[1:]
log = args[0]
skip = set()
i = 1
while i < len(args):
    if args[i] == "--skip-tid": skip.add(args[i+1]); i += 2
    else: i += 1

BUS = {"fapi174": "I2C0", "fapi175": "I2C1", "fapi176": "I2C2", "fapi186": "GPIO", "fapi199": "CLKPWR"}
hexb = lambda s: bytes(int(x, 16) for x in s.split())
u32 = lambda b, o: int.from_bytes(b[o:o+4], "little")

def describe(line):
    m = re.match(r"(\S+) \[(\d+)\] (.*)", line)
    if not m: return None, None
    t, tid, rest = m.groups()
    if "[proxydiag]" in rest or "[proxy]" in rest: return tid, None
    if rest.startswith("open ") or rest.startswith("close "):
        if "/dev/fapi/fapi186" in rest: return tid, None       # GPIO is opened per operation
        return tid, rest
    m = re.match(r"ioctl /dev/fapi/(fapi\d+) nr=(0x[0-9a-f]+)(.*)", rest)
    if m:
        dev, nr, tail = m.groups(); name = BUS.get(dev, dev)
        nr = int(nr, 16)
        if name.startswith("I2C") and nr == 0x62:
            if "[pre-call]" in tail:
                mm = re.search(r"i2c_slave=(0x[0-9a-f]+) tx=\[\d+\]:([0-9a-f ]+)", tail)
                return tid, "%s W %s: %s" % (name, mm.group(1), mm.group(2).strip()) if mm else (tid, None)
            if "dir=R" in tail:
                mm = re.search(r"i2c_slave=(0x[0-9a-f]+) tx=(NULL|\[\d+\]:[0-9a-f ]+?) rx=\[\d+\]:([0-9a-f ]+?) = (-?\d+)", tail)
                if mm:
                    tx = "" if mm.group(2) == "NULL" else " tx " + mm.group(2).split(":", 1)[1].strip()
                    return tid, "%s R %s%s -> %s%s" % (name, mm.group(1), tx, mm.group(3).strip(),
                                                      "" if mm.group(4) == "0" else " ret=" + mm.group(4))
            mm = re.search(r"= (-?\d+)$", tail)
            if mm and mm.group(1) != "0": return tid, "%s W ret=%s" % (name, mm.group(1))
            return tid, None
        if name == "GPIO":
            mm = re.search(r"in=\[\d+\]:([0-9a-f ]+) =", tail)
            if nr == 0x40 and mm:
                b = hexb(mm.group(1)); return tid, "GPIO open pin=%d func=0x%08x" % (u32(b, 0), u32(b, 4))
            if nr == 0x41 and "dir=W" in tail and mm:
                return tid, "GPIO write %d" % u32(hexb(mm.group(1)), 0)
            mm = re.search(r"out=\[\d+\]:([0-9a-f ]+) =", tail)
            if nr == 0x41 and mm: return tid, "GPIO read -> %d" % u32(hexb(mm.group(1)), 0)
            return tid, "GPIO nr=0x%02x %s" % (nr, tail.strip()[:60])
        return tid, "%s nr=0x%02x %s" % (name, nr, re.sub(r"\s+", " ", tail.strip())[:110])
    m = re.match(r"ioctl /dev/h57fmb0 type=0xae nr=(0x[0-9a-f]+) dir=(\S+) size=(\d+) (in|out)=\[\d+\]:([0-9a-f ]+?)(?: val=\[4\]:([0-9a-f ]+))? = (-?\d+)", rest)
    if m:
        nr, d, size, io, data, val, ret = m.groups(); nr = int(nr, 16); b = hexb(data)
        r = "" if ret == "0" else " ret=" + ret
        if nr in (0x02, 0x03) and len(b) >= 12:
            v = " = 0x%04x" % int.from_bytes(hexb(val)[:2], "little") if val else ""
            return tid, "DIONE %s reg 0x%06x%s (flag %d)%s" % ("rd" if nr == 2 else "wr", u32(b, 4), v, u32(b, 0), r)
        return tid, "DIONE nr=0x%02x %s %s%s" % (nr, d, data.strip()[:60], r)
    m = re.match(r"write /dev/h57fmb0 n=(\d+) data=\[\d+\]:([0-9a-f ]+) = (-?\d+)", rest)
    if m: return tid, "DIONE write chunk"
    if rest.startswith("mmap "): return tid, rest
    return tid, None

POLL_PINS = {73, 68, 17, 16, 13, 14}
pin_of = {}
segs = [("boot", [])]
for line in open(log, errors="replace"):
    line = line.rstrip("\n")
    if line.startswith("### MARK"):
        segs.append((line[9:].split("|")[0].strip() + " | " + line.split("|")[1].strip(), [])); continue
    tid, d = describe(line)
    if d is None or tid in skip: continue
    if d.startswith("GPIO open pin="):
        pin_of[tid] = int(d.split("=")[1].split()[0])
    if d.startswith("GPIO") and pin_of.get(tid) in POLL_PINS: continue
    if d.startswith("CLKPWR nr=0x05"): continue
    segs[-1][1].append("[%s] %s" % (tid, d))

for name, ev in segs:
    print("=" * 20, name, "(%d ops)" % len(ev))
    prev, n = None, 0
    for e in ev + [None]:
        if e == prev: n += 1; continue
        if prev is not None: print(prev + ("   x%d" % n if n > 1 else ""))
        prev, n = e, 1
